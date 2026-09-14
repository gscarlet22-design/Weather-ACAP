/*
 * axisevents.c — Publish weather events via the AXIS axevent GLib library.
 *
 * Sprint 9.  Additive on top of virtual input port activation.
 *
 * Compiled with -DHAVE_AXEVENT when the axevent pkg-config package is
 * found at build time (see Makefile / CMakeLists.txt).  When the flag is
 * absent, every public function in this file is a no-op stub — the rest of
 * the daemon compiles and links unchanged.
 *
 * Event topology (visible in the camera's Action Rules event picker):
 *
 *   Topic path                              Kind       Fires when
 *   ──────────────────────────────────────  ─────────  ──────────────────────
 *   tnsaxis:Application/WeatherACAP/Alert   Stateful   NWS / threshold change
 *   tnsaxis:Application/WeatherACAP/Cond    Stateless  Every successful poll
 */

#include "axisevents.h"
#include "alerts.h"
#include "threshold.h"

#include <stdio.h>
#include <string.h>
#include <syslog.h>

/* ── Runtime enable/disable ─────────────────────────────────────────────── */
static int g_enabled = 1;

void axisevents_set_enabled(int enabled) {
    g_enabled = enabled;
}

/* =========================================================================
 * HAVE_AXEVENT path — full implementation
 * ========================================================================= */
#ifdef HAVE_AXEVENT

#include <axsdk/axevent.h>

static AXEventHandler *g_handler       = NULL;
static guint           g_alert_id      = 0;   /* stateful alert event    */
static guint           g_cond_id       = 0;   /* stateless cond event    */

/* ── Internal helpers ────────────────────────────────────────────────────── */

/* Add a bool key to a key-value set (helper to keep callers concise). */
static void kv_add_bool(AXEventKeyValueSet *set, const char *key, gboolean val) {
    GError *err = NULL;
    if (!ax_event_key_value_set_add_key_value(set, key, NULL,
            &val, AX_VALUE_TYPE_BOOL, &err)) {
        syslog(LOG_WARNING, "axisevents: kv_add_bool(%s): %s",
               key, err ? err->message : "?");
        if (err) g_error_free(err);
    }
}

static void kv_add_int(AXEventKeyValueSet *set, const char *key, gint val) {
    GError *err = NULL;
    if (!ax_event_key_value_set_add_key_value(set, key, NULL,
            &val, AX_VALUE_TYPE_INT, &err)) {
        syslog(LOG_WARNING, "axisevents: kv_add_int(%s): %s",
               key, err ? err->message : "?");
        if (err) g_error_free(err);
    }
}

static void kv_add_double(AXEventKeyValueSet *set, const char *key, gdouble val) {
    GError *err = NULL;
    if (!ax_event_key_value_set_add_key_value(set, key, NULL,
            &val, AX_VALUE_TYPE_DOUBLE, &err)) {
        syslog(LOG_WARNING, "axisevents: kv_add_double(%s): %s",
               key, err ? err->message : "?");
        if (err) g_error_free(err);
    }
}

static void kv_add_str(AXEventKeyValueSet *set, const char *key,
                        const char *val) {
    GError *err = NULL;
    if (!ax_event_key_value_set_add_key_value(set, key, NULL,
            val ? val : "", AX_VALUE_TYPE_STRING, &err)) {
        syslog(LOG_WARNING, "axisevents: kv_add_str(%s): %s",
               key, err ? err->message : "?");
        if (err) g_error_free(err);
    }
}

/* Topic path for a declaration.  axevent rejects a set without the
 * tnsaxis topic keys ("External event is missing topic"), which is why
 * neither event ever appeared in Action Rules before 1.1.0.  The path
 * shows up in the event picker as
 *   Application platform → Weather Monitor ACAP → <leaf> */
