/*
 * weather_acap — Native ACAP v4 main daemon
 *
 * Runs a GLib event-loop with a configurable poll timer.
 * Each tick: fetch weather → update virtual inputs → update overlay →
 *            record history → optionally POST webhook → heartbeat.
 */
#include "params.h"
#include "cJSON.h"
#include "weather_api.h"
#include "alerts.h"
#include "overlay.h"
#include "history.h"
#include "webhook.h"

#include <curl/curl.h>
#include <glib.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define HEARTBEAT_FILE "/tmp/weather_acap_heartbeat"
#define STATUS_FILE    "/tmp/weather_acap_status.json"
#define CONFIG_FILE    "/tmp/weather_acap_config.json"
#define SAVE_FILE      "/tmp/weather_acap_save.json"
#define PID_FILE       "/tmp/weather_acap.pid"
#define MIN_POLL_SEC   60

static GMainLoop *g_loop     = NULL;
static guint      g_timer_id = 0;
static volatile sig_atomic_t g_reload_flag = 0;

/* ── Signal handlers ────────────────────────────────────────────────────── */

static void on_signal(int sig) {
    (void)sig;
    if (g_loop) g_main_loop_quit(g_loop);
}

static void on_sigusr1(int sig) {
    (void)sig;
    g_reload_flag = 1;   /* checked in poll loop */
}

/* ── Webhook context passed to alert callback ───────────────────────────── */

typedef struct {
    const WeatherSnapshot *snap;
    int   webhook_enabled;
    const char *webhook_url;
    int   webhook_on_alerts_only;
} TickCtx;

static void on_alert_transition(const char *event, const char *headline,
                                const char *action, int port, void *ud) {
    (void)port;
    history_append(event, headline, action);

    TickCtx *ctx = (TickCtx *)ud;
    if (ctx && ctx->webhook_enabled && ctx->webhook_url && *ctx->webhook_url) {
        char event_type[64];
        snprintf(event_type, sizeof(event_type), "alert_%s", action);
        webhook_post(ctx->webhook_url, ctx->snap, event_type, event);
    }
}

/* ── Status JSON (read by CGI) ──────────────────────────────────────────── */

static void json_esc(const char *in, char *out, size_t outlen) {
    size_t j = 0;
    if (!in) in = "";
    for (size_t i = 0; in[i] && j + 2 < outlen; i++) {
        unsigned char c = (unsigned char)in[i];
        if (c == '"' || c == '\\') {
            if (j + 3 >= outlen) break;
            out[j++] = '\\'; out[j++] = c;
        } else if (c < 0x20) { continue; }
        else out[j++] = c;
    }
    out[j] = '\0';
}

static void write_status(const WeatherSnapshot *snap,
                         const char *overlay_text,
                         int video_present,
                         const char *last_error) {
    FILE *f = fopen(STATUS_FILE, "w");
    if (!f) return;

    time_t now = time(NULL);
    char   ts[32];
    strftime(ts, sizeof(ts), "%Y-%m-%dT%H:%M:%SZ", gmtime(&now));

    char e_desc[192], e_ov[512], e_err[256];
    json_esc(snap->conditions.description, e_desc, sizeof(e_desc));
    json_esc(overlay_text ? overlay_text : "", e_ov, sizeof(e_ov));
    json_esc(last_error   ? last_error   : "", e_err, sizeof(e_err));

    fprintf(f,
        "{\n"
        "  \"last_poll\": \"%s\",\n"
        "  \"lat\": %.6f,\n"
        "  \"lon\": %.6f,\n"
        "  \"video_present\": %s,\n"
        "  \"conditions\": {\n"
        "    \"temp_f\": %.1f,\n"
        "    \"description\": \"%s\",\n"
        "    \"wind_speed_mph\": %.1f,\n"
        "    \"wind_dir_deg\": %d,\n"
        "    \"wind_dir_str\": \"%s\",\n"
        "    \"humidity_pct\": %d,\n"
        "    \"provider\": \"%s\",\n"
        "    \"valid\": %s\n"
        "  },\n"
        "  \"alert_count\": %d,\n"
        "  \"any_alert_active\": %s,\n"
        "  \"alerts\": [",
        ts,
        snap->lat, snap->lon,
        video_present ? "true" : "false",
        snap->conditions.temp_f,
        e_desc,
        snap->conditions.wind_speed_mph,
        snap->conditions.wind_dir_deg,
        weather_wind_dir_str(snap->conditions.wind_dir_deg),
        snap->conditions.humidity_pct,
        snap->conditions.provider,
        snap->conditions.valid ? "true" : "false",
        snap->alerts.count,
        alerts_any_active() ? "true" : "false");

    for (int i = 0; i < snap->alerts.count; i++) {
        char e_evt[192], e_hdl[512];
        json_esc(snap->alerts.alerts[i].event,    e_evt, sizeof(e_evt));
        json_esc(snap->alerts.alerts[i].headline, e_hdl, sizeof(e_hdl));
        fprintf(f, "%s{\"event\":\"%s\",\"headline\":\"%s\"}",
                i == 0 ? "" : ",", e_evt, e_hdl);
    }

    fprintf(f,
        "],\n"
        "  \"overlay_text\": \"%s\",\n"
        "  \"last_error\": \"%s\"\n"
        "}\n",
        e_ov, e_err);
    fclose(f);
}

