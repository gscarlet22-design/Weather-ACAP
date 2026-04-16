/*
 * config_cgi — Web UI backend for Weather ACAP
 *
 * Routed by ?action= query parameter. Each endpoint returns application/json.
 *
 * GET  action=status        → live snapshot + derived state
 * GET  action=config        → current config params
 * POST action=save          → save form-encoded config
 * GET  action=ports         → probe device virtual input port capability
 * GET  action=device        → device brand/model/firmware
 * GET  action=history       → tail of alert history (JSON array)
 * GET  action=logs          → tail of syslog entries (best-effort)
 * GET  action=preview_overlay → render overlay text against current config
 * GET  action=test_weather  → force a live weather fetch, return result
 * GET  action=test_vapix    → ping localhost VAPIX with current creds
 * POST action=fire_port     → activate a single virtual port (?port=N)
 * POST action=clear_port    → deactivate a single virtual port (?port=N)
 * POST action=clear_all     → deactivate every mapped port
 * POST action=fire_drill    → activate every enabled port for 30s then clear
 * POST action=test_webhook  → POST a sample payload to the configured URL
 * GET  action=export        → download full config as JSON
 * POST action=import        → upload full config JSON (body = JSON)
 *
 * Default (no action) → action=config (for convenience / backwards compat).
 */
#include "params.h"
#include "cJSON.h"
#include "vapix.h"
#include "weather_api.h"
#include "alerts.h"
#include "overlay.h"
#include "webhook.h"

#include <glib.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define STATUS_FILE    "/tmp/weather_acap_status.json"
#define HEARTBEAT_FILE "/tmp/weather_acap_heartbeat"
#define HISTORY_FILE   "/tmp/weather_acap_history.jsonl"

/* ── URL-decode / form parse ─────────────────────────────────────────────── */

static char *url_decode(const char *src) {
    if (!src) return strdup("");
    char *dst = (char *)malloc(strlen(src) + 1);
    char *p   = dst;
    while (*src) {
        if (*src == '+') { *p++ = ' '; src++; }
        else if (*src == '%' && src[1] && src[2]) {
            char h[3] = { src[1], src[2], 0 };
            *p++ = (char)strtol(h, NULL, 16);
            src += 3;
        } else { *p++ = *src++; }
    }
    *p = '\0';
    return dst;
}

typedef struct { char key[64]; char *value; } KV;

static int parse_kv(const char *s, KV *out, int max) {
    if (!s) return 0;
    char *copy = strdup(s);
    int   n    = 0;
    char *save = NULL;
    char *tok  = strtok_r(copy, "&", &save);
    while (tok && n < max) {
        char *eq = strchr(tok, '=');
        if (eq) {
            *eq = '\0';
            char *k = url_decode(tok);
            char *v = url_decode(eq + 1);
            snprintf(out[n].key, sizeof(out[n].key), "%s", k);
            out[n].value = v;
            free(k);
            n++;
        }
        tok = strtok_r(NULL, "&", &save);
    }
    free(copy);
    return n;
}

static const char *get_kv(KV *kv, int n, const char *key) {
    for (int i = 0; i < n; i++)
        if (strcmp(kv[i].key, key) == 0) return kv[i].value;
    return NULL;
}

static void free_kv(KV *kv, int n) {
    for (int i = 0; i < n; i++) free(kv[i].value);
}

/* ── Output helpers ─────────────────────────────────────────────────────── */

static void json_header(void) {
    printf("Content-Type: application/json\r\nCache-Control: no-cache\r\n\r\n");
}

static void err_json(const char *msg) {
    json_header();
    printf("{\"ok\":false,\"error\":\"%s\"}\n", msg ? msg : "");
}

static void ok_json(void) {
    json_header();
    printf("{\"ok\":true}\n");
}

