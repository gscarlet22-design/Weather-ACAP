#include "overlay.h"
#include "weather_api.h"
#include "vapix.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>
#include <time.h>

/* ── Template rendering ─────────────────────────────────────────────────── */

/* Append src into dst up to dst_sz, NUL-terminated. */
static void append(char *dst, size_t dst_sz, const char *src) {
    size_t cur = strlen(dst);
    size_t add = strlen(src);
    if (cur + add + 1 > dst_sz) add = (dst_sz > cur + 1) ? dst_sz - cur - 1 : 0;
    memcpy(dst + cur, src, add);
    dst[cur + add] = '\0';
}

static const char *lookup_var(const char *key, const WeatherSnapshot *snap,
                              const char *alert_event, char *scratch) {
    /* scratch is 64 bytes, caller-provided */
    if (strcmp(key, "temp") == 0) {
        snprintf(scratch, 64, "%.0f", snap->conditions.temp_f);
        return scratch;
    }
    if (strcmp(key, "temp_f") == 0) {
        snprintf(scratch, 64, "%.1f", snap->conditions.temp_f);
        return scratch;
    }
    if (strcmp(key, "cond") == 0 || strcmp(key, "description") == 0) {
        return snap->conditions.description;
    }
    if (strcmp(key, "wind") == 0) {
        snprintf(scratch, 64, "%.0f", snap->conditions.wind_speed_mph);
        return scratch;
    }
    if (strcmp(key, "dir") == 0) {
        return weather_wind_dir_str(snap->conditions.wind_dir_deg);
    }
    if (strcmp(key, "wind_arrow") == 0 || strcmp(key, "arrow") == 0) {
        return weather_wind_dir_arrow(snap->conditions.wind_dir_deg);
    }
    if (strcmp(key, "hum") == 0 || strcmp(key, "humidity") == 0) {
        snprintf(scratch, 64, "%d", snap->conditions.humidity_pct);
        return scratch;
    }
    if (strcmp(key, "provider") == 0) {
        return snap->conditions.provider;
    }
    if (strcmp(key, "sunrise") == 0) {
        return snap->conditions.sunrise[0] ? snap->conditions.sunrise : "--:--";
    }
    if (strcmp(key, "sunset") == 0) {
        return snap->conditions.sunset[0] ? snap->conditions.sunset : "--:--";
    }
    if (strcmp(key, "lat") == 0) {
        snprintf(scratch, 64, "%.4f", snap->lat);
        return scratch;
    }
    if (strcmp(key, "lon") == 0) {
        snprintf(scratch, 64, "%.4f", snap->lon);
        return scratch;
    }
    if (strcmp(key, "time") == 0) {
        time_t now = time(NULL);
        struct tm tm;
        gmtime_r(&now, &tm);
        strftime(scratch, 64, "%H:%M UTC", &tm);
        return scratch;
    }
    if (strcmp(key, "alert_type") == 0) {
        return alert_event ? alert_event : "";
    }
    return "";
}

/* Expand {var} placeholders from template into out. */
static void render_template(const char *tmpl,
                            const WeatherSnapshot *snap,
                            const char *alert_event,
                            char *out, size_t outlen) {
    out[0] = '\0';
    if (!tmpl) return;
    char scratch[64];

    const char *p = tmpl;
    while (*p) {
        if (*p == '{') {
            const char *end = strchr(p + 1, '}');
            if (end && end - p < 32) {
                char key[32];
                size_t klen = (size_t)(end - p - 1);
                memcpy(key, p + 1, klen);
                key[klen] = '\0';
                const char *v = lookup_var(key, snap, alert_event, scratch);
                append(out, outlen, v);
                p = end + 1;
                continue;
            }
        }
        /* append single char */
        char tmp[2] = { *p, 0 };
        append(out, outlen, tmp);
        p++;
    }
}

void overlay_render_text(const WeatherSnapshot *snap,
                         const OverlayConfig *cfg,
                         char *out, size_t outlen) {
    out[0] = '\0';
    if (!cfg) return;

    /* Alert prefix — fold up to max_alerts alert events */
    if (snap->alerts.count > 0 && cfg->alert_template && *cfg->alert_template) {
        char joined[256] = "";
        int  max = cfg->max_alerts > 0 ? cfg->max_alerts : 3;
        for (int i = 0; i < snap->alerts.count && i < max; i++) {
            if (i > 0) append(joined, sizeof(joined), " | ");
            append(joined, sizeof(joined), snap->alerts.alerts[i].event);
        }
        char prefix[384];
        render_template(cfg->alert_template, snap, joined, prefix, sizeof(prefix));
        append(out, outlen, prefix);
    }

    /* Main body */
    char body[384];
    render_template(cfg->template_str ? cfg->template_str : "", snap, "",
                    body, sizeof(body));
    append(out, outlen, body);
}

/* ── VAPIX overlay REST API ─────────────────────────────────────────────── */

#ifndef CGI_NO_CURL
#include <curl/curl.h>
#include "cJSON.h"