/* ── Config file for CGI ────────────────────────────────────────────────── */
/* The CGI cannot use axparameter (wrong process context), so the daemon
 * writes current config to a JSON file that the CGI reads.               */

static const char *CONFIG_PARAMS[] = {
    "SystemEnabled", "ZipCode", "LatOverride", "LonOverride",
    "WeatherProvider", "NWSUserAgent", "PollInterval", "AlertMap",
    "OverlayEnabled", "OverlayPosition", "OverlayTemplate",
    "OverlayAlertTemplate", "OverlayMaxAlerts",
    "WebhookEnabled", "WebhookUrl", "WebhookOnAlertsOnly",
    "VapixUser", "VapixPass", "MockMode", NULL
};

static void write_config_file(void) {
    FILE *f = fopen(CONFIG_FILE, "w");
    if (!f) return;
    fprintf(f, "{\n");
    for (int i = 0; CONFIG_PARAMS[i]; i++) {
        char *v = params_get(CONFIG_PARAMS[i]);
        char esc[1024];
        json_esc(v, esc, sizeof(esc));
        fprintf(f, "  \"%s\": \"%s\"%s\n",
                CONFIG_PARAMS[i], esc,
                CONFIG_PARAMS[i + 1] ? "," : "");
        free(v);
    }
    fprintf(f, "}\n");
    fclose(f);
}

static void write_pid_file(void) {
    FILE *f = fopen(PID_FILE, "w");
    if (f) { fprintf(f, "%d\n", (int)getpid()); fclose(f); }
}

/* Apply a save file written by the CGI (form-encoded or JSON). */
static void apply_save_file(void) {
    char *raw = NULL;
    FILE *f = fopen(SAVE_FILE, "r");
    if (!f) return;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    rewind(f);
    if (sz > 0) {
        raw = (char *)malloc(sz + 1);
        if (raw) {
            size_t nr = fread(raw, 1, sz, f);
            raw[nr] = '\0';
        }
    }
    fclose(f);
    unlink(SAVE_FILE);   /* consume it */
    if (!raw) return;

    /* Parse JSON object — keys are param names, values are strings */
    cJSON *root = cJSON_Parse(raw);
    free(raw);
    if (!root) return;

    int attempted = 0, succeeded = 0;
    for (int i = 0; CONFIG_PARAMS[i]; i++) {
        cJSON *v = cJSON_GetObjectItem(root, CONFIG_PARAMS[i]);
        if (cJSON_IsString(v)) {
            attempted++;
            GError *e = NULL;
            gboolean ok = params_set(CONFIG_PARAMS[i], v->valuestring, &e);
            if (ok) {
                succeeded++;
            } else {
                syslog(LOG_WARNING,
                       "weather_acap: params_set(%s=\"%s\") FAILED: %s",
                       CONFIG_PARAMS[i],
                       v->valuestring ? v->valuestring : "",
                       (e && e->message) ? e->message : "(no error message)");
            }
            if (e) g_error_free(e);
        }
    }
    cJSON_Delete(root);

    /* Re-export so CGI sees updated values */
    write_config_file();
    syslog(LOG_INFO,
           "weather_acap: applied save file from CGI (%d/%d keys stored)",
           succeeded, attempted);
}

/* Short-interval callback that processes the SIGUSR1 reload flag.
 * The poll tick also honors the flag (see do_poll), but only every
 * PollInterval seconds — too slow for an interactive UI, which would
 * otherwise see stale values on its next GET /config.  This runs every
 * second and is cheap: it's a flag check followed by an early return
 * when nothing changed. */
static gboolean check_reload_cb(gpointer user_data) {
    (void)user_data;
    if (g_reload_flag) {
        g_reload_flag = 0;
        apply_save_file();
    }
    return G_SOURCE_CONTINUE;
}

/* ── Poll callback ───────────────────────────────────────────────────────── */

