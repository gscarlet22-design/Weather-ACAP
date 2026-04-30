/*
 * alertoutput.c — Sprint 14: hardware alert output
 *
 * See alertoutput.h for the full API description.
 */

#include "alertoutput.h"

#include <curl/curl.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>

/* ── Internal curl response buffer ──────────────────────────────────────── */

typedef struct { char *data; size_t size; } AoBuf;

static size_t ao_write_cb(char *ptr, size_t sz, size_t nmemb, void *ud) {
    AoBuf *b = (AoBuf *)ud;
    size_t total = sz * nmemb;
    char *np = realloc(b->data, b->size + total + 1);
    if (!np) return 0;
    b->data = np;
    memcpy(b->data + b->size, ptr, total);
    b->size += total;
    b->data[b->size] = '\0';
    return total;
}

/* ── JSON-escape src into dst ───────────────────────────────────────────── */

static void ao_json_esc(const char *src, char *dst, size_t dstlen) {
    size_t j = 0;
    if (!src) src = "";
    for (const char *p = src; *p && j + 4 < dstlen; p++) {
        unsigned char c = (unsigned char)*p;
        if      (c == '"' || c == '\\') { dst[j++] = '\\'; dst[j++] = c; }
        else if (c == '\n')             { dst[j++] = '\\'; dst[j++] = 'n'; }
        else if (c == '\r')             { dst[j++] = '\\'; dst[j++] = 'r'; }
        else if (c < 0x20)             { /* skip control chars */ }
        else                            { dst[j++] = c; }
    }
    dst[j] = '\0';
}

/* ── Severity classification ─────────────────────────────────────────────── */

AlertTier alertoutput_classify(const char *nws_event) {
    if (!nws_event || !*nws_event) return ALERT_TIER_NONE;

    /* Lower-case copy for case-insensitive matching */
    char lower[128];
    size_t i;
    for (i = 0; nws_event[i] && i < sizeof(lower) - 1; i++)
        lower[i] = (char)tolower((unsigned char)nws_event[i]);
    lower[i] = '\0';

    if (strstr(lower, "warning")   ||
        strstr(lower, "emergency") ||
        strstr(lower, "extreme"))
        return ALERT_TIER_WARNING;

    if (strstr(lower, "watch")     ||
        strstr(lower, "advisory")  ||
        strstr(lower, "statement") ||
        strstr(lower, "outlook"))
        return ALERT_TIER_WATCH;

    /* Unknown type that made it through the AlertMap — treat as warning */
    return ALERT_TIER_WARNING;
}

/* ── Channel 1: Speaker display (C1710 / C1720) ────────────────────────── */

static void display_show(const char *message,
                          const char *text_color,
                          const char *bg_color,
                          int duration_s,
                          const char *vuser,
                          const char *vpass) {
    char emsg[512];
    ao_json_esc(message ? message : "", emsg, sizeof(emsg));

    /* API duration field is in milliseconds */
    int dur_ms = (duration_s > 0 ? duration_s : 30) * 1000;

    char body[1024];
    snprintf(body, sizeof(body),
        "{\"data\":{"
        "\"message\":\"%s\","
        "\"textColor\":\"%s\","
        "\"backgroundColor\":\"%s\","
        "\"textSize\":\"large\","
        "\"scrollSpeed\":3,"
        "\"scrollDirection\":\"fromRightToLeft\","
        "\"duration\":{\"type\":\"time\",\"value\":%d}"
        "}}",
        emsg,
        (text_color && *text_color) ? text_color : "#FFFFFF",
        (bg_color   && *bg_color)   ? bg_color   : "#CC0000",
        dur_ms);

    char cred[256];
    snprintf(cred, sizeof(cred), "%s:%s",
             vuser ? vuser : "root",
             vpass ? vpass : "");

    CURL *c = curl_easy_init();
    if (!c) {
        syslog(LOG_WARNING, "alertoutput/display: curl_easy_init failed");
        return;
    }

    struct curl_slist *hdrs = curl_slist_append(NULL, "Content-Type: application/json");
    AoBuf buf = {NULL, 0};

    curl_easy_setopt(c, CURLOPT_URL,
        "https://127.0.0.1/config/rest/speaker-display-notification/v1/simple");
    curl_easy_setopt(c, CURLOPT_USERPWD,        cred);
    curl_easy_setopt(c, CURLOPT_HTTPAUTH,        CURLAUTH_BASIC);
    curl_easy_setopt(c, CURLOPT_HTTPHEADER,      hdrs);
    curl_easy_setopt(c, CURLOPT_COPYPOSTFIELDS,  body);
    curl_easy_setopt(c, CURLOPT_SSL_VERIFYPEER,  0L);
    curl_easy_setopt(c, CURLOPT_SSL_VERIFYHOST,  0L);
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION,   ao_write_cb);
    curl_easy_setopt(c, CURLOPT_WRITEDATA,       &buf);
    curl_easy_setopt(c, CURLOPT_TIMEOUT,         5L);

    CURLcode rc  = curl_easy_perform(c);
    long http_code = -1;
    curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &http_code);
    curl_slist_free_all(hdrs);
    curl_easy_cleanup(c);
    free(buf.data);

    if (rc != CURLE_OK)
        syslog(LOG_WARNING, "alertoutput/display: curl error: %s",
               curl_easy_strerror(rc));
    else
        syslog(LOG_INFO, "alertoutput/display: http=%ld dur=%ds",
               http_code, duration_s);
}

