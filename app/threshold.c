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

static int g_prev_active[THRESHOLD_MAX_RULES] = { 0 };

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

/* Return the current value for a condition from the snapshot. */
static double condition_value(ThresholdCondition cond,
                               const WeatherSnapshot *snap) {
    switch (cond) {
    case THRESH_COND_TEMP_F:       return snap->conditions.temp_f;
    case THRESH_COND_WIND_MPH:     return snap->conditions.wind_speed_mph;
    case THRESH_COND_HUMIDITY_PCT: return (double)snap->conditions.humidity_pct;
    case THRESH_COND_WIND_DIR_DEG: return (double)snap->conditions.wind_dir_deg;
    default:                        return 0.0;
    }
}

static int evaluate_rule(const ThresholdRule *r,
                          const WeatherSnapshot *snap) {
    double cur = condition_value(r->condition, snap);
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

    for (int i = 0; i < map->count; i++) {
        const ThresholdRule *r = &map->rules[i];

        if (!r->enabled) {
            if (g_prev_active[i]) {
                vapix_port_set(r->port, 0, vapix_user, vapix_pass);
                g_prev_active[i] = 0;
            }
            continue;
        }

        int active = evaluate_rule(r, snap);

        if (active && !g_prev_active[i]) {
            double cur = condition_value(r->condition, snap);
            char headline[128];
            snprintf(headline, sizeof(headline), "%s (current: %.4g)",
                     r->label, cur);

            syslog(LOG_WARNING, "threshold: ACTIVE %s → port %d",
                   r->label, r->port);
            long code = vapix_port_set(r->port, 1, vapix_user, vapix_pass);
            if (code != 200)
                syslog(LOG_WARNING, "threshold: port %d activate HTTP %ld",
                       r->port, code);
            if (cb) cb(r->label, headline, "activated", r->port, cb_user);

        } else if (!active && g_prev_active[i]) {
            double cur = condition_value(r->condition, snap);
            char headline[128];
            snprintf(headline, sizeof(headline), "%s (current: %.4g)",
                     r->label, cur);

            syslog(LOG_INFO, "threshold: cleared %s → port %d",
                   r->label, r->port);
            long code = vapix_port_set(r->port, 0, vapix_user, vapix_pass);
            if (code != 200)
                syslog(LOG_WARNING, "threshold: port %d clear HTTP %ld",
                       r->port, code);
            if (cb) cb(r->label, headline, "cleared", r->port, cb_user);
        }

        g_prev_active[i] = active;
    }
}

/* ── Public: clear all ─────────────────────────────────────────────────── */

void threshold_clear_all(const ThresholdMap *map,
                          const char         *vapix_user,
                          const char         *vapix_pass) {
    if (!map) return;
    for (int i = 0; i < map->count; i++) {
        if (g_prev_active[i])
            vapix_port_set(map->rules[i].port, 0, vapix_user, vapix_pass);
    }
    memset(g_prev_active, 0, sizeof(g_prev_active));
}
