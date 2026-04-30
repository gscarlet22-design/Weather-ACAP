/*
 * alertoutput.h — Sprint 14: hardware alert output
 *
 * Drives up to four hardware channels when an NWS alert activates or clears:
 *
 *   1. Speaker display notification (Axis C1710 / C1720)
 *      POST https://127.0.0.1/config/rest/speaker-display-notification/v1/simple
 *      Scrolls the alert headline across the device's front-panel display.
 *
 *   2. Strobe / siren-and-light (C1710 / C1720 — local)
 *      POST http://127.0.0.1/axis-cgi/siren_and_light.cgi  (method: start)
 *      Warning tier → red Pulse fast; Watch tier → amber Pulse slow.
 *      Self-terminates via configured duration; no explicit stop needed.
 *
 *   3. D4200 Network Horn / remote siren — named profile activation
 *      POST http://<host>/axis-cgi/siren_and_light.cgi  (method: startProfile)
 *      Profile names are pre-configured on the D4200 and referenced by name.
 *
 *   4. Audio clip via VAPIX mediaclip.cgi
 *      GET http://127.0.0.1/axis-cgi/mediaclip.cgi?action=play&clip=<id>
 *      Clip IDs must be pre-loaded on the device (via the device web UI or
 *      mediaclip.cgi upload).  Separate IDs for Warning and Watch tier.
 *
 * All channels fail-silently: on devices that don't support the API (any
 * non-C1710/C1720 camera) the HTTP calls simply return 404/405 and are logged
 * at LOG_INFO level with no impact on the rest of the alert pipeline.
 *
 * Concurrency: all calls are synchronous (blocking up to CURLOPT_TIMEOUT=5s
 * each).  They are made from on_alert_transition() which fires only on genuine
 * state transitions, not every poll tick — so the brief latency is acceptable.
 */

#ifndef ALERTOUTPUT_H
#define ALERTOUTPUT_H

typedef struct {
    /* ── Speaker display (C1710 / C1720) ──────────────────────────────────── */
    int  display_enabled;
    int  display_duration_s;       /* per-alert display time, seconds (default 30) */
    char display_text_color[16];   /* hex e.g. "#FFFFFF" */
    char display_bg_warning[16];   /* background for Warning-tier alerts e.g. "#CC0000" */
    char display_bg_watch[16];     /* background for Watch-tier alerts  e.g. "#FF8800" */

    /* ── Local strobe / siren-and-light (C1710 / C1720) ─────────────────── */
    int  strobe_enabled;
    int  strobe_duration_s;        /* how long the strobe runs, seconds (default 30) */

    /* ── D4200 Network Horn — remote named profile ───────────────────────── */
    int  d4200_enabled;
    char d4200_host[128];          /* IP or hostname, no scheme — e.g. "192.168.1.50" */
    char d4200_user[64];
    char d4200_pass[64];
    char d4200_warning_profile[64];/* profile name on the D4200 for Warning tier */
    char d4200_watch_profile[64];  /* profile name on the D4200 for Watch tier */

    /* ── Audio clip via mediaclip.cgi ───────────────────────────────────── */
    int  audio_enabled;
    int  audio_clip_warning;       /* clip ID integer; -1 = disabled for this tier */
    int  audio_clip_watch;         /* clip ID integer; -1 = disabled for this tier */

    /* ── VAPIX credentials (for all local API calls) ──────────────────── */
    const char *vapix_user;        /* borrowed pointer — caller owns lifetime */
    const char *vapix_pass;
} AlertOutputConfig;

/*
 * Severity tier derived from the NWS event-type string.
 * Anything containing "Warning" or "Emergency" is WARNING.
 * "Watch", "Advisory", "Statement", "Outlook" → WATCH.
 * Unrecognised strings default to WARNING (conservative).
 */
typedef enum {
    ALERT_TIER_NONE    = 0,
    ALERT_TIER_WATCH   = 1,
    ALERT_TIER_WARNING = 2,
} AlertTier;

/*
 * alertoutput_classify()
 * Map an NWS event-type string to a severity tier.
 * Returns ALERT_TIER_NONE only for NULL / empty strings.
 */
AlertTier alertoutput_classify(const char *nws_event);

/*
 * alertoutput_on_activate()
 * Fire all enabled hardware channels for the given NWS event.
 * headline is used as the display message text (may be NULL → falls back to
 * nws_event).  Not gated by notification cool-down — mirrors the same policy
 * as VAPIX virtual port activation.
 */
void alertoutput_on_activate(const char *nws_event, const char *headline,
                              const AlertOutputConfig *cfg);

/*
 * alertoutput_on_clear()
 * Called when an alert clears.  Currently a no-op: the strobe self-terminates
 * via its configured duration and the display expires automatically.  Reserved
 * for future firmware that exposes an explicit siren_and_light stop method.
 */
void alertoutput_on_clear(const char *nws_event, const AlertOutputConfig *cfg);

#endif /* ALERTOUTPUT_H */
