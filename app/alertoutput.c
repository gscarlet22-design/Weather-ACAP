/*
 * alertoutput.c — Sprint 14: hardware alert output
 *
 * See alertoutput.h for the full API description.
 */

#include "alertoutput.h"
#include "cJSON.h"

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

/* ── Strobe color capability probe ──────────────────────────────────────── */

/*
 * Table of known color name → RGB values.
 * Must cover every name a siren_and_light.cgi device might return.
 */
typedef struct { const char *name; int r, g, b; } AoColor;
static const AoColor ao_palette_table[] = {
    { "red",       255,   0,   0 },
    { "green",       0, 200,   0 },
    { "blue",        0,   0, 255 },
    { "white",     255, 255, 255 },
    { "yellow",    255, 255,   0 },
    { "cyan",        0, 255, 255 },
    { "magenta",   255,   0, 255 },
    { "orange",    255, 165,   0 },
    { "amber",     255, 176,   0 },
    { "warmwhite", 255, 244, 229 },
    { "pink",      255,  20, 147 },
    { "purple",    128,   0, 128 },
    { NULL, 0, 0, 0 }
};

/* Map a color name to its RGB triple.  Returns 1 on success, 0 if unknown. */
static int ao_name_to_rgb(const char *name, int *r, int *g, int *b) {
    if (!name) return 0;
    for (int i = 0; ao_palette_table[i].name; i++) {
        if (strcasecmp(ao_palette_table[i].name, name) == 0) {
            *r = ao_palette_table[i].r;
            *g = ao_palette_table[i].g;
            *b = ao_palette_table[i].b;
            return 1;
        }
    }
    return 0;
}

/*
 * Cache: filled on the first successful probe per process lifetime.
 * The device palette never changes at runtime, so one probe is enough.
 */
static char s_warn_color[64] = "";   /* nearest to red   for Warning tier */
static char s_watch_color[64] = "";  /* nearest to amber for Watch tier   */

/*
 * Query siren_and_light.cgi getCapabilities and find the supported color
 * whose name's RGB value is nearest (Euclidean) to (tr, tg, tb).
 * Result is stored into `out` (up to outlen bytes); falls back to `fallback`.
 */
static void ao_probe_best_color(int tr, int tg, int tb,
                                const char *fallback,
                                const char *vuser, const char *vpass,
                                char *out, size_t outlen) {
    snprintf(out, outlen, "%s", fallback ? fallback : "red");

    char cred[256];
    snprintf(cred, sizeof(cred), "%s:%s",
             vuser ? vuser : "root", vpass ? vpass : "");

    static const char *req =
        "{\"apiVersion\":\"1.0\",\"method\":\"getCapabilities\",\"params\":{}}";

    struct curl_slist *hdrs = curl_slist_append(NULL,
                                                "Content-Type: application/json");
    CURL *c = curl_easy_init();
    if (!c) { curl_slist_free_all(hdrs); return; }

    AoBuf buf = {NULL, 0};
    curl_easy_setopt(c, CURLOPT_URL,
                     "http://127.0.0.1/axis-cgi/siren_and_light.cgi");
    curl_easy_setopt(c, CURLOPT_USERPWD,        cred);
    curl_easy_setopt(c, CURLOPT_HTTPAUTH,        CURLAUTH_DIGEST);
    curl_easy_setopt(c, CURLOPT_HTTPHEADER,      hdrs);
    curl_easy_setopt(c, CURLOPT_COPYPOSTFIELDS,  req);
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION,   ao_write_cb);
    curl_easy_setopt(c, CURLOPT_WRITEDATA,       &buf);
    curl_easy_setopt(c, CURLOPT_TIMEOUT,         5L);
    curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT,  3L);
    curl_easy_setopt(c, CURLOPT_NOSIGNAL,        1L);

    CURLcode rc = curl_easy_perform(c);
    long http_code = -1;
    curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &http_code);
    curl_slist_free_all(hdrs);
    curl_easy_cleanup(c);

    if (rc != CURLE_OK || http_code != 200 || !buf.data) {
        syslog(LOG_WARNING,
               "alertoutput/color-probe: curl=%s http=%ld — using fallback '%s'",
               curl_easy_strerror(rc), http_code, out);
        free(buf.data);
        return;
    }

    /* Parse: .data.capabilities.light.supportedPatterns[N].colors.possible[] */
    cJSON *root = cJSON_Parse(buf.data);
    free(buf.data);
    if (!root) {
        syslog(LOG_WARNING, "alertoutput/color-probe: JSON parse failed");
        return;
    }

    cJSON *data     = cJSON_GetObjectItem(root, "data");
    cJSON *caps     = data     ? cJSON_GetObjectItem(data,  "capabilities") : NULL;
    cJSON *light    = caps     ? cJSON_GetObjectItem(caps,  "light")        : NULL;
    cJSON *patterns = light    ? cJSON_GetObjectItem(light, "supportedPatterns") : NULL;

    int best_dist = 0x7fffffff;
    char best_name[64] = "";

    if (cJSON_IsArray(patterns)) {
        int np = cJSON_GetArraySize(patterns);
        for (int pi = 0; pi < np; pi++) {
            cJSON *pat     = cJSON_GetArrayItem(patterns, pi);
            cJSON *col_obj = pat ? cJSON_GetObjectItem(pat, "colors") : NULL;
            cJSON *possible = col_obj ? cJSON_GetObjectItem(col_obj, "possible") : NULL;
            if (!cJSON_IsArray(possible)) continue;

            int nc = cJSON_GetArraySize(possible);
            for (int ci = 0; ci < nc; ci++) {
                cJSON *ce = cJSON_GetArrayItem(possible, ci);
                if (!cJSON_IsString(ce) || !ce->valuestring) continue;

                int r = 0, g = 0, b = 0;
                if (!ao_name_to_rgb(ce->valuestring, &r, &g, &b)) continue;

                int dr = r - tr, dg = g - tg, db = b - tb;
                int dist = dr*dr + dg*dg + db*db;
                if (dist < best_dist) {
                    best_dist = dist;
                    snprintf(best_name, sizeof(best_name),
                             "%s", ce->valuestring);
                }
            }
        }
    }
    cJSON_Delete(root);

    if (best_name[0]) {
        snprintf(out, outlen, "%s", best_name);
        syslog(LOG_INFO,
               "alertoutput/color-probe: target=(%d,%d,%d) → '%s' dist=%d",
               tr, tg, tb, out, best_dist);
    } else {
        syslog(LOG_WARNING,
               "alertoutput/color-probe: palette empty/unrecognised — using '%s'",
               out);
    }
}

