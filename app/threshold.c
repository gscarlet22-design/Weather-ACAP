/*
 * threshold.c — Threshold-based condition alerts.
 *
 * See threshold.h for format and design notes.
 */
#include "threshold.h"
#include "vapix.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>

/* ── Static activation state (persists across poll ticks) ──────────────── */

/* Keyed by (label, port) rather than rule index — see alerts.c for why.
 * The label encodes condition/operator/value, so editing a rule's value
 * yields a new identity and the old one is cleared as "removed". */
typedef struct {
    char label[64];
    int  port;
    int  active;
    int  port_ok;
    int  seen;
} ThreshState;

static ThreshState g_state[THRESHOLD_MAX_RULES];
static int         g_state_n = 0;

static ThreshState *get_state(const char *label, int port) {
    for (int i = 0; i < g_state_n; i++)
        if (g_state[i].port == port && strcmp(g_state[i].label, label) == 0)
            return &g_state[i];
    if (g_state_n >= THRESHOLD_MAX_RULES) return NULL;
    ThreshState *st = &g_state[g_state_n++];
    memset(st, 0, sizeof(*st));
    snprintf(st->label, sizeof(st->label), "%s", label);
    st->port    = port;
    st->port_ok = 1;
    return st;
}

static int set_port(int port, int on, const char *user, const char *pass) {
    long code = vapix_port_set(port, on, user, pass);
    if (code != 200) {
        syslog(LOG_WARNING, "threshold: port %d %s HTTP %ld — will retry next poll",
               port, on ? "activate" : "clear", code);
        return 0;
    }
    return 1;
}

/* ── Parser helpers ─────────────────────────────────────────────────────── */

static void str_trim(char *s) {
    if (!s || !*s) return;
    char *start = s;
    while (*start == ' ' || *start == '\t') start++;
    if (start != s) memmove(s, start, strlen(start) + 1);
    size_t n = strlen(s);
    while (n > 0 && (s[n-1] == ' ' || s[n-1] == '\t' ||
                     s[n-1] == '\r' || s[n-1] == '\n'))
        s[--n] = '\0';
}

static ThresholdCondition parse_condition(const char *s) {
    if (!s) return THRESH_COND_UNKNOWN;
    if (strcasecmp(s, "TempF")        == 0) return THRESH_COND_TEMP_F;
    if (strcasecmp(s, "WindMph")      == 0) return THRESH_COND_WIND_MPH;
    if (strcasecmp(s, "HumidityPct")  == 0) return THRESH_COND_HUMIDITY_PCT;
    if (strcasecmp(s, "WindDirDeg")   == 0) return THRESH_COND_WIND_DIR_DEG;
    return THRESH_COND_UNKNOWN;
}

static ThresholdOperator parse_operator(const char *s) {
    if (!s) return THRESH_OP_UNKNOWN;
    if (strcmp(s, ">")  == 0) return THRESH_OP_GT;
    if (strcmp(s, "<")  == 0) return THRESH_OP_LT;
    if (strcmp(s, ">=") == 0) return THRESH_OP_GTE;
    if (strcmp(s, "<=") == 0) return THRESH_OP_LTE;
    return THRESH_OP_UNKNOWN;
}

static const char *condition_name(ThresholdCondition c) {
    switch (c) {
    case THRESH_COND_TEMP_F:       return "TempF";
    case THRESH_COND_WIND_MPH:     return "WindMph";
    case THRESH_COND_HUMIDITY_PCT: return "HumidityPct";
    case THRESH_COND_WIND_DIR_DEG: return "WindDirDeg";
    default:                        return "Unknown";
    }
}

static const char *operator_str(ThresholdOperator op) {
    switch (op) {
    case THRESH_OP_GT:  return ">";
    case THRESH_OP_LT:  return "<";
    case THRESH_OP_GTE: return ">=";
    case THRESH_OP_LTE: return "<=";
    default:             return "?";
    }
}

/* ── Public: parse ─────────────────────────────────────────────────────── */

void threshold_map_parse(const char *mapstr, ThresholdMap *out) {
    memset(out, 0, sizeof(*out));
    if (!mapstr || !*mapstr) return;

    char buf[2048];
    snprintf(buf, sizeof(buf), "%s", mapstr);

    char *save = NULL;
    char *tok  = strtok_r(buf, "|", &save);

    while (tok && out->count < THRESHOLD_MAX_RULES) {
        /* Expected format: "Condition:Operator:Value:Port:Enabled" */
        char *parts[5];
        int n = 0;
        char *s = tok;
        while (n < 5) {
            parts[n++] = s;
            char *colon = strchr(s, ':');
            if (!colon) break;
            *colon = '\0';
            s = colon + 1;
        }
        if (n < 5) { tok = strtok_r(NULL, "|", &save); continue; }

        for (int i = 0; i < 5; i++) str_trim(parts[i]);

        ThresholdCondition cond = parse_condition(parts[0]);
        ThresholdOperator  op   = parse_operator(parts[1]);

        if (cond == THRESH_COND_UNKNOWN || op == THRESH_OP_UNKNOWN) {
            tok = strtok_r(NULL, "|", &save);
            continue;
        }

        double val  = atof(parts[2]);
        int    port = atoi(parts[3]);
        int    ena  = atoi(parts[4]) ? 1 : 0;

        if (port <= 0) { tok = strtok_r(NULL, "|", &save); continue; }

        ThresholdRule *r = &out->rules[out->count];
        r->condition = cond;
        r->op        = op;
        r->value     = val;
        r->port      = port;
        r->enabled   = ena;
        snprintf(r->label, sizeof(r->label), "%s %s %.4g",
                 condition_name(cond), operator_str(op), val);
        out->count++;

        tok = strtok_r(NULL, "|", &save);
    }
}