static void add_topics(AXEventKeyValueSet *set, const char *leaf,
                       const char *leaf_label) {
    GError *err = NULL;
    const struct { const char *key; const char *val; } topics[] = {
        { "topic0", "CameraApplicationPlatform" },
        { "topic1", "WeatherACAP" },
        { "topic2", leaf },
    };
    for (size_t i = 0; i < 3; i++) {
        if (!ax_event_key_value_set_add_key_value(set, topics[i].key, "tnsaxis",
                topics[i].val, AX_VALUE_TYPE_STRING, &err)) {
            syslog(LOG_WARNING, "axisevents: add %s: %s",
                   topics[i].key, err ? err->message : "?");
            if (err) { g_error_free(err); err = NULL; }
        }
    }
    /* Friendly names in the picker (best effort). */
    ax_event_key_value_set_mark_as_user_defined(set, "topic1", "tnsaxis",
                                                "Weather Monitor ACAP", NULL);
    ax_event_key_value_set_mark_as_user_defined(set, "topic2", "tnsaxis",
                                                leaf_label, NULL);
}

/* Keys whose values change per event must be marked as data. */
static void mark_data(AXEventKeyValueSet *set, const char *key) {
    GError *err = NULL;
    if (!ax_event_key_value_set_mark_as_data(set, key, NULL, &err)) {
        syslog(LOG_WARNING, "axisevents: mark_as_data(%s): %s",
               key, err ? err->message : "?");
        if (err) g_error_free(err);
    }
}

/* ── Init ────────────────────────────────────────────────────────────────── */

void axisevents_init(void) {
    g_handler = ax_event_handler_new();
    if (!g_handler) {
        syslog(LOG_WARNING, "axisevents: ax_event_handler_new() returned NULL");
        return;
    }

    GError *err = NULL;

    /* ── Alert event — stateful (property) ─────────────────────────────── */
    {
        AXEventKeyValueSet *set = ax_event_key_value_set_new();
        add_topics(set, "Alert", "Weather alert");
        kv_add_bool(set, "active",     FALSE);
        kv_add_str (set, "alert_type", "");
        kv_add_str (set, "action",     "");
        mark_data(set, "active");
        mark_data(set, "alert_type");
        mark_data(set, "action");

        if (!ax_event_handler_declare(g_handler, set, TRUE /* stateful */,
                                      &g_alert_id, NULL, NULL, &err)) {
            syslog(LOG_WARNING, "axisevents: declare Alert event failed: %s",
                   err ? err->message : "?");
            if (err) { g_error_free(err); err = NULL; }
            g_alert_id = 0;
        } else {
            syslog(LOG_INFO,
                   "axisevents: Alert event declared (id=%u)", g_alert_id);
        }
        ax_event_key_value_set_free(set);
    }

    /* ── Conditions event — stateless (notification) ───────────────────── */
    {
        AXEventKeyValueSet *set = ax_event_key_value_set_new();
        add_topics(set, "Conditions", "Weather conditions");
        kv_add_double(set, "temp_f",       0.0);
        kv_add_double(set, "wind_mph",     0.0);
        kv_add_int   (set, "humidity_pct", 0);
        kv_add_str   (set, "description",  "");
        mark_data(set, "temp_f");
        mark_data(set, "wind_mph");
        mark_data(set, "humidity_pct");
        mark_data(set, "description");

        if (!ax_event_handler_declare(g_handler, set, FALSE /* stateless */,
                                      &g_cond_id, NULL, NULL, &err)) {
            syslog(LOG_WARNING,
                   "axisevents: declare Conditions event failed: %s",
                   err ? err->message : "?");
            if (err) { g_error_free(err); err = NULL; }
            g_cond_id = 0;
        } else {
            syslog(LOG_INFO,
                   "axisevents: Conditions event declared (id=%u)", g_cond_id);
        }
        ax_event_key_value_set_free(set);
    }
}

/* ── Publish alert ───────────────────────────────────────────────────────── */