/*
 * Fill the per-tier color cache if not already done.
 * Warning → nearest to red   (255,   0, 0), fallback "red"
 * Watch   → nearest to amber (255, 176, 0), fallback "yellow"
 * (Yellow is the most common warm color in standard AXIS palettes.)
 */
static void ao_ensure_colors(const char *vuser, const char *vpass) {
    if (!s_warn_color[0])
        ao_probe_best_color(255,   0, 0, "red",    vuser, vpass,
                            s_warn_color,  sizeof(s_warn_color));
    if (!s_watch_color[0])
        ao_probe_best_color(255, 176, 0, "yellow", vuser, vpass,
                            s_watch_color, sizeof(s_watch_color));
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
    curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT,  3L);
    curl_easy_setopt(c, CURLOPT_NOSIGNAL,        1L);

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
    curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT,  3L);
    curl_easy_setopt(c, CURLOPT_NOSIGNAL,        1L);

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
    curl_easy_setopt(c, CURLOPT_URL,            url);
    curl_easy_setopt(c, CURLOPT_USERPWD,        cred);
    curl_easy_setopt(c, CURLOPT_HTTPAUTH,        CURLAUTH_DIGEST);
    curl_easy_setopt(c, CURLOPT_HTTPHEADER,      hdrs);
    curl_easy_setopt(c, CURLOPT_COPYPOSTFIELDS,  body);
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION,   ao_write_cb);
    curl_easy_setopt(c, CURLOPT_WRITEDATA,       &buf);
    curl_easy_setopt(c, CURLOPT_TIMEOUT,         5L);
    curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT,  3L);
    curl_easy_setopt(c, CURLOPT_NOSIGNAL,        1L);

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
    curl_easy_setopt(c, CURLOPT_URL,            url);
    curl_easy_setopt(c, CURLOPT_USERPWD,        cred);
    curl_easy_setopt(c, CURLOPT_HTTPAUTH,        CURLAUTH_DIGEST);
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION,   ao_write_cb);
    curl_easy_setopt(c, CURLOPT_WRITEDATA,       &buf);
    curl_easy_setopt(c, CURLOPT_TIMEOUT,         5L);
    curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT,  3L);
    curl_easy_setopt(c, CURLOPT_NOSIGNAL,        1L);

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
        /* Probe the device palette once to find the best-match color name.
         * Results are cached in s_warn_color / s_watch_color for the
         * lifetime of the process so subsequent alerts are instant.        */
        ao_ensure_colors(cfg->vapix_user, cfg->vapix_pass);
        const char *color  = (tier == ALERT_TIER_WARNING)
                             ? s_warn_color : s_watch_color;
        int         speed  = (tier == ALERT_TIER_WARNING) ? 3 : 1;
        int         intens = (tier == ALERT_TIER_WARNING) ? 5 : 4;
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
    (void)cfg;
    /* The strobe self-terminates via its duration; the speaker display
     * expires automatically.  Reserved for future explicit-stop support.
     *
     * Reset the color cache so a fresh probe is done on the next activation.
     * This guards against stale entries surviving a device firmware update or
     * a credential change between alerts.                                   */
    s_warn_color[0]  = '\0';
    s_watch_color[0] = '\0';
}
