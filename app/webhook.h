#ifndef WEBHOOK_H
#define WEBHOOK_H

#include "weather_api.h"

/*
 * Post a payload to the configured webhook URL.
 *
 * event_type : e.g. "alert_activated", "alert_cleared", "poll", "firedrill"
 * alert_event: alert name or NULL for non-alert events
 * headline   : NWS headline text or NULL — used in {headline} template token
 * tmpl       : payload template string, or NULL/"" to use built-in JSON body
 *
 * Returns HTTP status code, or 0 on connection error.
 */
long webhook_post(const char *url,
                  const WeatherSnapshot *snap,
                  const char *event_type,
                  const char *alert_event,
                  const char *headline,
                  const char *tmpl);

/*
 * Render a webhook template string, replacing {token} placeholders.
 * Output is written into buf (max outlen bytes, NUL-terminated).
 * Returns the number of bytes written (not counting NUL).
 */
size_t webhook_render_template(const char *tmpl,
                               const WeatherSnapshot *snap,
                               const char *event_type,
                               const char *alert_event,
                               const char *headline,
                               char *buf, size_t outlen);

#endif /* WEBHOOK_H */
