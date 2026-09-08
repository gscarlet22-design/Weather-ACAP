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
    /* {time} follows the camera's configured time zone; {utc} is the
     * pre-existing behaviour kept under a new name for anyone who relied
     * on it. */
    if (strcmp(key, "time") == 0) {
        time_t now = time(NULL);
        struct tm tm;
        localtime_r(&now, &tm);
        strftime(scratch, 64, "%H:%M", &tm);
        return scratch;
    }
    if (strcmp(key, "utc") == 0) {
        time_t now = time(NULL);
        struct tm tm;
        gmtime_r(&now, &tm);
        strftime(scratch, 64, "%H:%M UTC", &tm);
        return scratch;
    }
    if (strcmp(key, "alert_type") == 0) {
        return alert_event ? alert_event : "";
    }
    /* Sprint 12 — SPC convective risk level label */
    if (strcmp(key, "lightning") == 0) {
        return snap->lightning_risk[0] ? snap->lightning_risk : "";
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

/* ── VAPIX overlay JSON-RPC API ─────────────────────────────────────────── */

#ifndef CGI_NO_CURL
#include <curl/curl.h>
#include <unistd.h>
#include "cJSON.h"

/*
 * Protocol notes (AXIS OS 11+/12, CV25 / ARTPEC-8 / ARTPEC-9):
 *
 *   POST /axis-cgi/dynamicoverlay/dynamicoverlay.cgi   Content-Type: application/json
 *
 *   {"apiVersion":"1.0","method":"addText",
 *    "params":{"camera":1,"position":"topLeft","text":"..."}}
 *      → {"apiVersion":"1.8","data":{"camera":1,"identity":N}}
 *   {"apiVersion":"1.0","method":"setText","params":{"identity":N,"text":"..."}}
 *   {"apiVersion":"1.0","method":"remove", "params":{"identity":N}}
 *   {"apiVersion":"1.0","method":"list",   "params":{"camera":1}}
 *      → {"data":{"textOverlays":[{"identity":N,"text":"...", ...}], ...}}
 *
 * Every response is HTTP 200 — errors arrive as {"error":{"code":N,...}}.
 * Code 103 = unknown parameter (identity/identifier spelling drift between
 * firmware minors); code 300 = per-channel overlay limit reached.
 *
 * Runtime overlays created here are NOT shown in the camera's Settings →
 * Overlays page and survive an app restart; only a camera reboot clears
 * them.  That is why the overlay handle is persisted to a file under /tmp:
 * the camera wipes /tmp on reboot, so the file's lifetime exactly matches
 * the overlay it names.  Without this, every daemon restart (crash, respawn,
 * upgrade, stop/start) created one more overlay until the camera hit the
 * limit and the app went dark.
 */

#define OVERLAY_URL        "http://localhost/axis-cgi/dynamicoverlay/dynamicoverlay.cgi"
#define OVERLAY_STATE_FILE "/tmp/weather_acap_overlay.json"

static int         g_overlay_id   = -1;    /* -1 = not yet created */
static int         g_has_video    = -1;    /* -1 unknown, 0 no/unreachable, 1 yes */
static char        g_position[32] = "";    /* position the live overlay was created with */
static int         g_state_loaded = 0;
static int         g_limit_hit    = 0;     /* last addText returned error 300 */
/* Which JSON key the firmware uses for the overlay handle in setText/remove.
 * OS 12.9+ wants "identity"; older docs used "identifier".  We default to
 * "identity" and follow whatever key addText echoed back. */
static const char *g_id_key = "identity";

/* ── curl response buffer ───────────────────────────────────────────────── */

typedef struct { char *data; size_t len; } OvBuf;

static size_t write_cb(void *ptr, size_t sz, size_t nmemb, void *ud) {
    OvBuf *b = (OvBuf *)ud;
    size_t n = sz * nmemb;
    char *p = realloc(b->data, b->len + n + 1);
    if (!p) return 0;
    b->data = p;
    memcpy(b->data + b->len, ptr, n);
    b->len += n;
    b->data[b->len] = '\0';
    return n;
}

/* One JSON-RPC round trip.  resp_out is heap-allocated on success (may be
 * NULL when the body was empty); caller frees.  Returns the CURLcode so
 * transport failures can be told apart from API errors. */
static CURLcode overlay_rpc(const char *body,
                            const char *user, const char *pass,
                            long timeout_s,
                            char **resp_out, long *http_out) {
    *resp_out = NULL;
    *http_out = 0;

    CURL *curl = curl_easy_init();
    if (!curl) return CURLE_FAILED_INIT;

    char userpwd[256];
    snprintf(userpwd, sizeof(userpwd), "%s:%s",
             user ? user : "", pass ? pass : "");

    struct curl_slist *hdrs = curl_slist_append(NULL,
        "Content-Type: application/json");

    OvBuf buf = { NULL, 0 };
    curl_easy_setopt(curl, CURLOPT_URL,            OVERLAY_URL);
    curl_easy_setopt(curl, CURLOPT_HTTPAUTH,       CURLAUTH_DIGEST);
    curl_easy_setopt(curl, CURLOPT_USERPWD,        userpwd);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER,     hdrs);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS,     body);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT,        timeout_s);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 3L);
    /* NOSIGNAL: the daemon's GLib loop and signal handlers own SIGALRM;
     * without this curl's timeout silently never fires. */
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL,       1L);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION,  write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA,      &buf);

    CURLcode rc = curl_easy_perform(curl);
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, http_out);
    curl_slist_free_all(hdrs);
    curl_easy_cleanup(curl);

    if (rc != CURLE_OK) {
        free(buf.data);
        return rc;
    }
    *resp_out = buf.data;
    return CURLE_OK;
}

