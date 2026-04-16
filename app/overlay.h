#ifndef OVERLAY_H
#define OVERLAY_H

#include "weather_api.h"

typedef struct {
    int         enabled;
    const char *position;         /* topLeft|topRight|bottomLeft|bottomRight */
    const char *template_str;     /* e.g. "Temp: {temp}F | {cond} | Wind: {wind}mph {dir}" */
    const char *alert_template;   /* prepended when alert active, e.g. "[ALERT: {alert_type}] " */
    int         max_alerts;       /* how many alerts to fold into {alert_type} */
} OverlayConfig;

/* Render the overlay text using template variables from snap.
 * Supported substitutions:
 *   {temp}, {cond}, {wind}, {dir}, {hum}, {provider}, {lat}, {lon}, {time}
 *   {alert_type} — only valid inside alert_template
 * Writes to out (≤ outlen). */
void overlay_render_text(const WeatherSnapshot *snap,
                         const OverlayConfig *cfg,
                         char *out, size_t outlen);

/* Push the overlay to VAPIX. No-op if device lacks video. */
void overlay_update(const WeatherSnapshot *snap,
                    const OverlayConfig *cfg,
                    const char *vapix_user,
                    const char *vapix_pass);

/* Remove the overlay (shutdown cleanup). */
void overlay_delete(const char *vapix_user, const char *vapix_pass);

#endif /* OVERLAY_H */
