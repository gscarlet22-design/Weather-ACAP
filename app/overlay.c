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

static char g_overlay_id[64] = { 0 };
static int  g_has_video      = -1;

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

/* Map UI-style position (camelCase) to the legacy dynamicoverlay.cgi's
 * hyphenated form.  Accepts already-hyphenated values unchanged so the
 * config can be set directly for edge cases.                              */
static const char *map_position(const char *pos) {
    if (!pos || !*pos)                   return "top-left";
    if (strcmp(pos, "topLeft") == 0)     return "top-left";
    if (strcmp(pos, "topRight") == 0)    return "top-right";
    if (strcmp(pos, "bottomLeft") == 0)  return "bottom-left";
    if (strcmp(pos, "bottomRight") == 0) return "bottom-right";
    return pos;
}

/* Extract the identifier from a legacy CGI response body.  Bodies look
 * like "Identifier=0\n" on success.  Returns 1 if an id was written. */
static int parse_identifier(const char *resp, char *out, size_t outlen) {
    if (!resp) return 0;
    const char *p = strstr(resp, "Identifier=");
    if (!p) return 0;
    p += 11; /* strlen("Identifier=") */
    size_t i = 0;
    while (*p && *p != '\r' && *p != '\n' && *p != ' ' && i + 1 < outlen) {
        out[i++] = *p++;
    }
    out[i] = '\0';
    return i > 0;
}

/*
 * Overlay push via the legacy VAPIX Dynamic Overlay CGI.
 *
 * The original code tried to PUT JSON to /vapix/overlays/text/<id> — that
 * endpoint does not exist on any AXIS OS version and returned HTTP 404.
 *
 * The Dynamic Overlay CGI at /axis-cgi/dynamicoverlay/dynamicoverlay.cgi
 * has been the stable contract for text overlays across ARTPEC-7/8/9 and
 * CV25, AXIS OS 10/11/12.  It uses GET with query params:
 *
 *   action=addtext&camera=1&position=top-left&text=...   → "Identifier=N"
 *   action=settext&identifier=N&text=...                 → "OK"
 *   action=remove&identifier=N                           → "OK"
 *
 * The text field is URL-encoded via curl_easy_escape so Unicode glyphs
 * (compass arrows, sun/moon symbols) round-trip correctly.
 */
void overlay_update(const WeatherSnapshot *snap,
                    const OverlayConfig *cfg,
                    const char *vapix_user,
                    const char *vapix_pass) {
    if (!cfg || !cfg->enabled) return;
    if (!snap->conditions.valid) return;
    if (!has_video(vapix_user, vapix_pass)) return;

    char text[300];
    overlay_render_text(snap, cfg, text, sizeof(text));

    CURL *curl = curl_easy_init();
    if (!curl) return;

    /* URL-encode the overlay text.  curl_easy_escape handles Unicode
     * (multi-byte UTF-8), spaces, braces, pipes — everything render
     * might emit. */
    char *enc_text = curl_easy_escape(curl, text, 0);
    if (!enc_text) { curl_easy_cleanup(curl); return; }

    const char *pos = map_position(cfg->position);

    char url[1200];
    if (g_overlay_id[0]) {
        snprintf(url, sizeof(url),
            "http://localhost/axis-cgi/dynamicoverlay/dynamicoverlay.cgi"
            "?action=settext&identifier=%s&text=%s",
            g_overlay_id, enc_text);
    } else {
        snprintf(url, sizeof(url),
            "http://localhost/axis-cgi/dynamicoverlay/dynamicoverlay.cgi"
            "?action=addtext&camera=1&position=%s&text=%s",
            pos, enc_text);
    }
    curl_free(enc_text);

    char userpwd[256];
    snprintf(userpwd, sizeof(userpwd), "%s:%s",
             vapix_user ? vapix_user : "", vapix_pass ? vapix_pass : "");

    char *resp = NULL;
    curl_easy_setopt(curl, CURLOPT_URL,           url);
    curl_easy_setopt(curl, CURLOPT_HTTPAUTH,      CURLAUTH_DIGEST);
    curl_easy_setopt(curl, CURLOPT_USERPWD,       userpwd);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT,       10L);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA,     &resp);

    curl_easy_perform(curl);
    long code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &code);
    curl_easy_cleanup(curl);

    if (code == 200 && resp && !g_overlay_id[0]) {
        if (parse_identifier(resp, g_overlay_id, sizeof(g_overlay_id))) {
            syslog(LOG_INFO, "overlay: created identifier=%s position=%s",
                   g_overlay_id, pos);
        } else {
            syslog(LOG_WARNING,
                   "overlay: addtext HTTP 200 but no Identifier in response: %.120s",
                   resp);
        }
    } else if (code != 200) {
        syslog(LOG_WARNING, "overlay: update HTTP %ld body=%.120s",
               code, resp ? resp : "(null)");
        /* If the stored id became stale (camera restart, overlay removed
         * by another app), the next call will re-addtext.  Reset it. */
        if (code == 400 || code == 404) g_overlay_id[0] = '\0';
    }

    free(resp);
}

void overlay_delete(const char *vapix_user, const char *vapix_pass) {
    if (!g_overlay_id[0]) return;

    char url[512];
    snprintf(url, sizeof(url),
        "http://localhost/axis-cgi/dynamicoverlay/dynamicoverlay.cgi"
        "?action=remove&identifier=%s",
        g_overlay_id);

    CURL *curl = curl_easy_init();
    if (!curl) return;

    char userpwd[256];
    snprintf(userpwd, sizeof(userpwd), "%s:%s",
             vapix_user ? vapix_user : "", vapix_pass ? vapix_pass : "");

    curl_easy_setopt(curl, CURLOPT_URL,           url);
    curl_easy_setopt(curl, CURLOPT_HTTPAUTH,      CURLAUTH_DIGEST);
    curl_easy_setopt(curl, CURLOPT_USERPWD,       userpwd);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT,       5L);

    curl_easy_perform(curl);
    curl_easy_cleanup(curl);
    g_overlay_id[0] = '\0';
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
