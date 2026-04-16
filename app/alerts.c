#include "alerts.h"
#include "vapix.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>

/* Per-rule active state across polls.  Index matches AlertMap rules[]. */
static int g_prev_active[ALERTS_MAX_TYPES] = { 0 };
static int g_any_active = 0;

/* ── Parser ─────────────────────────────────────────────────────────────── */

static void str_trim(char *s) {
    if (!s) return;
    char *start = s;
    while (*start == ' ' || *start == '\t') start++;
    if (start != s) memmove(s, start, strlen(start) + 1);
    size_t n = strlen(s);
    while (n > 0 && (s[n-1] == ' ' || s[n-1] == '\t' || s[n-1] == '\r' || s[n-1] == '\n'))
        s[--n] = '\0';
}

void alerts_map_parse(const char *mapstr, AlertMap *out) {
    memset(out, 0, sizeof(*out));
    if (!mapstr || !*mapstr) return;

    char buf[4096];
    snprintf(buf, sizeof(buf), "%s", mapstr);

    char *save = NULL;
    char *tok  = strtok_r(buf, "|", &save);
    while (tok && out->count < ALERTS_MAX_TYPES) {
        /* tok = "Type:Port:Enabled" */
        char *colon1 = strchr(tok, ':');
        if (!colon1) { tok = strtok_r(NULL, "|", &save); continue; }
        *colon1 = '\0';
        char *rest = colon1 + 1;
        char *colon2 = strchr(rest, ':');
        if (!colon2) { tok = strtok_r(NULL, "|", &save); continue; }
        *colon2 = '\0';

        AlertRule *r = &out->rules[out->count];
        snprintf(r->type, sizeof(r->type), "%s", tok);
        str_trim(r->type);
        r->port    = atoi(rest);
        r->enabled = atoi(colon2 + 1) ? 1 : 0;
        if (*r->type && r->port > 0) out->count++;

        tok = strtok_r(NULL, "|", &save);
    }
}

/* ── Alert matching ─────────────────────────────────────────────────────── */

/* For a given rule, is there a matching active alert in the snapshot? */
static int find_matching_alert(const AlertRule *r, const WeatherSnapshot *snap,
                               const char **headline_out) {
    for (int i = 0; i < snap->alerts.count; i++) {
        if (strcasecmp(snap->alerts.alerts[i].event, r->type) == 0) {
            if (headline_out) *headline_out = snap->alerts.alerts[i].headline;
            return 1;
        }
    }
    if (headline_out) *headline_out = "";
    return 0;
}

/* ── Public API ─────────────────────────────────────────────────────────── */

void alerts_process(const WeatherSnapshot *snap,
                    const AlertMap *map,
                    const char *vapix_user,
                    const char *vapix_pass,
                    alerts_transition_cb cb,
                    void *cb_user) {
    int any = 0;

    for (int i = 0; i < map->count; i++) {
        const AlertRule *r = &map->rules[i];
        if (!r->enabled) {
            /* If previously active on this slot, clear it so we don't leak state. */
            if (g_prev_active[i]) {
                vapix_port_set(r->port, 0, vapix_user, vapix_pass);
                g_prev_active[i] = 0;
            }
            continue;
        }

        const char *headline = "";
        int active = find_matching_alert(r, snap, &headline);

        if (active && !g_prev_active[i]) {
            syslog(LOG_WARNING, "alerts: ACTIVE %s → port %d", r->type, r->port);
            long code = vapix_port_set(r->port, 1, vapix_user, vapix_pass);
            if (code != 200)
                syslog(LOG_WARNING, "alerts: port %d activate HTTP %ld", r->port, code);
            if (cb) cb(r->type, headline, "activated", r->port, cb_user);
        } else if (!active && g_prev_active[i]) {
            syslog(LOG_INFO, "alerts: cleared %s → port %d", r->type, r->port);
            long code = vapix_port_set(r->port, 0, vapix_user, vapix_pass);
            if (code != 200)
                syslog(LOG_WARNING, "alerts: port %d clear HTTP %ld", r->port, code);
            if (cb) cb(r->type, headline, "cleared", r->port, cb_user);
        }

        g_prev_active[i] = active;
        if (active) any = 1;
    }

    g_any_active = any;
}

void alerts_clear_all(const AlertMap *map,
                      const char *vapix_user,
                      const char *vapix_pass) {
    for (int i = 0; i < map->count; i++) {
        if (g_prev_active[i])
            vapix_port_set(map->rules[i].port, 0, vapix_user, vapix_pass);
    }
    memset(g_prev_active, 0, sizeof(g_prev_active));
    g_any_active = 0;
}

int alerts_any_active(void) {
    return g_any_active;
}