/* ── Persisted overlay handle ───────────────────────────────────────────── */

static void state_save(void) {
    FILE *f = fopen(OVERLAY_STATE_FILE, "w");
    if (!f) return;
    fprintf(f, "{\"id\":%d,\"key\":\"%s\",\"position\":\"%s\"}\n",
            g_overlay_id, g_id_key, g_position);
    fclose(f);
}

static void state_clear(void) {
    unlink(OVERLAY_STATE_FILE);
}

static void state_load_once(void) {
    if (g_state_loaded) return;
    g_state_loaded = 1;

    FILE *f = fopen(OVERLAY_STATE_FILE, "r");
    if (!f) return;
    char raw[256];
    size_t n = fread(raw, 1, sizeof(raw) - 1, f);
    fclose(f);
    raw[n] = '\0';

    cJSON *root = cJSON_Parse(raw);
    if (!root) return;

    cJSON *id  = cJSON_GetObjectItem(root, "id");
    cJSON *key = cJSON_GetObjectItem(root, "key");
    cJSON *pos = cJSON_GetObjectItem(root, "position");

    if (cJSON_IsNumber(id) && id->valuedouble >= 0) {
        g_overlay_id = (int)id->valuedouble;
        if (cJSON_IsString(key) && key->valuestring &&
            strcmp(key->valuestring, "identifier") == 0)
            g_id_key = "identifier";
        else
            g_id_key = "identity";
        if (cJSON_IsString(pos) && pos->valuestring)
            snprintf(g_position, sizeof(g_position), "%s", pos->valuestring);
        syslog(LOG_INFO,
               "overlay: resuming persisted overlay id=%d key=%s position=%s",
               g_overlay_id, g_id_key, g_position);
    }
    cJSON_Delete(root);
}

/* ── Response helpers ───────────────────────────────────────────────────── */

/* Return the JSON-RPC error code from a response, or 0 if none. */
static int response_error_code(const char *resp) {
    if (!resp) return 0;
    cJSON *root = cJSON_Parse(resp);
    if (!root) return 0;
    int code = 0;
    cJSON *err = cJSON_GetObjectItem(root, "error");
    if (err) {
        cJSON *c = cJSON_GetObjectItem(err, "code");
        code = cJSON_IsNumber(c) ? (int)c->valuedouble : -1;
    }
    cJSON_Delete(root);
    return code;
}

/* Extract the overlay handle from an addText response and remember which
 * key spelling the firmware uses. */
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

/* Video probe.  A positive result is cached for the life of the process.
 * A negative result is NOT cached: the most common cause is wrong VAPIX
 * credentials at first poll, and caching that would keep the overlay dark
 * even after the user fixes the password.  The probe is one localhost GET
 * per poll — negligible. */