static void json_esc_to(FILE *f, const char *in) {
    if (!in) in = "";
    for (size_t i = 0; in[i]; i++) {
        unsigned char c = (unsigned char)in[i];
        if (c == '"' || c == '\\') { fputc('\\', f); fputc(c, f); }
        else if (c < 0x20) { continue; }
        else fputc(c, f);
    }
}

/* ── File helpers ───────────────────────────────────────────────────────── */

static char *read_file(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    rewind(f);
    if (sz < 0) { fclose(f); return NULL; }
    char *buf = (char *)malloc(sz + 1);
    if (!buf) { fclose(f); return NULL; }
    size_t nr = fread(buf, 1, sz, f);
    buf[nr] = '\0';
    fclose(f);
    return buf;
}

/* Tail last N lines from a newline-delimited file. Returns heap; caller frees. */
static char *read_file_tail(const char *path, int max_lines) {
    char *all = read_file(path);
    if (!all) return strdup("");
    /* count newlines; keep last max_lines */
    int count = 0;
    for (char *p = all; *p; p++) if (*p == '\n') count++;
    if (count <= max_lines) return all;

    int skip = count - max_lines;
    char *p = all;
    while (skip > 0 && *p) {
        if (*p == '\n') skip--;
        p++;
    }
    char *tail = strdup(p);
    free(all);
    return tail;
}

/* ── Config field table ─────────────────────────────────────────────────── */

typedef struct { const char *param; const char *form; } FieldMap;
static const FieldMap FIELDS[] = {
    { "SystemEnabled",        "system_enabled" },
    { "ZipCode",              "zip" },
    { "LatOverride",          "lat_override" },
    { "LonOverride",          "lon_override" },
    { "WeatherProvider",      "weather_provider" },
    { "NWSUserAgent",         "nws_user_agent" },
    { "PollInterval",         "poll_interval" },
    { "AlertMap",             "alert_map" },
    { "OverlayEnabled",       "overlay_enabled" },
    { "OverlayPosition",      "overlay_position" },
    { "OverlayTemplate",      "overlay_template" },
    { "OverlayAlertTemplate", "overlay_alert_template" },
    { "OverlayMaxAlerts",     "overlay_max_alerts" },
    { "WebhookEnabled",       "webhook_enabled" },
    { "WebhookUrl",           "webhook_url" },
    { "WebhookOnAlertsOnly",  "webhook_on_alerts_only" },
    { "VapixUser",            "vapix_user" },
    { "VapixPass",            "vapix_pass" },
    { "MockMode",             "mock_mode" },
    { NULL, NULL }
};

/* ── Endpoints ──────────────────────────────────────────────────────────── */

static void endpoint_config(void) {
    json_header();
    printf("{\n");
    for (int i = 0; FIELDS[i].param; i++) {
        char *v = params_get(FIELDS[i].param);
        /* Don't echo vapix password — send empty if set, or "" if unset. */
        int is_secret = (strcmp(FIELDS[i].param, "VapixPass") == 0);
        printf("  \"%s\": \"", FIELDS[i].form);
        if (is_secret) {
            if (v && *v) printf("__SET__");
        } else {
            json_esc_to(stdout, v);
        }
        printf("\"%s\n", FIELDS[i + 1].param ? "," : "");
        free(v);
    }
    printf("}\n");
}

static void endpoint_status(void) {
    json_header();
    char *status = read_file(STATUS_FILE);
    char *hb     = read_file(HEARTBEAT_FILE);
    long  hb_ts  = hb ? atol(hb) : 0;
    printf("{\n  \"snapshot\": %s,\n  \"last_heartbeat\": %ld\n}\n",
           status ? status : "{}", hb_ts);
    free(status);
    free(hb);
}

