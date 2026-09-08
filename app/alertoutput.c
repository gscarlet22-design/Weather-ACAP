/*
 * alertoutput.c — Sprint 14: hardware alert output
 *
 * See alertoutput.h for the full API description.
 */

#include "alertoutput.h"
#include "cJSON.h"
#include "vapix.h"

#include <curl/curl.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
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

/* One HTTP round trip shared by every channel.  `method` is NULL for GET.
 * Returns CURLE_OK on transport success; *http_out and *resp_out filled. */
static CURLcode ao_request(const char *url,
                           const char *json_body,      /* NULL → GET */
                           long auth,                   /* CURLAUTH_* */
                           int  insecure_tls,
                           const char *user, const char *pass,
                           char **resp_out, long *http_out) {
    *resp_out = NULL;
    *http_out = -1;

    CURL *c = curl_easy_init();
    if (!c) return CURLE_FAILED_INIT;

    char cred[256];
    snprintf(cred, sizeof(cred), "%s:%s", user ? user : "root", pass ? pass : "");

    struct curl_slist *hdrs = NULL;
    if (json_body) {
        hdrs = curl_slist_append(NULL, "Content-Type: application/json");
        curl_easy_setopt(c, CURLOPT_HTTPHEADER,     hdrs);
        curl_easy_setopt(c, CURLOPT_COPYPOSTFIELDS, json_body);
    }

    AoBuf buf = {NULL, 0};
    curl_easy_setopt(c, CURLOPT_URL,            url);
    curl_easy_setopt(c, CURLOPT_USERPWD,        cred);
    curl_easy_setopt(c, CURLOPT_HTTPAUTH,       auth);
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION,  ao_write_cb);
    curl_easy_setopt(c, CURLOPT_WRITEDATA,      &buf);
    curl_easy_setopt(c, CURLOPT_TIMEOUT,        5L);
    curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 3L);
    curl_easy_setopt(c, CURLOPT_NOSIGNAL,       1L);
    if (insecure_tls) {
        curl_easy_setopt(c, CURLOPT_SSL_VERIFYPEER, 0L);
        curl_easy_setopt(c, CURLOPT_SSL_VERIFYHOST, 0L);
    }

    CURLcode rc = curl_easy_perform(c);
    curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, http_out);
    if (hdrs) curl_slist_free_all(hdrs);
    curl_easy_cleanup(c);

    if (rc != CURLE_OK) { free(buf.data); return rc; }
    *resp_out = buf.data;
    return CURLE_OK;
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

/* ── Strobe colour capability probe ─────────────────────────────────────── */

/* Known colour name → RGB.  Must cover every name a siren_and_light.cgi
 * device might return. */
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

/* Cache: filled by the first SUCCESSFUL probe and kept for the life of
 * the process (the palette never changes at runtime).  Empty until then;
 * a failed probe leaves it empty so the next activation retries. */
static char s_warn_color[64]  = "";   /* nearest to red   */
static char s_watch_color[64] = "";   /* nearest to amber */

/* Id of the strobe session started by the last activation, for stop. */
static int  s_strobe_id = -1;

static void ao_nearest(char (*names)[64], int n,
                       int tr, int tg, int tb, char *out, size_t outlen) {
    int best = 0x7fffffff;
    out[0] = '\0';
    for (int i = 0; i < n; i++) {
        int r, g, b;
        if (!ao_name_to_rgb(names[i], &r, &g, &b)) continue;
        int dr = r - tr, dg = g - tg, db = b - tb;
        int d = dr*dr + dg*dg + db*db;
        if (d < best) { best = d; snprintf(out, outlen, "%s", names[i]); }
    }
}

