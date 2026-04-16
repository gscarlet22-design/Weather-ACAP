#include "overlay.h"
#include "weather_api.h"
#include "vapix.h"
#include "cJSON.h"

#include <curl/curl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>
#include <time.h>

static char g_overlay_id[64] = { 0 };
static int  g_has_video      = -1;

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
    if (strcmp(key, "hum") == 0 || strcmp(key, "humidity") == 0) {
        snprintf(scratch, 64, "%d", snap->conditions.humidity_pct);
        return scratch;
    }
    if (strcmp(key, "provider") == 0) {
        return snap->conditions.provider;
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

/* Escape a string for JSON inclusion. */
static void json_esc(const char *in, char *out, size_t outlen) {
    size_t j = 0;
    if (!in) in = "";
    for (size_t i = 0; in[i] && j + 2 < outlen; i++) {
        unsigned char c = (unsigned char)in[i];
        if (c == '"' || c == '\\') {
            if (j + 3 >= outlen) break;
            out[j++] = '\\'; out[j++] = c;
        } else if (c < 0x20) {
            continue;
        } else {
            out[j++] = c;
        }
    }
    out[j] = '\0';
}

void overlay_update(const WeatherSnapshot *snap,
                    const OverlayConfig *cfg,
                    const char *vapix_user,
                    const char *vapix_pass) {
    if (!cfg || !cfg->enabled) return;
    if (!snap->conditions.valid) return;
    if (!has_video(vapix_user, vapix_pass)) return;

    char text[300];
    overlay_render_text(snap, cfg, text, sizeof(text));

    char esc[400];
    json_esc(text, esc, sizeof(esc));

    const char *pos = cfg->position && *cfg->position ? cfg->position : "topLeft";

    char body[600];
    snprintf(body, sizeof(body),
             "{\"text\":\"%s\",\"position\":\"%s\",\"visible\":true}", esc, pos);

    CURL *curl = curl_easy_init();
    if (!curl) return;

    char userpwd[256];
    snprintf(userpwd, sizeof(userpwd), "%s:%s",
             vapix_user ? vapix_user : "", vapix_pass ? vapix_pass : "");

    char *resp = NULL;
    struct curl_slist *hdrs = curl_slist_append(NULL, "Content-Type: application/json");

    curl_easy_setopt(curl, CURLOPT_HTTPAUTH,      CURLAUTH_DIGEST);
    curl_easy_setopt(curl, CURLOPT_USERPWD,       userpwd);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER,    hdrs);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT,       10L);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA,     &resp);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS,    body);

    if (g_overlay_id[0]) {
        char url[256];
        snprintf(url, sizeof(url), "http://localhost/vapix/overlays/text/%s", g_overlay_id);
        curl_easy_setopt(curl, CURLOPT_URL,        url);
        curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "PUT");
    } else {
        curl_easy_setopt(curl, CURLOPT_URL, "http://localhost/vapix/overlays/text");
    }

    curl_easy_perform(curl);
    long code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &code);
    curl_slist_free_all(hdrs);
    curl_easy_cleanup(curl);

    if ((code == 200 || code == 201) && resp && !g_overlay_id[0]) {
        cJSON *root = cJSON_Parse(resp);
        cJSON *id   = root ? cJSON_GetObjectItem(root, "id") : NULL;
        if (cJSON_IsString(id))
            snprintf(g_overlay_id, sizeof(g_overlay_id), "%s", id->valuestring);
        else if (cJSON_IsNumber(id))
            snprintf(g_overlay_id, sizeof(g_overlay_id), "%.0f", id->valuedouble);
        cJSON_Delete(root);
    }
    if (code != 200 && code != 201 && code != 204)
        syslog(LOG_WARNING, "overlay: update HTTP %ld", code);

    free(resp);
}

void overlay_delete(const char *vapix_user, const char *vapix_pass) {
    if (!g_overlay_id[0]) return;

    char url[256];
    snprintf(url, sizeof(url), "http://localhost/vapix/overlays/text/%s", g_overlay_id);

    CURL *curl = curl_easy_init();
    if (!curl) return;

    char userpwd[256];
    snprintf(userpwd, sizeof(userpwd), "%s:%s",
             vapix_user ? vapix_user : "", vapix_pass ? vapix_pass : "");

    curl_easy_setopt(curl, CURLOPT_URL,           url);
    curl_easy_setopt(curl, CURLOPT_HTTPAUTH,      CURLAUTH_DIGEST);
    curl_easy_setopt(curl, CURLOPT_USERPWD,       userpwd);
    curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "DELETE");
    curl_easy_setopt(curl, CURLOPT_TIMEOUT,       5L);
    curl_easy_setopt(curl, CURLOPT_NOBODY,        1L);

    curl_easy_perform(curl);
    curl_easy_cleanup(curl);
    g_overlay_id[0] = '\0';
}