static void endpoint_save(const char *body) {
    KV kv[64] = {0};
    int n = parse_kv(body, kv, 64);

    int saved = 0, errors = 0;
    for (int i = 0; i < n; i++) {
        const char *param_name = NULL;
        for (int m = 0; FIELDS[m].param; m++) {
            if (strcmp(kv[i].key, FIELDS[m].form) == 0) {
                param_name = FIELDS[m].param;
                break;
            }
        }
        if (!param_name) continue;

        /* Don't overwrite password with placeholder value */
        if (strcmp(param_name, "VapixPass") == 0
            && strcmp(kv[i].value, "__SET__") == 0) continue;

        GError *err = NULL;
        if (params_set(param_name, kv[i].value, &err)) saved++;
        else { errors++; if (err) g_error_free(err); }
    }
    free_kv(kv, n);

    json_header();
    printf("{\"ok\":%s,\"saved\":%d,\"errors\":%d}\n",
           errors == 0 ? "true" : "false", saved, errors);
}

static void endpoint_ports(void) {
    char *u = params_get("VapixUser");
    char *p = params_get("VapixPass");
    int max_ports = 32;
    vapix_probe_virtual_ports(u, p, &max_ports);
    free(u); free(p);
    json_header();
    printf("{\"max_ports\":%d}\n", max_ports);
}

static void endpoint_device(void) {
    char *u = params_get("VapixUser");
    char *p = params_get("VapixPass");
    char *info = vapix_device_info(u, p);
    json_header();
    printf("{\"raw\":\"");
    json_esc_to(stdout, info ? info : "");
    printf("\"}\n");
    free(info); free(u); free(p);
}

static void endpoint_history(void) {
    json_header();
    char *tail = read_file_tail(HISTORY_FILE, 50);
    printf("{\"entries\":[");
    if (tail && *tail) {
        int first = 1;
        char *line = strtok(tail, "\n");
        while (line) {
            if (*line) {
                printf("%s%s", first ? "" : ",", line);
                first = 0;
            }
            line = strtok(NULL, "\n");
        }
    }
    printf("]}\n");
    free(tail);
}

static void endpoint_logs(void) {
    json_header();
    /* Best-effort tail of messages. Many ACAPs don't have /var/log/messages
     * readable; so fall back to syslog recent write to our own STATUS_FILE
     * as an alternative. This endpoint is primarily diagnostic. */
    char *log = NULL;
    const char *paths[] = { "/var/log/messages", "/var/log/syslog", "/var/log/weather_acap.log", NULL };
    for (int i = 0; paths[i] && !log; i++) {
        if (access(paths[i], R_OK) == 0)
            log = read_file_tail(paths[i], 60);
    }
    printf("{\"lines\":\"");
    json_esc_to(stdout, log ? log : "(no accessible syslog; check camera System Log via web UI)");
    printf("\"}\n");
    free(log);
}