/* One getCapabilities call fills BOTH tier colours.  Returns 1 on success. */
static int ao_probe_palette(const char *vuser, const char *vpass) {
    static const char *req =
        "{\"apiVersion\":\"1.0\",\"method\":\"getCapabilities\",\"params\":{}}";

    char *resp = NULL; long code = -1;
    CURLcode rc = ao_request("http://127.0.0.1/axis-cgi/siren_and_light.cgi",
                             req, CURLAUTH_DIGEST, 0, vuser, vpass, &resp, &code);
    if (rc != CURLE_OK || code != 200 || !resp) {
        syslog(LOG_WARNING,
               "alertoutput/color-probe: curl=%s http=%ld — using fallback colours this time",
               curl_easy_strerror(rc), code);
        free(resp);
        return 0;
    }

    /* .data.capabilities.light.supportedPatterns[N].colors.possible[] */
    cJSON *root = cJSON_Parse(resp);
    free(resp);
    if (!root) {
        syslog(LOG_WARNING, "alertoutput/color-probe: JSON parse failed");
        return 0;
    }

    char names[32][64]; int n = 0;

    cJSON *data     = cJSON_GetObjectItem(root, "data");
    cJSON *caps     = data  ? cJSON_GetObjectItem(data,  "capabilities")      : NULL;
    cJSON *light    = caps  ? cJSON_GetObjectItem(caps,  "light")             : NULL;
    cJSON *patterns = light ? cJSON_GetObjectItem(light, "supportedPatterns") : NULL;

    for (cJSON *pat = cJSON_IsArray(patterns) ? patterns->child : NULL; pat; pat = pat->next) {
        cJSON *col_obj  = cJSON_GetObjectItem(pat, "colors");
        cJSON *possible = col_obj ? cJSON_GetObjectItem(col_obj, "possible") : NULL;
        for (cJSON *ce = cJSON_IsArray(possible) ? possible->child : NULL; ce && n < 32; ce = ce->next) {
            if (!cJSON_IsString(ce) || !ce->valuestring) continue;
            int dup = 0;
            for (int i = 0; i < n; i++) if (strcasecmp(names[i], ce->valuestring) == 0) { dup = 1; break; }
            if (!dup) snprintf(names[n++], 64, "%s", ce->valuestring);
        }
    }
    cJSON_Delete(root);

    if (n == 0) {
        syslog(LOG_WARNING, "alertoutput/color-probe: palette empty/unrecognised");
        return 0;
    }

    ao_nearest(names, n, 255,   0, 0, s_warn_color,  sizeof(s_warn_color));
    ao_nearest(names, n, 255, 176, 0, s_watch_color, sizeof(s_watch_color));
    if (!s_warn_color[0] || !s_watch_color[0]) {
        /* Names we don't know how to map — log them so the table can grow. */
        syslog(LOG_WARNING, "alertoutput/color-probe: no mappable colour among %d names (first: %s)",
               n, names[0]);
        s_warn_color[0] = s_watch_color[0] = '\0';
        return 0;
    }
    syslog(LOG_INFO, "alertoutput/color-probe: %d colours; warning='%s' watch='%s'",
           n, s_warn_color, s_watch_color);
    return 1;
}

/* ── Severity classification ─────────────────────────────────────────────── */

