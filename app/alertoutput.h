/*
 * alertoutput.h — Sprint 14: hardware alert output
 *
 * Drives up to four hardware channels when an alert activates or clears:
 *
 *   1. Speaker display notification (Axis C1710 / C1720)
 *      POST https://127.0.0.1/config/rest/speaker-display-notification/v1/simple
 *      Scrolls the alert headline across the device's front-panel display.
 *
 *   2. Strobe / siren-and-light (C1710 / C1720 — local)
 *      POST http://127.0.0.1/axis-cgi/siren_and_light.cgi  (method: start)
 *      Warning tier → red Pulse fast; Watch tier → nearest-to-amber Pulse
 *      slow.  Colour names are probed once from getCapabilities.  The
 *      strobe self-terminates via duration and is also stopped explicitly
 *      on clear.
 *
 *   3. D4200 Network Horn / remote siren — named profile activation
 *      POST http://<host>/axis-cgi/siren_and_light.cgi  (method: startProfile)
 *      Profile names are pre-configured on the D4200 and referenced by name.
 *
 *   4. Audio clip via VAPIX mediaclip.cgi
 *      GET http://127.0.0.1/axis-cgi/mediaclip.cgi?action=play&clip=<id>
 *      Clip IDs must be pre-loaded on the device.  Separate IDs for Warning
 *      and Watch tier.
 *
 * All channels fail-silently: on devices that don't support the API (any
 * non-C1710/C1720 camera) the HTTP calls simply return 404/405 and are logged
 * with no impact on the rest of the alert pipeline.
 *
 * Concurrency: all calls are synchronous (5 s timeout each, 3 s connect).
 * Worst case for one activation with every channel enabled and every
 * endpoint dead is ~25 s (probe 5 + display 5 + strobe 5 + D4200 5 +
 * audio 5); the probe result is cached so later activations skip it.
 */

#ifndef ALERTOUTPUT_H
#define ALERTOUTPUT_H

/*
 * Severity tier.  Derived from the NWS event-type string unless the
 * caller forces one (threshold rules and SPC lightning are WATCH tier —
 * a humidity crossing must not fire the red strobe / emergency profile).
 */
typedef enum {
    ALERT_TIER_NONE    = 0,   /* in forced_tier: classify by event name */
    ALERT_TIER_WATCH   = 1,
    ALERT_TIER_WARNING = 2,
} AlertTier;

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

    /* ── Tier override ──────────────────────────────────────────────────── */
    AlertTier forced_tier;         /* ALERT_TIER_NONE → classify by event name */

    /* ── VAPIX credentials (for all local API calls) ──────────────────── */
    const char *vapix_user;        /* borrowed pointer — caller owns lifetime */
    const char *vapix_pass;
} AlertOutputConfig;

/*
 * alertoutput_classify()
 * Map an NWS event-type string to a severity tier:
 *   contains "warning"                      → WARNING
 *   contains "watch"                        → WATCH   (incl. "Extreme Cold Watch")
 *   contains "emergency" or "extreme"       → WARNING
 *   contains "advisory"/"statement"/"outlook" → WATCH
 *   anything else                           → WARNING (conservative)
 * Returns ALERT_TIER_NONE only for NULL / empty strings.
 */
AlertTier alertoutput_classify(const char *nws_event);

/*
 * alertoutput_on_activate()
 * Fire all enabled hardware channels for the given event.
 * headline is used as the display message text (may be NULL → falls back to
 * nws_event).  Not gated by notification cool-down — mirrors the same policy
 * as VAPIX virtual port activation.
 */
void alertoutput_on_activate(const char *nws_event, const char *headline,
                              const AlertOutputConfig *cfg);

/*
 * alertoutput_on_clear()
 * Stops the local strobe started by the most recent activation (if the
 * firmware returned an id for it).  The display expires on its own.
 */
void alertoutput_on_clear(const char *nws_event, const AlertOutputConfig *cfg);

#endif /* ALERTOUTPUT_H */