static int g_overlay_id = -1;       /* -1 = not yet created */
static int g_has_video  = -1;
/* Which JSON key the firmware uses for the overlay handle in
 * setText/remove params. Modern (OS 12.9+) wants "identity"; some older
 * docs used "identifier". We default to "identity" and downgrade to
 * "identifier" only if addText returned the legacy key. */
static const char *g_id_key = "identity";

static size_t write_cb(void *ptr, size_t sz, size_t nmemb, void *ud) {
    char **pp = (char **)ud;
    size_t n = sz * nmemb;
    size_t cur = *pp ? strlen(*pp) : 0;
    char *p = realloc(*pp, cur + n + 1);
    if (!p) return 0;
    memcpy(p + cur, ptr, n);
    p[cur + n] = '\0';
    *pp = p;
    return n;
}

static int has_video(const char *user, const char *pass) {
    if (g_has_video < 0) g_has_video = vapix_has_video(user, pass);
    return g_has_video;
}

/* Normalize position to the camelCase form the JSON API expects.
 * Accepts hyphenated input too so legacy configs keep working. */
static const char *map_position(const char *pos) {
    if (!pos || !*pos)                    return "topLeft";
    if (strcmp(pos, "top-left") == 0)     return "topLeft";
    if (strcmp(pos, "top-right") == 0)    return "topRight";
    if (strcmp(pos, "bottom-left") == 0)  return "bottomLeft";
    if (strcmp(pos, "bottom-right") == 0) return "bottomRight";
    return pos;
}

/* Minimal JSON string escaper — same shape as webhook.c::escape_json.
 * Drops control chars (<0x20) rather than emitting \uXXXX since the
 * overlay text never carries any. UTF-8 multibyte passes through. */
static void escape_json(const char *in, char *out, size_t outlen) {
    size_t j = 0;
    if (!in) in = "";
    for (size_t i = 0; in[i] && j + 2 < outlen; i++) {
        unsigned char c = (unsigned char)in[i];
        if (c == '"' || c == '\\') {
            if (j + 3 >= outlen) break;
            out[j++] = '\\';
            out[j++] = c;
        } else if (c < 0x20) {
            continue;
        } else {
            out[j++] = c;
        }
    }
    out[j] = '\0';
}

/* Parse the modern JSON-RPC response.  Observed schemas in the wild:
 *   AXIS OS 12.9+  → {"apiVersion":"1.8","data":{"camera":1,"identity":N},"method":"addText"}
 *   Earlier docs   → {"apiVersion":"1.0","data":{"identifier":N}}
 * Error shape     → {"error":{"code":N,"message":"..."}}
 * We accept either key so the app survives firmware schema drift. */
static int parse_identifier_json(const char *resp, int *out) {
    if (!resp) return 0;
    cJSON *root = cJSON_Parse(resp);
    if (!root) return 0;

    int ok = 0;
    cJSON *data = cJSON_GetObjectItem(root, "data");
    if (data) {
        cJSON *id = cJSON_GetObjectItem(data, "identity");
        if (cJSON_IsNumber(id)) {
            g_id_key = "identity";
        } else {
            id = cJSON_GetObjectItem(data, "identifier");
            if (cJSON_IsNumber(id)) g_id_key = "identifier";
        }
        if (cJSON_IsNumber(id)) {
            *out = (int)id->valuedouble;
            ok = 1;
        }
    }
    cJSON_Delete(root);
    return ok;
}

/*
 * Overlay push via the modern AXIS Dynamic Overlay JSON-RPC API.
 *
 * Two prior approaches failed:
 *   1. PUT /vapix/overlays/text/<id>  — endpoint never existed (HTTP 404).
 *   2. GET /axis-cgi/dynamicoverlay/dynamicoverlay.cgi?action=addtext&...
 *      — legacy form on AXIS OS 10. On 11+/12 the same URL responds with
 *      HTTP 200 + {"error":{"code":200,"message":"JSON input error"}}.
 *
 * The current contract (CV25, ARTPEC-8/9, AXIS OS 11+) is JSON-RPC POST:
 *
 *   POST /axis-cgi/dynamicoverlay/dynamicoverlay.cgi
 *   Content-Type: application/json
 *
 *   {"apiVersion":"1.0","method":"addText",
 *    "params":{"camera":1,"position":"topLeft","text":"..."}}
 *
 *   → {"apiVersion":"1.8","data":{"camera":1,"identity":N}}
 *
 *   {"apiVersion":"1.0","method":"setText",
 *    "params":{"identity":N,"text":"..."}}
 *
 *   {"apiVersion":"1.0","method":"remove","params":{"identity":N}}
 *
 * IMPORTANT: AXIS OS 12.9.57 rejects `params.identifier` with error
 *   {"code":103,"message":"Unknown parameter supplied"} — the modern
 *   request key is `identity` (matches the addText response key). Older
 *   AXIS OS 11.x docs spelled it `identifier`. We track which key the
 *   firmware echoed in addText and reuse the same one for setText/remove,
 *   so the app works across both spellings without a probe.
 *
 * Text is JSON-string-escaped (quotes, backslashes); UTF-8 glyphs pass
 * through untouched.
 */