/* ── Channel 2: Local strobe — siren_and_light.cgi ─────────────────────── */

static void strobe_start(const char *color,
                          const char *pattern,
                          int speed,
                          int intensity,
                          int duration_s,
                          const char *vuser,
                          const char *vpass) {
    char cred[256];
    snprintf(cred, sizeof(cred), "%s:%s",
             vuser ? vuser : "root",
             vpass ? vpass : "");

    char body[512];
    snprintf(body, sizeof(body),
        "{\"apiVersion\":\"1.0\",\"method\":\"start\","
        "\"params\":{\"light\":{"
        "\"pattern\":\"%s\","
        "\"speed\":%d,"
        "\"colors\":[\"%s\"],"
        "\"intensity\":%d,"
        "\"duration\":{\"unit\":\"seconds\",\"value\":%d}}}}",
        (pattern   && *pattern)   ? pattern   : "Pulse",
        (speed     > 0)           ? speed     : 2,
        (color     && *color)     ? color     : "red",
        (intensity > 0)           ? intensity : 4,
        (duration_s > 0)          ? duration_s : 30);

    struct curl_slist *hdrs = curl_slist_append(NULL, "Content-Type: application/json");
    CURL *c = curl_easy_init();
    if (!c) {
        curl_slist_free_all(hdrs);
        syslog(LOG_WARNING, "alertoutput/strobe: curl_easy_init failed");
        return;
    }

    AoBuf buf = {NULL, 0};
    curl_easy_setopt(c, CURLOPT_URL,
        "http://127.0.0.1/axis-cgi/siren_and_light.cgi");
    curl_easy_setopt(c, CURLOPT_USERPWD,        cred);
    curl_easy_setopt(c, CURLOPT_HTTPAUTH,        CURLAUTH_DIGEST);
    curl_easy_setopt(c, CURLOPT_HTTPHEADER,      hdrs);
    curl_easy_setopt(c, CURLOPT_COPYPOSTFIELDS,  body);
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION,   ao_write_cb);
    curl_easy_setopt(c, CURLOPT_WRITEDATA,       &buf);
    curl_easy_setopt(c, CURLOPT_TIMEOUT,         5L);

    CURLcode rc = curl_easy_perform(c);
    long http_code = -1;
    curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &http_code);
    curl_slist_free_all(hdrs);
    curl_easy_cleanup(c);
    free(buf.data);

    if (rc != CURLE_OK)
        syslog(LOG_WARNING, "alertoutput/strobe: curl error: %s",
               curl_easy_strerror(rc));
    else
        syslog(LOG_INFO,
               "alertoutput/strobe: http=%ld pattern=%s color=%s dur=%ds",
               http_code, pattern, color, duration_s);
}

/* ── Channel 3: D4200 remote strobe profile ─────────────────────────────── */

static void d4200_start_profile(const char *host,
                                 const char *user,
                                 const char *pass,
                                 const char *profile) {
    if (!host || !*host || !profile || !*profile) return;

    char url[256];
    snprintf(url, sizeof(url),
             "http://%s/axis-cgi/siren_and_light.cgi", host);

    char cred[256];
    snprintf(cred, sizeof(cred), "%s:%s",
             user ? user : "root",
             pass ? pass : "");

    char eprofile[128];
    ao_json_esc(profile, eprofile, sizeof(eprofile));

    char body[256];
    snprintf(body, sizeof(body),
        "{\"apiVersion\":\"1.0\",\"method\":\"startProfile\","
        "\"params\":{\"name\":\"%s\"}}",
        eprofile);

    struct curl_slist *hdrs = curl_slist_append(NULL, "Content-Type: application/json");
    CURL *c = curl_easy_init();
    if (!c) {
        curl_slist_free_all(hdrs);
        syslog(LOG_WARNING, "alertoutput/d4200: curl_easy_init failed");
        return;
    }

    AoBuf buf = {NULL, 0};
    curl_easy_setopt(c, CURLOPT_URL,           url);
    curl_easy_setopt(c, CURLOPT_USERPWD,       cred);
    curl_easy_setopt(c, CURLOPT_HTTPAUTH,       CURLAUTH_DIGEST);
    curl_easy_setopt(c, CURLOPT_HTTPHEADER,     hdrs);
    curl_easy_setopt(c, CURLOPT_COPYPOSTFIELDS, body);
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION,  ao_write_cb);
    curl_easy_setopt(c, CURLOPT_WRITEDATA,      &buf);
    curl_easy_setopt(c, CURLOPT_TIMEOUT,        5L);

    CURLcode rc = curl_easy_perform(c);
    long http_code = -1;
    curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &http_code);
    curl_slist_free_all(hdrs);
    curl_easy_cleanup(c);
    free(buf.data);

    if (rc != CURLE_OK)
        syslog(LOG_WARNING, "alertoutput/d4200: curl error: %s",
               curl_easy_strerror(rc));
    else
        syslog(LOG_INFO,
               "alertoutput/d4200: http=%ld host=%s profile=%s",
               http_code, host, profile);
}

