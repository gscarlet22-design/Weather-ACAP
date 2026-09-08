#ifndef ALERTS_H
#define ALERTS_H

#include "weather_api.h"

/* One entry from AlertMap: "Type:Port:Enabled|..." */
#define ALERTS_MAX_TYPES 64
#define ALERTS_MAX_TYPE_LEN 96

typedef struct {
    char type[ALERTS_MAX_TYPE_LEN];
    int  port;
    int  enabled;
} AlertRule;

typedef struct {
    AlertRule rules[ALERTS_MAX_TYPES];
    int       count;
} AlertMap;

/* Parse "Type:Port:Enabled|Type:Port:Enabled|..." into a map. */
void alerts_map_parse(const char *mapstr, AlertMap *out);

/* Process active NWS alerts against the map.
 * For transitions (new/cleared), invoke the VAPIX virtual port and call
 * the on_transition callback (for history / webhooks).
 *
 * State is keyed by (type, port), not rule index, so the AlertMap may be
 * edited at runtime.  A rule that is disabled or deleted while active is
 * cleared through the callback like any other transition.  A VAPIX write
 * that fails is retried on the next poll until it succeeds.
 *
 * Callers must only invoke this when snap->alerts.fetch_ok is set — an
 * empty list from a failed fetch is "unknown", not "no alerts".
 *
 * transition_cb can be NULL. action is "activated" or "cleared".
 */
typedef void (*alerts_transition_cb)(const char *event,
                                     const char *headline,
                                     const char *action,
                                     int port,
                                     void *user_data);

void alerts_process(const WeatherSnapshot *snap,
                    const AlertMap *map,
                    const char *vapix_user,
                    const char *vapix_pass,
                    alerts_transition_cb cb,
                    void *cb_user);

/* Clear every port this module currently believes is active and forget
 * the state (shutdown / system disabled). */
void alerts_clear_all(const AlertMap *map,
                      const char *vapix_user,
                      const char *vapix_pass);

/* Force every port in the map OFF regardless of remembered state.  Call
 * once at startup: a crash or SIGKILL skips alerts_clear_all(), and the
 * next process has no memory of which ports the old one left ON. */
void alerts_reset_ports(const AlertMap *map,
                        const char *vapix_user,
                        const char *vapix_pass);

/* True if any NWS-alert rule is currently firing a port. */
int alerts_any_active(void);

#endif /* ALERTS_H */