AlertTier alertoutput_classify(const char *nws_event) {
    if (!nws_event || !*nws_event) return ALERT_TIER_NONE;

    char lower[128];
    size_t i;
    for (i = 0; nws_event[i] && i < sizeof(lower) - 1; i++)
        lower[i] = (char)tolower((unsigned char)nws_event[i]);
    lower[i] = '\0';

    /* Order matters: "Extreme Cold Watch" must land on WATCH, so the
     * product-class words are tested before the intensity words. */
    if (strstr(lower, "warning"))   return ALERT_TIER_WARNING;
    if (strstr(lower, "watch"))     return ALERT_TIER_WATCH;
    if (strstr(lower, "emergency") ||
        strstr(lower, "extreme"))   return ALERT_TIER_WARNING;
    if (strstr(lower, "advisory")  ||
        strstr(lower, "statement") ||
        strstr(lower, "outlook"))   return ALERT_TIER_WATCH;

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
    char emsg[512], etc[32], ebg[32];
    ao_json_esc(message ? message : "", emsg, sizeof(emsg));
    ao_json_esc((text_color && *text_color) ? text_color : "#FFFFFF", etc, sizeof(etc));
    ao_json_esc((bg_color   && *bg_color)   ? bg_color   : "#CC0000", ebg, sizeof(ebg));

    /* API duration field is in milliseconds; clamp to a sane range so an
     * absurd config value can't overflow the multiply. */
    if (duration_s <= 0)    duration_s = 30;
    if (duration_s > 86400) duration_s = 86400;
    int dur_ms = duration_s * 1000;

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
        emsg, etc, ebg, dur_ms);

    char *resp = NULL; long code = -1;
    CURLcode rc = ao_request(
        "https://127.0.0.1/config/rest/speaker-display-notification/v1/simple",
        body, CURLAUTH_BASIC, 1, vuser, vpass, &resp, &code);
    free(resp);

    if (rc != CURLE_OK)
        syslog(LOG_WARNING, "alertoutput/display: curl error: %s", curl_easy_strerror(rc));
    else
        syslog(LOG_INFO, "alertoutput/display: http=%ld dur=%ds", code, duration_s);
}

/* ── Channel 2: Local strobe — siren_and_light.cgi ─────────────────────── */

static void strobe_start(const char *color,
                          const char *pattern,
                          int speed,
                          int intensity,
                          int duration_s,
                          const char *vuser,
                          const char *vpass) {
    char ecolor[64];
    ao_json_esc((color && *color) ? color : "red", ecolor, sizeof(ecolor));

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
        ecolor,
        (intensity > 0)           ? intensity : 4,
        (duration_s > 0)          ? duration_s : 30);

    char *resp = NULL; long code = -1;
    CURLcode rc = ao_request("http://127.0.0.1/axis-cgi/siren_and_light.cgi",
                             body, CURLAUTH_DIGEST, 0, vuser, vpass, &resp, &code);

    if (rc != CURLE_OK) {
        syslog(LOG_WARNING, "alertoutput/strobe: curl error: %s", curl_easy_strerror(rc));
    } else {
        /* Remember the session id so on_clear can stop it early. */
        s_strobe_id = -1;
        cJSON *root = resp ? cJSON_Parse(resp) : NULL;
        if (root) {
            cJSON *data = cJSON_GetObjectItem(root, "data");
            cJSON *id   = data ? cJSON_GetObjectItem(data, "id") : NULL;
            if (cJSON_IsNumber(id)) s_strobe_id = (int)id->valuedouble;
            cJSON_Delete(root);
        }
        syslog(LOG_INFO, "alertoutput/strobe: http=%ld pattern=%s color=%s dur=%ds id=%d body=%.120s",
               code, pattern, color, duration_s, s_strobe_id, resp ? resp : "");
    }
    free(resp);
}

static void strobe_stop(int id, const char *vuser, const char *vpass) {
    char body[128];
    snprintf(body, sizeof(body),
        "{\"apiVersion\":\"1.0\",\"method\":\"stop\",\"params\":{\"id\":%d}}", id);

    char *resp = NULL; long code = -1;
    CURLcode rc = ao_request("http://127.0.0.1/axis-cgi/siren_and_light.cgi",
                             body, CURLAUTH_DIGEST, 0, vuser, vpass, &resp, &code);
    if (rc != CURLE_OK)
        syslog(LOG_WARNING, "alertoutput/strobe: stop curl error: %s", curl_easy_strerror(rc));
    else
        syslog(LOG_INFO, "alertoutput/strobe: stop id=%d http=%ld", id, code);
    free(resp);
}

/* ── Channel 3: D4200 remote strobe profile ─────────────────────────────── */