/* ── Channel 4: Audio clip via mediaclip.cgi ────────────────────────────── */

static void audio_play_clip(int clip_id,
                              const char *vuser,
                              const char *vpass) {
    if (clip_id < 0) return;

    char url[256];
    snprintf(url, sizeof(url),
             "http://127.0.0.1/axis-cgi/mediaclip.cgi?action=play&clip=%d",
             clip_id);

    char cred[256];
    snprintf(cred, sizeof(cred), "%s:%s",
             vuser ? vuser : "root",
             vpass ? vpass : "");

    CURL *c = curl_easy_init();
    if (!c) {
        syslog(LOG_WARNING, "alertoutput/audio: curl_easy_init failed");
        return;
    }

    AoBuf buf = {NULL, 0};
    curl_easy_setopt(c, CURLOPT_URL,           url);
    curl_easy_setopt(c, CURLOPT_USERPWD,       cred);
    curl_easy_setopt(c, CURLOPT_HTTPAUTH,       CURLAUTH_DIGEST);
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION,  ao_write_cb);
    curl_easy_setopt(c, CURLOPT_WRITEDATA,      &buf);
    curl_easy_setopt(c, CURLOPT_TIMEOUT,        5L);

    CURLcode rc = curl_easy_perform(c);
    long http_code = -1;
    curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &http_code);
    curl_easy_cleanup(c);
    free(buf.data);

    if (rc != CURLE_OK)
        syslog(LOG_WARNING, "alertoutput/audio: curl error: %s",
               curl_easy_strerror(rc));
    else
        syslog(LOG_INFO,
               "alertoutput/audio: clip=%d http=%ld",
               clip_id, http_code);
}

/* ── Public API ──────────────────────────────────────────────────────────── */

void alertoutput_on_activate(const char *nws_event,
                              const char *headline,
                              const AlertOutputConfig *cfg) {
    if (!cfg) return;

    AlertTier tier = alertoutput_classify(nws_event);

    /* Build the display message: prefer headline, fall back to event type */
    char display_msg[320];
    if (headline && *headline)
        snprintf(display_msg, sizeof(display_msg), "!! %s", headline);
    else if (nws_event && *nws_event)
        snprintf(display_msg, sizeof(display_msg), "!! %s", nws_event);
    else
        snprintf(display_msg, sizeof(display_msg), "!! Weather Alert Active");

    /* ── 1. Speaker display ───────────────────────────────────────────── */
    if (cfg->display_enabled) {
        const char *bg = (tier == ALERT_TIER_WARNING)
                         ? cfg->display_bg_warning
                         : cfg->display_bg_watch;
        display_show(display_msg,
                     cfg->display_text_color[0] ? cfg->display_text_color : "#FFFFFF",
                     bg[0] ? bg : (tier == ALERT_TIER_WARNING ? "#CC0000" : "#FF8800"),
                     cfg->display_duration_s,
                     cfg->vapix_user, cfg->vapix_pass);
    }

    /* ── 2. Local strobe ─────────────────────────────────────────────── */
    if (cfg->strobe_enabled) {
        /* Warning: red, fast Pulse.  Watch: amber, slow Pulse. */
        const char *color   = (tier == ALERT_TIER_WARNING) ? "red"   : "amber";
        int         speed   = (tier == ALERT_TIER_WARNING) ? 3       : 1;
        int         intens  = (tier == ALERT_TIER_WARNING) ? 5       : 4;
        strobe_start(color, "Pulse", speed, intens,
                     cfg->strobe_duration_s,
                     cfg->vapix_user, cfg->vapix_pass);
    }

    /* ── 3. D4200 remote strobe profile ──────────────────────────────── */
    if (cfg->d4200_enabled) {
        const char *profile = (tier == ALERT_TIER_WARNING)
                              ? cfg->d4200_warning_profile
                              : cfg->d4200_watch_profile;
        d4200_start_profile(cfg->d4200_host,
                            cfg->d4200_user,
                            cfg->d4200_pass,
                            profile);
    }

    /* ── 4. Audio clip ───────────────────────────────────────────────── */
    if (cfg->audio_enabled) {
        int clip = (tier == ALERT_TIER_WARNING)
                   ? cfg->audio_clip_warning
                   : cfg->audio_clip_watch;
        audio_play_clip(clip, cfg->vapix_user, cfg->vapix_pass);
    }
}

void alertoutput_on_clear(const char *nws_event,
                           const AlertOutputConfig *cfg) {
    (void)nws_event;
    /* The strobe self-terminates via its duration; the speaker display
     * expires automatically.  Reserved for future explicit-stop support. */
    (void)cfg;
}