#define OVERLAY_URL "http://localhost/axis-cgi/dynamicoverlay/dynamicoverlay.cgi"

void overlay_update(const WeatherSnapshot *snap,
                    const OverlayConfig *cfg,
                    const char *vapix_user,
                    const char *vapix_pass) {
    if (!cfg || !cfg->enabled) return;
    if (!snap->conditions.valid) return;
    if (!has_video(vapix_user, vapix_pass)) return;

    char text[300];
    overlay_render_text(snap, cfg, text, sizeof(text));

    char esc[600];
    escape_json(text, esc, sizeof(esc));

    const char *pos = map_position(cfg->position);

    char body[1024];
    if (g_overlay_id >= 0) {
        snprintf(body, sizeof(body),
            "{\"apiVersion\":\"1.0\",\"method\":\"setText\","
             "\"params\":{\"%s\":%d,\"text\":\"%s\"}}",
            g_id_key, g_overlay_id, esc);
    } else {
        snprintf(body, sizeof(body),
            "{\"apiVersion\":\"1.0\",\"method\":\"addText\","
             "\"params\":{\"camera\":1,\"position\":\"%s\",\"text\":\"%s\"}}",
            pos, esc);
    }

    CURL *curl = curl_easy_init();
    if (!curl) return;

    char userpwd[256];
    snprintf(userpwd, sizeof(userpwd), "%s:%s",
             vapix_user ? vapix_user : "", vapix_pass ? vapix_pass : "");

    struct curl_slist *hdrs = curl_slist_append(NULL,
        "Content-Type: application/json");

    char *resp = NULL;
    curl_easy_setopt(curl, CURLOPT_URL,           OVERLAY_URL);
    curl_easy_setopt(curl, CURLOPT_HTTPAUTH,      CURLAUTH_DIGEST);
    curl_easy_setopt(curl, CURLOPT_USERPWD,       userpwd);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER,    hdrs);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS,    body);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT,       10L);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA,     &resp);

    curl_easy_perform(curl);
    long code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &code);
    curl_slist_free_all(hdrs);
    curl_easy_cleanup(curl);

    if (code == 200 && resp) {
        if (g_overlay_id < 0) {
            int new_id = -1;
            if (parse_identifier_json(resp, &new_id)) {
                g_overlay_id = new_id;
                syslog(LOG_INFO, "overlay: created identifier=%d position=%s",
                       g_overlay_id, pos);
            } else {
                syslog(LOG_WARNING,
                       "overlay: addText HTTP 200 but no identifier in response: %.180s",
                       resp);
            }
        } else if (strstr(resp, "\"error\"")) {
            /* setText reported an error — most likely the identifier is
             * stale (camera reboot, overlay cleared by another app).
             * Drop it so the next tick re-runs addText. */
            syslog(LOG_WARNING,
                   "overlay: setText id=%d returned error, will re-add: %.180s",
                   g_overlay_id, resp);
            g_overlay_id = -1;
        }
    } else if (code != 200) {
        syslog(LOG_WARNING, "overlay: update HTTP %ld body=%.180s",
               code, resp ? resp : "(null)");
        if (code == 400 || code == 401 || code == 404) g_overlay_id = -1;
    }

    free(resp);
}

void overlay_delete(const char *vapix_user, const char *vapix_pass) {
    if (g_overlay_id < 0) return;

    char body[128];
    snprintf(body, sizeof(body),
        "{\"apiVersion\":\"1.0\",\"method\":\"remove\","
         "\"params\":{\"%s\":%d}}",
        g_id_key, g_overlay_id);

    CURL *curl = curl_easy_init();
    if (!curl) return;

    char userpwd[256];
    snprintf(userpwd, sizeof(userpwd), "%s:%s",
             vapix_user ? vapix_user : "", vapix_pass ? vapix_pass : "");

    struct curl_slist *hdrs = curl_slist_append(NULL,
        "Content-Type: application/json");

    curl_easy_setopt(curl, CURLOPT_URL,        OVERLAY_URL);
    curl_easy_setopt(curl, CURLOPT_HTTPAUTH,   CURLAUTH_DIGEST);
    curl_easy_setopt(curl, CURLOPT_USERPWD,    userpwd);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, hdrs);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT,    5L);

    curl_easy_perform(curl);
    curl_slist_free_all(hdrs);
    curl_easy_cleanup(curl);
    g_overlay_id = -1;
}

#else /* CGI_NO_CURL — overlay push not available, render-only mode */

void overlay_update(const WeatherSnapshot *snap,
                    const OverlayConfig *cfg,
                    const char *vapix_user,
                    const char *vapix_pass) {
    (void)snap; (void)cfg; (void)vapix_user; (void)vapix_pass;
}

void overlay_delete(const char *vapix_user, const char *vapix_pass) {
    (void)vapix_user; (void)vapix_pass;
}

#endif /* CGI_NO_CURL */
