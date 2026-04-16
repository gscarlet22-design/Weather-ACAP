#ifndef WEBHOOK_H
#define WEBHOOK_H

#include "weather_api.h"

/*
 * Post a JSON payload to the configured webhook URL.
 *
 * event_type: e.g. "alert_activated", "alert_cleared", "poll", "firedrill"
 * alert_event: alert name or NULL for non-alert events
 *
 * Returns HTTP status code, or 0 on connection error.
 */
long webhook_post(const char *url,
                  const WeatherSnapshot *snap,
                  const char *event_type,
                  const char *alert_event);

#endif /* WEBHOOK_H */