/* ── Evaluation ────────────────────────────────────────────────────────── */

/* Return the current value for a condition from the snapshot.
 * Returns 0 (and leaves *out untouched) when the provider did not report
 * that quantity — those rules are skipped rather than evaluated against a
 * sentinel (-1 humidity used to satisfy "HumidityPct < 20"). */
static int condition_value(ThresholdCondition cond,
                           const WeatherSnapshot *snap, double *out) {
    switch (cond) {
    case THRESH_COND_TEMP_F:
        *out = snap->conditions.temp_f; return 1;
    case THRESH_COND_WIND_MPH:
        if (snap->conditions.wind_speed_mph < 0) return 0;
        *out = snap->conditions.wind_speed_mph; return 1;
    case THRESH_COND_HUMIDITY_PCT:
        if (snap->conditions.humidity_pct < 0) return 0;
        *out = (double)snap->conditions.humidity_pct; return 1;
    case THRESH_COND_WIND_DIR_DEG:
        if (snap->conditions.wind_dir_deg < 0) return 0;
        *out = (double)snap->conditions.wind_dir_deg; return 1;
    default:
        return 0;
    }
}

static int evaluate_rule(const ThresholdRule *r, double cur) {
    switch (r->op) {
    case THRESH_OP_GT:  return cur >  r->value;
    case THRESH_OP_LT:  return cur <  r->value;
    case THRESH_OP_GTE: return cur >= r->value;
    case THRESH_OP_LTE: return cur <= r->value;
    default:             return 0;
    }
}

/* ── Public: process ───────────────────────────────────────────────────── */

void threshold_process(const WeatherSnapshot    *snap,
                       const ThresholdMap       *map,
                       const char               *vapix_user,
                       const char               *vapix_pass,
                       threshold_transition_cb   cb,
                       void                     *cb_user) {
    if (!snap || !map) return;

    /* No conditions this poll (alerts-only fetch) — hold state.  An
     * all-zero snapshot used to fire "TempF < 32" on every NWS station
     * hiccup. */
    if (!snap->conditions.valid) return;

    for (int i = 0; i < g_state_n; i++) g_state[i].seen = 0;

    for (int i = 0; i < map->count; i++) {
        const ThresholdRule *r  = &map->rules[i];
        ThreshState         *st = get_state(r->label, r->port);
        if (!st) continue;
        st->seen = 1;

        if (!r->enabled) {
            if (st->active) {
                syslog(LOG_INFO, "threshold: %s disabled while active → clearing port %d",
                       r->label, r->port);
                st->port_ok = set_port(r->port, 0, vapix_user, vapix_pass);
                st->active  = 0;
                if (cb) cb(r->label, "", "cleared", r->port, cb_user);
            }
            continue;
        }

        double cur;
        if (!condition_value(r->condition, snap, &cur))
            continue;   /* quantity not reported this poll — hold state */
        int active = evaluate_rule(r, cur);

        char headline[128];
        snprintf(headline, sizeof(headline), "%s (current: %.4g)", r->label, cur);

        if (active && !st->active) {
            syslog(LOG_WARNING, "threshold: ACTIVE %s → port %d", r->label, r->port);
            st->port_ok = set_port(r->port, 1, vapix_user, vapix_pass);
            st->active  = 1;
            if (cb) cb(r->label, headline, "activated", r->port, cb_user);
        } else if (!active && st->active) {
            syslog(LOG_INFO, "threshold: cleared %s → port %d", r->label, r->port);
            st->port_ok = set_port(r->port, 0, vapix_user, vapix_pass);
            st->active  = 0;
            if (cb) cb(r->label, headline, "cleared", r->port, cb_user);
        } else if (!st->port_ok) {
            st->port_ok = set_port(r->port, st->active, vapix_user, vapix_pass);
            if (st->port_ok)
                syslog(LOG_INFO, "threshold: port %d write recovered (%s)",
                       r->port, st->active ? "on" : "off");
        }
    }

    /* Rules removed (or re-valued) while active: clear the old identity. */
    for (int i = 0; i < g_state_n; i++) {
        ThreshState *st = &g_state[i];
        if (st->seen || !st->active) continue;
        syslog(LOG_INFO, "threshold: rule %s removed while active → clearing port %d",
               st->label, st->port);
        st->port_ok = set_port(st->port, 0, vapix_user, vapix_pass);
        st->active  = 0;
        if (cb) cb(st->label, "", "cleared", st->port, cb_user);
    }
}

/* ── Public: clear all / reset ─────────────────────────────────────────── */

void threshold_clear_all(const ThresholdMap *map,
                          const char         *vapix_user,
                          const char         *vapix_pass) {
    (void)map;
    for (int i = 0; i < g_state_n; i++) {
        if (g_state[i].active)
            vapix_port_set(g_state[i].port, 0, vapix_user, vapix_pass);
    }
    memset(g_state, 0, sizeof(g_state));
    g_state_n = 0;
}

void threshold_reset_ports(const ThresholdMap *map,
                           const char         *vapix_user,
                           const char         *vapix_pass) {
    if (!map) return;
    int n = 0;
    for (int i = 0; i < map->count; i++)
        if (vapix_port_set(map->rules[i].port, 0, vapix_user, vapix_pass) == 200)
            n++;
    if (map->count)
        syslog(LOG_INFO, "threshold: startup reset — %d/%d mapped ports forced OFF",
               n, map->count);
}

int threshold_any_active(void) {
    for (int i = 0; i < g_state_n; i++)
        if (g_state[i].active) return 1;
    return 0;
}
