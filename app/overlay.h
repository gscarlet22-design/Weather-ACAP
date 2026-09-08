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
 *   {temp} {temp_f} {cond} {wind} {dir} {arrow} {hum} {provider}
 *   {sunrise} {sunset} {lat} {lon} {time} {utc} {lightning}
 *   {alert_type} — only valid inside alert_template
 * {time} is the camera's local time; {utc} is HH:MM UTC.
 * Writes to out (≤ outlen). */
void overlay_render_text(const WeatherSnapshot *snap,
                         const OverlayConfig *cfg,
                         char *out, size_t outlen);

/* Push the overlay to VAPIX.  No-op if device lacks video or the overlay
 * is disabled.  Creates the overlay on first use, updates it thereafter,
 * re-creates it if the configured position changed, and persists the
 * handle to /tmp so a daemon restart reuses the existing overlay instead
 * of stacking a new one. */
void overlay_update(const WeatherSnapshot *snap,
                    const OverlayConfig *cfg,
                    const char *vapix_user,
                    const char *vapix_pass);

/* Remove this app's overlay (shutdown, or overlay disabled in config).
 * Safe to call when nothing is on screen. */
void overlay_delete(const char *vapix_user, const char *vapix_pass);

/* Remove EVERY runtime text overlay on camera 1 — including ones left
 * behind by earlier runs of this app before handles were persisted.
 * Intended as an explicit admin action (Diagnostics tab).
 * Returns the number removed, or -1 if the list call failed. */
int overlay_purge_all(const char *vapix_user, const char *vapix_pass);

/* Diagnostics: 1 = video confirmed, 0 = no video / unreachable, -1 = not
 * probed yet. */
int overlay_video_present(void);

/* Diagnostics: 1 if the last addText failed with "limit reached". */
int overlay_limit_reached(void);

/* Diagnostics: current overlay handle, or -1 if none. */
int overlay_current_id(void);

#endif /* OVERLAY_H */
