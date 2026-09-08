#include "alerts.h"
#include "vapix.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <syslog.h>

/* ── Per-rule state ─────────────────────────────────────────────────────── */

/* Keyed by (type, port), NOT by rule index.  The map is re-parsed from live
 * config every poll; an index-keyed table pointed at the wrong rule as soon
 * as the user inserted, deleted or reordered a row mid-alert — a port got
 * "cleared" that was never set, and the real one stayed ON forever. */
typedef struct {
    char type[ALERTS_MAX_TYPE_LEN];
    int  port;
    int  active;    /* logical state — edge-detected, drives callbacks */
    int  port_ok;   /* last VAPIX write for `active` returned HTTP 200 */
    int  seen;      /* touched during the current alerts_process pass */
} AlertState;

static AlertState g_state[ALERTS_MAX_TYPES];
static int        g_state_n = 0;

static AlertState *find_state(const char *type, int port) {
    for (int i = 0; i < g_state_n; i++)
        if (g_state[i].port == port && strcasecmp(g_state[i].type, type) == 0)
            return &g_state[i];
    return NULL;
}

static AlertState *get_state(const char *type, int port) {
    AlertState *st = find_state(type, port);
    if (st) return st;
    if (g_state_n >= ALERTS_MAX_TYPES) return NULL;
    st = &g_state[g_state_n++];
    memset(st, 0, sizeof(*st));
    snprintf(st->type, sizeof(st->type), "%s", type);
    st->port    = port;
    st->port_ok = 1;
    return st;
}

/* Write a port; returns 1 on HTTP 200. */
static int set_port(int port, int on, const char *user, const char *pass) {
    long code = vapix_port_set(port, on, user, pass);
    if (code != 200) {
        syslog(LOG_WARNING, "alerts: port %d %s HTTP %ld — will retry next poll",
               port, on ? "activate" : "clear", code);
        return 0;
    }
    return 1;
}

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
    for (int i = 0; i < g_state_n; i++) g_state[i].seen = 0;

    for (int i = 0; i < map->count; i++) {
        const AlertRule *r  = &map->rules[i];
        AlertState      *st = get_state(r->type, r->port);
        if (!st) continue;
        st->seen = 1;

        if (!r->enabled) {
            if (st->active) {
                syslog(LOG_INFO, "alerts: %s disabled while active → clearing port %d",
                       r->type, r->port);
                st->port_ok = set_port(r->port, 0, vapix_user, vapix_pass);
                st->active  = 0;
                if (cb) cb(r->type, "", "cleared", r->port, cb_user);
            }
            continue;
        }

        const char *headline = "";
        int active = find_matching_alert(r, snap, &headline);

        if (active && !st->active) {
            syslog(LOG_WARNING, "alerts: ACTIVE %s → port %d", r->type, r->port);
            st->port_ok = set_port(r->port, 1, vapix_user, vapix_pass);
            st->active  = 1;
            if (cb) cb(r->type, headline, "activated", r->port, cb_user);
        } else if (!active && st->active) {
            syslog(LOG_INFO, "alerts: cleared %s → port %d", r->type, r->port);
            st->port_ok = set_port(r->port, 0, vapix_user, vapix_pass);
            st->active  = 0;
            if (cb) cb(r->type, headline, "cleared", r->port, cb_user);
        } else if (!st->port_ok) {
            /* No edge, but the last write failed — retry until the camera
             * agrees with us.  Previously a transient 401/timeout on the
             * activate left the daemon believing the port was ON while it
             * was OFF, with no retry ever. */
            st->port_ok = set_port(r->port, st->active, vapix_user, vapix_pass);
            if (st->port_ok)
                syslog(LOG_INFO, "alerts: port %d write recovered (%s)",
                       r->port, st->active ? "on" : "off");
        }
    }

    /* Rules removed from the map while active: clear their ports too. */
    for (int i = 0; i < g_state_n; i++) {
        AlertState *st = &g_state[i];
        if (st->seen || !st->active) continue;
        syslog(LOG_INFO, "alerts: rule %s removed while active → clearing port %d",
               st->type, st->port);
        st->port_ok = set_port(st->port, 0, vapix_user, vapix_pass);
        st->active  = 0;
        if (cb) cb(st->type, "", "cleared", st->port, cb_user);
    }
}

void alerts_clear_all(const AlertMap *map,
                      const char *vapix_user,
                      const char *vapix_pass) {
    (void)map;
    for (int i = 0; i < g_state_n; i++) {
        if (g_state[i].active)
            vapix_port_set(g_state[i].port, 0, vapix_user, vapix_pass);
    }
    memset(g_state, 0, sizeof(g_state));
    g_state_n = 0;
}

void alerts_reset_ports(const AlertMap *map,
                        const char *vapix_user,
                        const char *vapix_pass) {
    int n = 0;
    for (int i = 0; i < map->count; i++) {
        if (vapix_port_set(map->rules[i].port, 0, vapix_user, vapix_pass) == 200)
            n++;
    }
    syslog(LOG_INFO, "alerts: startup reset — %d/%d mapped ports forced OFF",
           n, map->count);
}

int alerts_any_active(void) {
    /* Live scan rather than the end-of-pass flag: axisevents reads this
     * from inside the transition callback, mid-pass, and must see the
     * rule that just flipped. */
    for (int i = 0; i < g_state_n; i++)
        if (g_state[i].active) return 1;
    return 0;
}