static int has_video(const char *user, const char *pass) {
    if (g_has_video == 1) return 1;

    long code = 0;
    char *body = vapix_get(
        "/axis-cgi/param.cgi?action=list&group=Properties.Image",
        user, pass, &code);
    int yes = (code == 200 && body && strstr(body, "Properties.Image")) ? 1 : 0;
    free(body);

    if (yes) {
        if (g_has_video != 1)
            syslog(LOG_INFO, "overlay: device has video — overlay enabled");
    } else if (code == 401) {
        syslog(LOG_WARNING,
               "overlay: VAPIX auth failed (HTTP 401) — check VapixUser / "
               "VapixPass on the Advanced tab; overlay skipped this cycle");
    } else if (code == 200) {
        syslog(LOG_INFO,
               "overlay: device reports no video (Properties.Image absent); "
               "overlay skipped");
    } else {
        syslog(LOG_WARNING,
               "overlay: video probe HTTP %ld; overlay skipped this cycle", code);
    }
    g_has_video = yes;
    return yes;
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

/* Minimal JSON string escaper.  Drops control chars (<0x20) rather than
 * emitting \uXXXX since the overlay text never carries any.  UTF-8
 * multibyte passes through. */
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

/* Remove one overlay by handle.  Used by delete, position change, purge. */
static void overlay_remove_id(int id, const char *key,
                              const char *user, const char *pass) {
    char body[128];
    snprintf(body, sizeof(body),
        "{\"apiVersion\":\"1.0\",\"method\":\"remove\","
         "\"params\":{\"%s\":%d}}", key, id);

    char *resp = NULL; long code = 0;
    CURLcode rc = overlay_rpc(body, user, pass, 5L, &resp, &code);
    if (rc != CURLE_OK)
        syslog(LOG_WARNING, "overlay: remove id=%d curl error: %s",
               id, curl_easy_strerror(rc));
    else if (code != 200 || response_error_code(resp))
        syslog(LOG_WARNING, "overlay: remove id=%d HTTP %ld body=%.180s",
               id, code, resp ? resp : "(null)");
    else
        syslog(LOG_INFO, "overlay: removed id=%d", id);
    free(resp);
}

/* ── Public API ─────────────────────────────────────────────────────────── */

void overlay_update(const WeatherSnapshot *snap,
                    const OverlayConfig *cfg,
                    const char *vapix_user,
                    const char *vapix_pass) {
    if (!cfg || !cfg->enabled) return;
    if (!snap->conditions.valid) return;
    if (!has_video(vapix_user, vapix_pass)) return;

    state_load_once();

    char text[300];
    overlay_render_text(snap, cfg, text, sizeof(text));

    char esc[600];
    escape_json(text, esc, sizeof(esc));

    const char *pos = map_position(cfg->position);

    /* setText cannot move an overlay.  If the configured position changed
     * since the live overlay was created, drop it and re-add. */
    if (g_overlay_id >= 0 && g_position[0] && strcmp(g_position, pos) != 0) {
        syslog(LOG_INFO, "overlay: position changed %s → %s; re-creating",
               g_position, pos);
        overlay_remove_id(g_overlay_id, g_id_key, vapix_user, vapix_pass);
        g_overlay_id = -1;
        state_clear();
    }

    /* Up to two attempts: if setText reports a stale handle we fall
     * straight through to addText in the same tick instead of waiting a
     * full poll interval with no overlay on screen. */
    for (int attempt = 0; attempt < 2; attempt++) {
        char body[1024];
        int  adding = (g_overlay_id < 0);
        if (adding)
            snprintf(body, sizeof(body),
                "{\"apiVersion\":\"1.0\",\"method\":\"addText\","
                 "\"params\":{\"camera\":1,\"position\":\"%s\",\"text\":\"%s\"}}",
                pos, esc);
        else
            snprintf(body, sizeof(body),
                "{\"apiVersion\":\"1.0\",\"method\":\"setText\","
                 "\"params\":{\"%s\":%d,\"text\":\"%s\"}}",
                g_id_key, g_overlay_id, esc);

        char *resp = NULL; long code = 0;
        CURLcode rc = overlay_rpc(body, vapix_user, vapix_pass, 10L, &resp, &code);

        if (rc != CURLE_OK) {
            /* Transport failure — keep the handle; it is probably still valid. */
            syslog(LOG_WARNING, "overlay: %s curl error: %s",
                   adding ? "addText" : "setText", curl_easy_strerror(rc));
            free(resp);
            return;
        }
        if (code != 200) {
            syslog(LOG_WARNING, "overlay: %s HTTP %ld body=%.180s",
                   adding ? "addText" : "setText", code, resp ? resp : "(null)");
            if (code == 400 || code == 401 || code == 404) {
                g_overlay_id = -1;
                state_clear();
            }
            free(resp);
            return;
        }

        int err = response_error_code(resp);

        if (adding) {
            int new_id = -1;
            if (parse_identifier_json(resp, &new_id)) {
                g_overlay_id = new_id;
                g_limit_hit  = 0;
                snprintf(g_position, sizeof(g_position), "%s", pos);
                state_save();
                syslog(LOG_INFO, "overlay: created id=%d key=%s position=%s",
                       g_overlay_id, g_id_key, pos);
            } else if (err == 300) {
                g_limit_hit = 1;
                syslog(LOG_ERR,
                       "overlay: camera overlay limit reached (error 300) — "
                       "orphaned runtime overlays from earlier runs are "
                       "occupying every slot.  Use Diagnostics → "
                       "\"Remove all text overlays\" once, or reboot the camera.");
            } else {
                syslog(LOG_WARNING,
                       "overlay: addText HTTP 200 but no handle in response: %.180s",
                       resp ? resp : "(null)");
            }
            free(resp);
            return;
        }

        /* setText path */
        if (err) {
            syslog(LOG_WARNING,
                   "overlay: setText id=%d returned error %d, re-adding: %.180s",
                   g_overlay_id, err, resp ? resp : "");
            g_overlay_id = -1;
            state_clear();
            free(resp);
            continue;   /* second attempt → addText */
        }
        free(resp);
        return;         /* setText OK */
    }
}

void overlay_delete(const char *vapix_user, const char *vapix_pass) {
    state_load_once();
    if (g_overlay_id < 0) return;
    overlay_remove_id(g_overlay_id, g_id_key, vapix_user, vapix_pass);
    g_overlay_id = -1;
    g_position[0] = '\0';
    state_clear();
}

int overlay_purge_all(const char *vapix_user, const char *vapix_pass) {
    static const char *list_body =
        "{\"apiVersion\":\"1.0\",\"method\":\"list\",\"params\":{\"camera\":1}}";

    char *resp = NULL; long code = 0;
    CURLcode rc = overlay_rpc(list_body, vapix_user, vapix_pass, 10L, &resp, &code);
    if (rc != CURLE_OK) {
        syslog(LOG_WARNING, "overlay: list curl error: %s", curl_easy_strerror(rc));
        free(resp);
        return -1;
    }
    if (code != 200 || !resp) {
        syslog(LOG_WARNING, "overlay: list HTTP %ld body=%.180s",
               code, resp ? resp : "(null)");
        free(resp);
        return -1;
    }

    cJSON *root = cJSON_Parse(resp);
    if (!root) {
        syslog(LOG_WARNING, "overlay: list response unparseable: %.180s", resp);
        free(resp);
        return -1;
    }

    /* Collect handles first, then remove — do not mutate while iterating. */
    int ids[64]; const char *keys[64]; int n = 0;

    cJSON *data = cJSON_GetObjectItem(root, "data");
    cJSON *arr  = data ? cJSON_GetObjectItem(data, "textOverlays") : NULL;
    if (!cJSON_IsArray(arr) && data) arr = cJSON_GetObjectItem(data, "overlays");

    if (cJSON_IsArray(arr)) {
        int cnt = cJSON_GetArraySize(arr);
        for (int i = 0; i < cnt && n < 64; i++) {
            cJSON *it = cJSON_GetArrayItem(arr, i);
            cJSON *id = cJSON_GetObjectItem(it, "identity");
            const char *key = "identity";
            if (!cJSON_IsNumber(id)) {
                id  = cJSON_GetObjectItem(it, "identifier");
                key = "identifier";
            }
            if (cJSON_IsNumber(id)) {
                ids[n]  = (int)id->valuedouble;
                keys[n] = key;
                n++;
            }
        }
    } else {
        /* Schema drift — surface the raw body so the user can report it. */
        syslog(LOG_WARNING,
               "overlay: list response has no textOverlays array: %.300s", resp);
    }
    cJSON_Delete(root);
    free(resp);

    for (int i = 0; i < n; i++)
        overlay_remove_id(ids[i], keys[i], vapix_user, vapix_pass);

    g_overlay_id  = -1;
    g_position[0] = '\0';
    g_limit_hit   = 0;
    state_clear();

    syslog(LOG_NOTICE, "overlay: purge removed %d text overlay(s)", n);
    return n;
}

int overlay_video_present(void)  { return g_has_video; }
int overlay_limit_reached(void)  { return g_limit_hit; }
int overlay_current_id(void)     { return g_overlay_id; }

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

int overlay_purge_all(const char *vapix_user, const char *vapix_pass) {
    (void)vapix_user; (void)vapix_pass;
    return -1;
}

int overlay_video_present(void)  { return -1; }
int overlay_limit_reached(void)  { return 0;  }
int overlay_current_id(void)     { return -1; }

#endif /* CGI_NO_CURL */