static void endpoint_preview_overlay(void) {
    char *pos   = params_get("OverlayPosition");
    char *tmpl  = params_get("OverlayTemplate");
    char *atmpl = params_get("OverlayAlertTemplate");
    int   max   = params_get_int("OverlayMaxAlerts", 3);

    /* Use latest status snapshot for realistic preview */
    char *snap_raw = read_file(STATUS_FILE);
    WeatherSnapshot snap;
    memset(&snap, 0, sizeof(snap));
    snap.conditions.valid = 1;
    snprintf(snap.conditions.description, sizeof(snap.conditions.description), "Sample Conditions");
    snprintf(snap.conditions.provider,    sizeof(snap.conditions.provider), "preview");
    snap.conditions.temp_f         = 72.0;
    snap.conditions.wind_speed_mph = 8.0;
    snap.conditions.wind_dir_deg   = 225;
    snap.conditions.humidity_pct   = 55;

    if (snap_raw) {
        cJSON *root = cJSON_Parse(snap_raw);
        cJSON *ss   = root ? cJSON_GetObjectItem(root, "snapshot") : NULL;
        cJSON *cond = ss   ? cJSON_GetObjectItem(ss, "conditions") : NULL;
        if (cond) {
            cJSON *t = cJSON_GetObjectItem(cond, "temp_f");
            cJSON *d = cJSON_GetObjectItem(cond, "description");
            cJSON *w = cJSON_GetObjectItem(cond, "wind_speed_mph");
            cJSON *wd = cJSON_GetObjectItem(cond, "wind_dir_deg");
            cJSON *h = cJSON_GetObjectItem(cond, "humidity_pct");
            if (cJSON_IsNumber(t)) snap.conditions.temp_f = t->valuedouble;
            if (cJSON_IsString(d))
                snprintf(snap.conditions.description, sizeof(snap.conditions.description),
                         "%s", d->valuestring);
            if (cJSON_IsNumber(w)) snap.conditions.wind_speed_mph = w->valuedouble;
            if (cJSON_IsNumber(wd)) snap.conditions.wind_dir_deg = (int)wd->valuedouble;
            if (cJSON_IsNumber(h)) snap.conditions.humidity_pct = (int)h->valuedouble;
        }
        cJSON_Delete(root);
    }
    free(snap_raw);

    OverlayConfig cfg = {
        .enabled        = 1,
        .position       = pos,
        .template_str   = tmpl,
        .alert_template = atmpl,
        .max_alerts     = max,
    };

    char out[400];
    overlay_render_text(&snap, &cfg, out, sizeof(out));

    json_header();
    printf("{\"text\":\"");
    json_esc_to(stdout, out);
    printf("\",\"position\":\"");
    json_esc_to(stdout, pos ? pos : "topLeft");
    printf("\"}\n");

    free(pos); free(tmpl); free(atmpl);
}

static void endpoint_test_weather(void) {
    char *zip    = params_get("ZipCode");
    char *lat    = params_get("LatOverride");
    char *lon    = params_get("LonOverride");
    char *prov   = params_get("WeatherProvider");
    char *ua     = params_get("NWSUserAgent");

    WeatherSnapshot snap;
    memset(&snap, 0, sizeof(snap));
    int ok = weather_api_fetch(prov ? prov : "auto",
                               zip, lat, lon,
                               ua ? ua : "WeatherACAP/2.0",
                               &snap);

    json_header();
    printf("{\"ok\":%s,\"valid\":%s,\"provider\":\"",
           ok ? "true" : "false", snap.conditions.valid ? "true" : "false");
    json_esc_to(stdout, snap.conditions.provider);
    printf("\",\"temp_f\":%.1f,\"description\":\"", snap.conditions.temp_f);
    json_esc_to(stdout, snap.conditions.description);
    printf("\",\"wind_mph\":%.1f,\"humidity_pct\":%d,\"alert_count\":%d,"
           "\"lat\":%.6f,\"lon\":%.6f}\n",
           snap.conditions.wind_speed_mph, snap.conditions.humidity_pct,
           snap.alerts.count, snap.lat, snap.lon);

    free(zip); free(lat); free(lon); free(prov); free(ua);
}

static void endpoint_test_vapix(void) {
    char *u = params_get("VapixUser");
    char *p = params_get("VapixPass");
    long code = 0;
    char *body = vapix_get("/axis-cgi/param.cgi?action=list&group=root.Brand", u, p, &code);
    int has_video = vapix_has_video(u, p);
    int max_ports = 0;
    vapix_probe_virtual_ports(u, p, &max_ports);
    json_header();
    printf("{\"http_code\":%ld,\"ok\":%s,\"has_video\":%s,\"max_ports\":%d,\"brand_raw\":\"",
           code, (code == 200) ? "true" : "false",
           has_video ? "true" : "false", max_ports);
    json_esc_to(stdout, body ? body : "");
    printf("\"}\n");
    free(body); free(u); free(p);
}

static int parse_query_int(const char *qs, const char *key, int def) {
    if (!qs) return def;
    KV kv[16] = {0};
    int n = parse_kv(qs, kv, 16);
    const char *v = get_kv(kv, n, key);
    int r = v ? atoi(v) : def;
    free_kv(kv, n);
    return r;
}