void axisevents_publish_alert(const char *event_type,
                               const char *action,
                               const char *headline) {
    (void)headline;   /* reserved for future use */
    if (!g_enabled || !g_handler || !g_alert_id) return;

    /* The stateful `active` property must mean "any alert is active", not
     * "this transition was an activation" — otherwise a Flood Warning
     * clearing flips it false while a Tornado Warning is still up and an
     * Action Rule keyed on it stops recording. */
    gboolean active = (alerts_any_active() || threshold_any_active())
                      ? TRUE : FALSE;

    AXEventKeyValueSet *set = ax_event_key_value_set_new();
    kv_add_bool(set, "active",     active);
    kv_add_str (set, "alert_type", event_type ? event_type : "");
    kv_add_str (set, "action",     action     ? action     : "");

    GError  *err = NULL;
    AXEvent *ev  = ax_event_new2(set, NULL);
    if (!ax_event_handler_send_event(g_handler, g_alert_id, ev, &err)) {
        syslog(LOG_WARNING, "axisevents: send Alert event failed: %s",
               err ? err->message : "?");
        if (err) g_error_free(err);
    } else {
        syslog(LOG_INFO,
               "axisevents: Alert event sent (type=%s action=%s)",
               event_type ? event_type : "?",
               action     ? action     : "?");
    }
    ax_event_free(ev);
    ax_event_key_value_set_free(set);
}

/* ── Publish conditions ─────────────────────────────────────────────────── */

void axisevents_publish_conditions(const WeatherSnapshot *snap) {
    if (!g_enabled || !g_handler || !g_cond_id) return;
    if (!snap || !snap->conditions.valid) return;

    gdouble temp = snap->conditions.temp_f;
    gdouble wind = snap->conditions.wind_speed_mph;
    gint    hum  = snap->conditions.humidity_pct;

    AXEventKeyValueSet *set = ax_event_key_value_set_new();
    kv_add_double(set, "temp_f",       temp);
    kv_add_double(set, "wind_mph",     wind);
    kv_add_int   (set, "humidity_pct", hum);
    kv_add_str   (set, "description",  snap->conditions.description);

    GError  *err = NULL;
    AXEvent *ev  = ax_event_new2(set, NULL);
    if (!ax_event_handler_send_event(g_handler, g_cond_id, ev, &err)) {
        syslog(LOG_WARNING, "axisevents: send Conditions event failed: %s",
               err ? err->message : "?");
        if (err) g_error_free(err);
    }
    ax_event_free(ev);
    ax_event_key_value_set_free(set);
}

/* ── Cleanup ─────────────────────────────────────────────────────────────── */

void axisevents_cleanup(void) {
    if (!g_handler) return;
    GError *err = NULL;
    if (g_alert_id) {
        ax_event_handler_undeclare(g_handler, g_alert_id, &err);
        if (err) { syslog(LOG_WARNING, "axisevents: undeclare alert: %s", err->message); g_error_free(err); err = NULL; }
        g_alert_id = 0;
    }
    if (g_cond_id) {
        ax_event_handler_undeclare(g_handler, g_cond_id, &err);
        if (err) { syslog(LOG_WARNING, "axisevents: undeclare cond: %s", err->message); g_error_free(err); err = NULL; }
        g_cond_id = 0;
    }
    ax_event_handler_free(g_handler);
    g_handler = NULL;
}

/* =========================================================================
 * No-axevent path — stubs so callers compile without the library
 * ========================================================================= */
#else

void axisevents_init(void) {
    syslog(LOG_INFO, "axisevents: built without axevent; "
                     "native camera events disabled");
}

void axisevents_publish_alert(const char *e, const char *a, const char *h) {
    (void)e; (void)a; (void)h;
}

void axisevents_publish_conditions(const WeatherSnapshot *snap) {
    (void)snap;
}

void axisevents_cleanup(void) {}

#endif /* HAVE_AXEVENT */
