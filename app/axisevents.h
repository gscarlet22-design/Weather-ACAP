/*
 * axisevents.h — Publish weather conditions and alert state as native
 * AXIS camera events via the axevent GLib library (ACAP Native SDK).
 *
 * Sprint 9 feature.  This module is ADDITIVE — virtual input port
 * activation (Sprints 1 / 5) continues unchanged.  These events provide
 * a second, richer integration path that shows up in the camera's own
 * event system (Axis Camera Station, Action Rules, ONVIF consumers).
 *
 * Two event types are declared on startup:
 *
 *   Alert (stateful / property event)
 *     active     : bool   — true while any NWS / threshold alert is active
 *     alert_type : string — most-recently-activated alert type label
 *     action     : string — "activated" | "cleared"
 *
 *   Conditions (stateless / notification event, fires every poll)
 *     temp_f       : double
 *     wind_mph     : double
 *     humidity_pct : int
 *     description  : string
 *
 * Compiled in only when the axevent pkg-config package is present
 * (HAVE_AXEVENT is defined by the build system).  Stubs are provided
 * so callers compile cleanly regardless.
 */

#ifndef AXISEVENTS_H
#define AXISEVENTS_H

#include "weather_api.h"   /* WeatherSnapshot */

/*
 * Initialise the axevent handler and declare both event types.
 * Must be called from the main thread after params_init() and
 * g_main_loop_new() (axevent requires the GLib main loop).
 * Safe to call when compiled without axevent — becomes a no-op.
 */
void axisevents_init(void);

/*
 * Publish an alert state-change event.
 *
 * event_type — NWS alert type or threshold rule label
 * action     — "activated" or "cleared"
 * headline   — full NWS headline string (may be NULL)
 *
 * No-op when axevent is not available or AxisEventsEnabled=no.
 */
void axisevents_publish_alert(const char *event_type,
                              const char *action,
                              const char *headline);

/*
 * Publish a stateless conditions event with the latest poll data.
 * Only fires when snap->conditions.valid is true.
 * No-op when axevent is not available or AxisEventsEnabled=no.
 */
void axisevents_publish_conditions(const WeatherSnapshot *snap);

/*
 * Set the enabled flag at runtime (reads AxisEventsEnabled param value).
 * Called each poll so the param can be toggled without a restart.
 */
void axisevents_set_enabled(int enabled);

/*
 * Undeclare events and free the axevent handler.
 * Safe to call even if axisevents_init() was never called.
 */
void axisevents_cleanup(void);

#endif /* AXISEVENTS_H */