static gboolean do_poll(gpointer user_data) {
    (void)user_data;

    /* Check if CGI wrote a save file (SIGUSR1) */
    if (g_reload_flag) {
        g_reload_flag = 0;
        apply_save_file();
    }

    char *enabled_s = params_get("SystemEnabled");
    int enabled = enabled_s && strcasecmp(enabled_s, "yes") == 0;
    free(enabled_s);

    if (!enabled) {
        syslog(LOG_INFO, "weather_acap: SystemEnabled=no, skipping poll");
        return G_SOURCE_CONTINUE;
    }

    char *zip      = params_get("ZipCode");
    char *lat_ov   = params_get("LatOverride");
    char *lon_ov   = params_get("LonOverride");
    char *provider = params_get("WeatherProvider");
    char *ua       = params_get("NWSUserAgent");
    char *alertmap = params_get("AlertMap");
    char *vuser    = params_get("VapixUser");
    char *vpass    = params_get("VapixPass");
    char *mock     = params_get("MockMode");

    char *ov_enabled = params_get("OverlayEnabled");
    char *ov_pos     = params_get("OverlayPosition");
    char *ov_tmpl    = params_get("OverlayTemplate");
    char *ov_atmpl   = params_get("OverlayAlertTemplate");
    int   ov_max     = params_get_int("OverlayMaxAlerts", 3);

    char *wh_enabled = params_get("WebhookEnabled");
    char *wh_url     = params_get("WebhookUrl");
    char *wh_alerts  = params_get("WebhookOnAlertsOnly");

    int is_mock = mock && strcasecmp(mock, "yes") == 0;

    WeatherSnapshot snap;
    memset(&snap, 0, sizeof(snap));
    const char *last_error = "";
    int ok;

    if (is_mock) {
        syslog(LOG_INFO, "weather_acap: [MOCK] poll tick");
        snap.conditions.temp_f         = 72.0;
        snap.conditions.wind_speed_mph = 8.0;
        snap.conditions.wind_dir_deg   = 225;
        snap.conditions.humidity_pct   = 65;
        snap.conditions.valid          = 1;
        snprintf(snap.conditions.description, sizeof(snap.conditions.description),
                 "Mostly Cloudy");
        snprintf(snap.conditions.provider, sizeof(snap.conditions.provider), "mock");
        snprintf(snap.alerts.alerts[0].event, sizeof(snap.alerts.alerts[0].event),
                 "Tornado Warning");
        snprintf(snap.alerts.alerts[0].headline, sizeof(snap.alerts.alerts[0].headline),
                 "Mock tornado warning for testing");
        snap.alerts.count = 1;
        ok = 1;
    } else {
        syslog(LOG_INFO, "weather_acap: poll tick (provider=%s)", provider ? provider : "auto");
        ok = weather_api_fetch(provider ? provider : "auto",
                               zip, lat_ov, lon_ov,
                               ua ? ua : "WeatherACAP/2.0",
                               &snap);
        if (!ok) last_error = "weather fetch failed";
    }

    /* Parse current AlertMap */
    AlertMap map;
    alerts_map_parse(alertmap, &map);

    char overlay_text[400] = "";
    int  video_present = 0;

    if (ok) {
        TickCtx ctx = {
            .snap                    = &snap,
            .webhook_enabled         = wh_enabled && strcasecmp(wh_enabled, "yes") == 0,
            .webhook_url             = wh_url,
            .webhook_on_alerts_only  = wh_alerts && strcasecmp(wh_alerts, "yes") == 0,
        };

        /* Fire/clear virtual input ports */
        alerts_process(&snap, &map, vuser, vpass, on_alert_transition, &ctx);

        /* Overlay */
        OverlayConfig ocfg = {
            .enabled        = ov_enabled && strcasecmp(ov_enabled, "yes") == 0,
            .position       = ov_pos,
            .template_str   = ov_tmpl,
            .alert_template = ov_atmpl,
            .max_alerts     = ov_max,
        };
        overlay_render_text(&snap, &ocfg, overlay_text, sizeof(overlay_text));
        if (ocfg.enabled)
            overlay_update(&snap, &ocfg, vuser, vpass);

        video_present = (overlay_text[0] != '\0');

        syslog(LOG_INFO,
               "weather_acap: %.0fF %s | wind %.0fmph | alerts:%d",
               snap.conditions.temp_f,
               snap.conditions.description,
               snap.conditions.wind_speed_mph,
               snap.alerts.count);
    }

    /* Status file */
    write_status(&snap, overlay_text, video_present, last_error);

    /* Heartbeat */
    FILE *hb = fopen(HEARTBEAT_FILE, "w");
    if (hb) { fprintf(hb, "%ld\n", (long)time(NULL)); fclose(hb); }

    free(zip); free(lat_ov); free(lon_ov); free(provider);
    free(ua); free(alertmap); free(vuser); free(vpass); free(mock);
    free(ov_enabled); free(ov_pos); free(ov_tmpl); free(ov_atmpl);
    free(wh_enabled); free(wh_url); free(wh_alerts);

    return G_SOURCE_CONTINUE;
}

/* ── Entry point ─────────────────────────────────────────────────────────── */

