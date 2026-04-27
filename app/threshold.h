/*
 * threshold.h — Threshold-based condition alerts.
 *
 * Maps numeric weather conditions (temperature, wind speed, humidity,
 * wind direction) to VAPIX virtual input ports via configurable
 * comparison rules.  When a condition crosses its threshold the
 * corresponding port is activated; when it drops back below (or above,
 * depending on the operator) the port is cleared.
 *
 * Map format (ThresholdMap param = "ThresholdMap"):
 *   "Condition:Operator:Value:Port:Enabled|..."
 *
 * Conditions : TempF | WindMph | HumidityPct | WindDirDeg
 * Operators  : >  <  >=  <=
 * Value      : floating-point threshold value
 * Port       : VAPIX virtual input port number (1–255)
 * Enabled    : 0 or 1
 *
 * Example:
 *   "TempF:>:90:10:1|TempF:<:32:11:1|WindMph:>:40:12:1|HumidityPct:>:90:13:0"
 */
#ifndef THRESHOLD_H
#define THRESHOLD_H

#include "weather_api.h"

#define THRESHOLD_MAX_RULES 16

typedef enum {
    THRESH_COND_TEMP_F = 0,
    THRESH_COND_WIND_MPH,
    THRESH_COND_HUMIDITY_PCT,
    THRESH_COND_WIND_DIR_DEG,
    THRESH_COND_UNKNOWN
} ThresholdCondition;

typedef enum {
    THRESH_OP_GT = 0,
    THRESH_OP_LT,
    THRESH_OP_GTE,
    THRESH_OP_LTE,
    THRESH_OP_UNKNOWN
} ThresholdOperator;

typedef struct {
    ThresholdCondition condition;
    ThresholdOperator  op;
    double             value;
    int                port;
    int                enabled;
    /* Human-readable label built at parse time for log messages. */
    char               label[64];
} ThresholdRule;

typedef struct {
    ThresholdRule rules[THRESHOLD_MAX_RULES];
    int           count;
} ThresholdMap;

/* Parse "Condition:Op:Value:Port:Enabled|..." into a map.
 * Unknown condition/operator tokens are silently skipped. */
void threshold_map_parse(const char *mapstr, ThresholdMap *out);

/* Evaluate each rule against the current snapshot.
 * Port transitions are written via VAPIX and the callback is invoked with:
 *   event    — rule label, e.g. "TempF > 90"
 *   headline — detail string, e.g. "TempF > 90 (current: 92.5)"
 *   action   — "activated" or "cleared"
 *   port     — VAPIX port number
 *
 * The callback signature is identical to alerts_transition_cb so the same
 * on_alert_transition handler can be reused for MQTT / email / snapshot. */
typedef void (*threshold_transition_cb)(const char *event,
                                        const char *headline,
                                        const char *action,
                                        int         port,
                                        void       *user_data);

void threshold_process(const WeatherSnapshot    *snap,
                       const ThresholdMap       *map,
                       const char               *vapix_user,
                       const char               *vapix_pass,
                       threshold_transition_cb   cb,
                       void                     *cb_user);

/* Clear all active threshold ports (call on shutdown). */
void threshold_clear_all(const ThresholdMap *map,
                         const char         *vapix_user,
                         const char         *vapix_pass);

#endif /* THRESHOLD_H */