static void d4200_start_profile(const char *host,
                                 const char *user,
                                 const char *pass,
                                 const char *profile) {
    if (!host || !*host || !profile || !*profile) return;
    if (!vapix_valid_host(host)) {
        syslog(LOG_WARNING, "alertoutput/d4200: invalid host \"%s\" — not contacted", host);
        return;
    }

    char url[256];
    snprintf(url, sizeof(url), "http://%s/axis-cgi/siren_and_light.cgi", host);

    char eprofile[128];
    ao_json_esc(profile, eprofile, sizeof(eprofile));

    char body[256];
    snprintf(body, sizeof(body),
        "{\"apiVersion\":\"1.0\",\"method\":\"startProfile\","
        "\"params\":{\"name\":\"%s\"}}", eprofile);

    char *resp = NULL; long code = -1;
    CURLcode rc = ao_request(url, body, CURLAUTH_DIGEST, 0, user, pass, &resp, &code);
    free(resp);

    if (rc != CURLE_OK)
        syslog(LOG_WARNING, "alertoutput/d4200: curl error: %s", curl_easy_strerror(rc));
    else
        syslog(LOG_INFO, "alertoutput/d4200: http=%ld host=%s profile=%s", code, host, profile);
}

/* ── Channel 4: Audio clip via mediaclip.cgi ────────────────────────────── */

static void audio_play_clip(int clip_id, const char *vuser, const char *vpass) {
    if (clip_id < 0) return;

    char url[256];
    snprintf(url, sizeof(url),
             "http://127.0.0.1/axis-cgi/mediaclip.cgi?action=play&clip=%d", clip_id);

    char *resp = NULL; long code = -1;
    CURLcode rc = ao_request(url, NULL, CURLAUTH_DIGEST, 0, vuser, vpass, &resp, &code);
    free(resp);

    if (rc != CURLE_OK)
        syslog(LOG_WARNING, "alertoutput/audio: curl error: %s", curl_easy_strerror(rc));
    else
        syslog(LOG_INFO, "alertoutput/audio: clip=%d http=%ld", clip_id, code);
}

/* ── Public API ──────────────────────────────────────────────────────────── */

void alertoutput_on_activate(const char *nws_event,
                              const char *headline,
                              const AlertOutputConfig *cfg) {
    if (!cfg) return;

    AlertTier tier = (cfg->forced_tier != ALERT_TIER_NONE)
                     ? cfg->forced_tier
                     : alertoutput_classify(nws_event);
    if (tier == ALERT_TIER_NONE) tier = ALERT_TIER_WARNING;

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
        /* One probe fills both tier colours; cached only on success so a
         * device that was still booting gets re-probed next time. */
        if (!s_warn_color[0] || !s_watch_color[0])
            ao_probe_palette(cfg->vapix_user, cfg->vapix_pass);

        const char *color = (tier == ALERT_TIER_WARNING)
                            ? (s_warn_color[0]  ? s_warn_color  : "red")
                            : (s_watch_color[0] ? s_watch_color : "yellow");
        int speed  = (tier == ALERT_TIER_WARNING) ? 3 : 1;
        int intens = (tier == ALERT_TIER_WARNING) ? 5 : 4;
        strobe_start(color, "Pulse", speed, intens,
                     cfg->strobe_duration_s,
                     cfg->vapix_user, cfg->vapix_pass);
    }

    /* ── 3. D4200 remote strobe profile ──────────────────────────────── */
    if (cfg->d4200_enabled) {
        const char *profile = (tier == ALERT_TIER_WARNING)
                              ? cfg->d4200_warning_profile
                              : cfg->d4200_watch_profile;
        d4200_start_profile(cfg->d4200_host, cfg->d4200_user, cfg->d4200_pass, profile);
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
    if (!cfg) return;

    /* Stop the strobe early rather than letting a long duration run on
     * after the alert has ended.  The display expires on its own. */
    if (cfg->strobe_enabled && s_strobe_id >= 0) {
        strobe_stop(s_strobe_id, cfg->vapix_user, cfg->vapix_pass);
        s_strobe_id = -1;
    }
}