/* ── Spawn the FastCGI child process ────────────────────────────────────────
 * The ACAP runtime starts the appName binary (this daemon) and, for ACAPs
 * with a fastCgi httpConfig entry, sets FCGI_SOCKET_NAME in its env pointing
 * to the Unix socket Apache forwards requests to.  We fork+exec the CGI
 * binary so it inherits that env var and opens the socket.  Apache's
 * 503 Service Unavailable on /local/weather_acap/weather_acap.cgi comes
 * from no process listening on the expected socket.
 *
 * We also log whether FCGI_SOCKET_NAME is present — if it is not, the runtime
 * did not pre-wire the socket for the appName process (in which case we will
 * need a different spawning strategy, e.g. runMode=never or merged binary).
 */
static pid_t g_cgi_pid = 0;

static void spawn_fastcgi_child(void) {
    const char *sock = getenv("FCGI_SOCKET_NAME");
    syslog(LOG_INFO, "weather_acap: FCGI_SOCKET_NAME=%s",
           sock ? sock : "(unset)");
    if (!sock || !*sock) {
        syslog(LOG_WARNING,
               "weather_acap: no FCGI_SOCKET_NAME in env; web UI CGI will "
               "not be spawned — expect HTTP 503 on /local/weather_acap/");
        return;
    }

    pid_t pid = fork();
    if (pid < 0) {
        syslog(LOG_ERR, "weather_acap: fork for CGI failed: %m");
        return;
    }
    if (pid == 0) {
        /* Child: exec the CGI binary from the installed package path.
         * The env (including FCGI_SOCKET_NAME) is inherited. */
        const char *cgi_path = "/usr/local/packages/weather_acap/weather_acap.cgi";
        execl(cgi_path, "weather_acap.cgi", (char *)NULL);
        /* Only reached on failure */
        syslog(LOG_ERR, "weather_acap: exec %s failed: %m", cgi_path);
        _exit(1);
    }
    g_cgi_pid = pid;
    syslog(LOG_INFO, "weather_acap: spawned FastCGI child pid=%d on socket=%s",
           (int)pid, sock);
}

static void on_sigchld(int sig) {
    (void)sig;
    /* Reap the CGI child if it exits — we only log, not respawn for now. */
    int status = 0;
    pid_t pid;
    while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
        syslog(LOG_WARNING,
               "weather_acap: CGI child pid=%d exited status=%d",
               (int)pid, status);
        if (pid == g_cgi_pid) g_cgi_pid = 0;
    }
}

int main(void) {
    openlog("weather_acap", LOG_PID | LOG_CONS, LOG_USER);
    syslog(LOG_INFO, "weather_acap: starting up (native ACAP v4)");

    signal(SIGTERM, on_signal);
    signal(SIGINT,  on_signal);
    signal(SIGUSR1, on_sigusr1);
    signal(SIGCHLD, on_sigchld);

    GError *err = NULL;
    if (!params_init(&err)) {
        syslog(LOG_ERR, "weather_acap: axparameter init failed: %s",
               err ? err->message : "unknown");
        if (err) g_error_free(err);
        return 1;
    }

    curl_global_init(CURL_GLOBAL_DEFAULT);

    /* Write PID and config file so the CGI can read them */
    write_pid_file();
    write_config_file();

    /* Spawn the FastCGI child (web UI backend).  Must happen after
     * write_pid_file / write_config_file so the CGI finds valid state
     * on its first request. */
    spawn_fastcgi_child();

    do_poll(NULL);   /* immediate first poll */

    int interval = params_get_int("PollInterval", 300);
    if (interval < MIN_POLL_SEC) interval = MIN_POLL_SEC;
    syslog(LOG_INFO, "weather_acap: poll interval %d seconds", interval);

    g_loop     = g_main_loop_new(NULL, FALSE);
    g_timer_id = g_timeout_add_seconds((guint)interval, do_poll, NULL);
    /* Fast path for CGI-triggered config reloads (SIGUSR1).  Without this,
     * Save would not appear to persist until the next poll, up to 5 min. */
    g_timeout_add(1000, check_reload_cb, NULL);

    g_main_loop_run(g_loop);

    syslog(LOG_INFO, "weather_acap: shutting down");

    if (g_timer_id) g_source_remove(g_timer_id);

    /* Clear any active ports */
    char *vuser    = params_get("VapixUser");
    char *vpass    = params_get("VapixPass");
    char *alertmap = params_get("AlertMap");
    AlertMap map;
    alerts_map_parse(alertmap, &map);
    alerts_clear_all(&map, vuser, vpass);
    overlay_delete(vuser, vpass);
    free(vuser); free(vpass); free(alertmap);

    params_cleanup();
    curl_global_cleanup();
    g_main_loop_unref(g_loop);
    closelog();
    return 0;
}