static void endpoint_fire_port(const char *qs, int activate) {
    int port = parse_query_int(qs, "port", 0);
    if (port <= 0) { err_json("missing or invalid port"); return; }
    char *u = params_get("VapixUser");
    char *p = params_get("VapixPass");
    long code = vapix_port_set(port, activate, u, p);
    free(u); free(p);
    json_header();
    printf("{\"ok\":%s,\"port\":%d,\"activated\":%s,\"http_code\":%ld}\n",
           (code == 200) ? "true" : "false", port, activate ? "true" : "false", code);
}

static void endpoint_clear_all(void) {
    char *u   = params_get("VapixUser");
    char *p   = params_get("VapixPass");
    char *mp  = params_get("AlertMap");
    AlertMap map;
    alerts_map_parse(mp, &map);
    int cleared = 0;
    for (int i = 0; i < map.count; i++) {
        long code = vapix_port_set(map.rules[i].port, 0, u, p);
        if (code == 200) cleared++;
    }
    free(u); free(p); free(mp);
    json_header();
    printf("{\"ok\":true,\"cleared\":%d,\"total\":%d}\n", cleared, map.count);
}

static void endpoint_fire_drill(void) {
    /* Activate every enabled port; UI instructs user to call clear_all to reset. */
    char *u  = params_get("VapixUser");
    char *p  = params_get("VapixPass");
    char *mp = params_get("AlertMap");
    AlertMap map;
    alerts_map_parse(mp, &map);
    int fired = 0;
    for (int i = 0; i < map.count; i++) {
        if (!map.rules[i].enabled) continue;
        long code = vapix_port_set(map.rules[i].port, 1, u, p);
        if (code == 200) fired++;
    }
    free(u); free(p); free(mp);
    json_header();
    printf("{\"ok\":true,\"fired\":%d,\"note\":\"Ports will remain active until "
           "next weather poll clears them, or you click 'Clear all ports'.\"}\n", fired);
}

static void endpoint_test_webhook(void) {
    char *url = params_get("WebhookUrl");
    if (!url || !*url) { free(url); err_json("webhook URL not set"); return; }

    WeatherSnapshot snap;
    memset(&snap, 0, sizeof(snap));
    snap.conditions.valid = 1;
    snap.conditions.temp_f = 72.0;
    snprintf(snap.conditions.description, sizeof(snap.conditions.description),
             "Webhook Test");
    snprintf(snap.conditions.provider, sizeof(snap.conditions.provider), "test");
    long code = webhook_post(url, &snap, "webhook_test", "");
    free(url);

    json_header();
    printf("{\"ok\":%s,\"http_code\":%ld}\n", (code >= 200 && code < 300) ? "true" : "false", code);
}

static void endpoint_export(void) {
    printf("Content-Type: application/json\r\n"
           "Content-Disposition: attachment; filename=\"weather_acap_config.json\"\r\n\r\n");
    printf("{\n  \"app\": \"weather_acap\",\n  \"version\": 1,\n  \"config\": {\n");
    for (int i = 0; FIELDS[i].param; i++) {
        char *v = params_get(FIELDS[i].param);
        int is_secret = (strcmp(FIELDS[i].param, "VapixPass") == 0);
        printf("    \"%s\": \"", FIELDS[i].param);
        if (!is_secret) json_esc_to(stdout, v);
        printf("\"%s\n", FIELDS[i + 1].param ? "," : "");
        free(v);
    }
    printf("  }\n}\n");
}

static void endpoint_import(const char *body) {
    if (!body || !*body) { err_json("empty body"); return; }
    cJSON *root = cJSON_Parse(body);
    if (!root) { err_json("invalid JSON"); return; }
    cJSON *cfg = cJSON_GetObjectItem(root, "config");
    if (!cfg) { cJSON_Delete(root); err_json("missing config object"); return; }

    int saved = 0, errors = 0;
    for (int i = 0; FIELDS[i].param; i++) {
        cJSON *v = cJSON_GetObjectItem(cfg, FIELDS[i].param);
        if (!cJSON_IsString(v)) continue;
        if (strcmp(FIELDS[i].param, "VapixPass") == 0 && !*v->valuestring) continue;
        GError *e = NULL;
        if (params_set(FIELDS[i].param, v->valuestring, &e)) saved++;
        else { errors++; if (e) g_error_free(e); }
    }
    cJSON_Delete(root);

    json_header();
    printf("{\"ok\":%s,\"saved\":%d,\"errors\":%d}\n",
           errors == 0 ? "true" : "false", saved, errors);
}

/* ── Dispatcher ─────────────────────────────────────────────────────────── */

static char *read_post_body(void) {
    const char *cl_str = getenv("CONTENT_LENGTH");
    int cl = cl_str ? atoi(cl_str) : 0;
    if (cl <= 0) return strdup("");
    if (cl > 65536) cl = 65536;   /* safety cap */
    char *body = (char *)malloc(cl + 1);
    if (!body) return NULL;
    size_t r = fread(body, 1, cl, stdin);
    body[r] = '\0';
    return body;
}

static const char *query_action(const char *qs) {
    static char action[32];
    action[0] = '\0';
    if (!qs) return "";
    KV kv[16] = {0};
    int n = parse_kv(qs, kv, 16);
    const char *a = get_kv(kv, n, "action");
    if (a) snprintf(action, sizeof(action), "%s", a);
    free_kv(kv, n);
    return action;
}

int main(void) {
    GError *err = NULL;
    if (!params_init(&err)) {
        err_json("axparameter init failed");
        if (err) g_error_free(err);
        return 1;
    }

    const char *method = getenv("REQUEST_METHOD");
    const char *qs     = getenv("QUERY_STRING");
    const char *action = query_action(qs);

    if (!*action) action = "config";   /* default */

    int is_post = method && strcmp(method, "POST") == 0;

    if (strcmp(action, "status") == 0)            endpoint_status();
    else if (strcmp(action, "config") == 0)       endpoint_config();
    else if (strcmp(action, "save") == 0 && is_post) {
        char *body = read_post_body();
        endpoint_save(body ? body : "");
        free(body);
    }
    else if (strcmp(action, "ports") == 0)        endpoint_ports();
    else if (strcmp(action, "device") == 0)       endpoint_device();
    else if (strcmp(action, "history") == 0)      endpoint_history();
    else if (strcmp(action, "logs") == 0)         endpoint_logs();
    else if (strcmp(action, "preview_overlay") == 0) endpoint_preview_overlay();
    else if (strcmp(action, "test_weather") == 0) endpoint_test_weather();
    else if (strcmp(action, "test_vapix") == 0)   endpoint_test_vapix();
    else if (strcmp(action, "fire_port") == 0 && is_post)   endpoint_fire_port(qs, 1);
    else if (strcmp(action, "clear_port") == 0 && is_post)  endpoint_fire_port(qs, 0);
    else if (strcmp(action, "clear_all") == 0 && is_post)   endpoint_clear_all();
    else if (strcmp(action, "fire_drill") == 0 && is_post)  endpoint_fire_drill();
    else if (strcmp(action, "test_webhook") == 0 && is_post) endpoint_test_webhook();
    else if (strcmp(action, "export") == 0)       endpoint_export();
    else if (strcmp(action, "import") == 0 && is_post) {
        char *body = read_post_body();
        endpoint_import(body ? body : "");
        free(body);
    }
    else {
        err_json("unknown action or wrong method");
    }

    (void)ok_json;  /* referenced elsewhere if needed */
    params_cleanup();
    return 0;
}
