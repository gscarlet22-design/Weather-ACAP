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
#include "condhistory.h"
#include "webhook.h"
#include "snapshot.h"
#include "mqtt.h"
#include "email.h"
#include "threshold.h"
#include "multicam.h"
#include "axisevents.h"
#include "lightning.h"
#include "jsonlog.h"
#include "alertoutput.h"
#include "version.h"
#include "vapix.h"

#include <curl/curl.h>
#include <glib.h>
#include <glib-unix.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <syslog.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define HEARTBEAT_FILE "/tmp/weather_acap_heartbeat"
#define STATUS_FILE    "/tmp/weather_acap_status.json"
#define STATUS_TMP     "/tmp/weather_acap_status.json.daemon.tmp"
#define CONFIG_FILE    "/tmp/weather_acap_config.json"
/* Temp name is process-specific: the CGI also writes CONFIG_FILE via its
 * own temp+rename, and sharing one ".tmp" path let two writers truncate
 * each other and rename garbage into place. */
#define CONFIG_TMP     "/tmp/weather_acap_config.json.daemon.tmp"
#define SAVE_FILE      "/tmp/weather_acap_save.json"
#define SAVE_CLAIMED   "/tmp/weather_acap_save.json.applying"
#define PID_FILE       "/tmp/weather_acap.pid"
#define MIN_POLL_SEC   60

static GMainLoop *g_loop          = NULL;
static guint      g_timer_id      = 0;
static int        g_poll_interval = 0;

/* Sprint 12 — SPC lightning state.  Label/level persist between checks so
 * the overlay {lightning} token and status JSON are populated on every
 * tick, not only on the ticks that hit the SPC feed. */
static int  g_lightning_active   = 0;   /* 1 = port currently activated */
static int  g_lightning_tick     = 0;   /* poll-cycle counter */
static char g_lightning_label[8] = "";  /* last risk label ("" = none) */
static int  g_lightning_level    = 0;

/* FastCGI backend child (web UI).  Watched via g_child_watch_add and
 * respawned from the GLib loop if it dies. */
#define CGI_RESPAWN_MIN_SEC 5
#define CGI_RESPAWN_MAX     10
static pid_t  g_cgi_pid         = 0;
static time_t g_cgi_last_spawn  = 0;
static int    g_cgi_respawns    = 0;
static guint  g_cgi_respawn_src = 0;

static gboolean do_poll(gpointer user_data);
static void     spawn_fastcgi_child(void);
static void     arm_poll_timer(int interval);
static void     apply_save_file(void);

/* ── Sprint 7: Notification cool-down ──────────────────────────────────── */

/* One record per event name.  Only notification channels are throttled —
 * VAPIX port transitions always reflect real-time state and are NOT
 * subject to cool-down.
 *
 * Semantics: an "activated" is suppressed if a previous activation for the
 * same event was SENT within the hold window.  A "cleared" is sent iff its
 * activation was sent — downstream consumers (webhook/MQTT/email) must see
 * matching pairs or they latch "active" forever.  Timing uses the
 * monotonic clock so an NTP step at boot cannot mute notifications. */
#define NOTIF_HISTORY_MAX 256
typedef struct {
    char   event[128];
    time_t last_sent;      /* CLOCK_MONOTONIC seconds of the last sent activation */
    int    ever_sent;
    int    active_sent;    /* the current activation was sent */
} NotifRecord;
static NotifRecord g_notif[NOTIF_HISTORY_MAX];
static int         g_notif_n = 0;

static time_t mono_now(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec;
}

/* Returns 1 if notification channels should fire for this transition. */
static int cooldown_check_and_update(const char *event, const char *action,
                                     int cooldown_min) {
    if (!event) event = "";
    int is_clear = action && strcmp(action, "cleared") == 0;

    NotifRecord *rec = NULL;
    for (int i = 0; i < g_notif_n; i++)
        if (strcmp(g_notif[i].event, event) == 0) { rec = &g_notif[i]; break; }
    if (!rec) {
        if (g_notif_n >= NOTIF_HISTORY_MAX) {
            static int warned = 0;
            if (!warned) {
                jlog(LOG_WARNING, "cooldown: record table full (%d); "
                     "further events are not throttled", NOTIF_HISTORY_MAX);
                warned = 1;
            }
            return 1;
        }
        rec = &g_notif[g_notif_n++];
        memset(rec, 0, sizeof(*rec));
        snprintf(rec->event, sizeof(rec->event), "%s", event);
    }

    if (is_clear) {
        int send = rec->active_sent;
        rec->active_sent = 0;
        return send;
    }

    time_t now  = mono_now();
    long   hold = (long)cooldown_min * 60;
    if (hold > 0 && rec->ever_sent && (now - rec->last_sent) < hold) {
        rec->active_sent = 0;
        return 0;
    }
    rec->last_sent   = now;
    rec->ever_sent   = 1;
    rec->active_sent = 1;
    return 1;
}

/* ── Context passed to the transition callback ──────────────────────────── */

typedef struct {
    const WeatherSnapshot *snap;
    int   webhook_enabled;
    const char *webhook_url;
    const char *webhook_template;   /* Sprint 11 — custom payload template */
    int   webhook_on_alerts_only;
    /* VAPIX credentials — needed by snapshot_capture */
    const char *vapix_user;
    const char *vapix_pass;
    /* Sprint 2 — snapshot on alert */
    SnapshotConfig snap_cfg;
    /* Sprint 3 — MQTT + email */
    MqttConfig  mqtt_cfg;
    EmailConfig email_cfg;
    /* Sprint 7 — notification cool-down (minutes; 0 = disabled) */
    int cooldown_min;
    /* Sprint 8 — multi-camera snapshot */
    MultiCamConfig multicam_cfg;
    const char    *multicam_resolution;
    /* Sprint 14 — hardware alert output (display, strobe, D4200, audio) */
    AlertOutputConfig alertout_cfg;
} TickCtx;

static void on_alert_transition(const char *event, const char *headline,
                                const char *action, int port, void *ud) {
    (void)port;
    if (!action) action = "";
    history_append(event, headline, action);

    /* Sprint 9 — publish native AXIS event (not gated by cool-down:
     * the event system should see every transition, same as VAPIX ports). */
    axisevents_publish_alert(event, action, headline);

    TickCtx *ctx = (TickCtx *)ud;
    if (!ctx) return;

    /* Build event_type string used by multiple notification channels */
    char event_type[64];
    snprintf(event_type, sizeof(event_type), "alert_%s", action);

    /* Sprint 7 — cool-down gate for notification channels only.
     * VAPIX port activation already happened inside alerts_process /
     * threshold_process before this callback was invoked. */
    int send_notifs = cooldown_check_and_update(event, action, ctx->cooldown_min);

    /* Snapshot capture (Sprint 2) — gated by cool-down */
    if (send_notifs)
        snapshot_capture(event, action,
                         ctx->vapix_user, ctx->vapix_pass,
                         &ctx->snap_cfg,
                         NULL, 0);

    /* Webhook — gated by cool-down (Sprint 11: pass headline + template) */
    if (send_notifs &&
        ctx->webhook_enabled && ctx->webhook_url && *ctx->webhook_url)
        webhook_post(ctx->webhook_url, ctx->snap, event_type, event,
                     headline, ctx->webhook_template);

    /* MQTT publish (Sprint 3) — gated by cool-down */
    if (send_notifs && ctx->mqtt_cfg.enabled)
        mqtt_publish(&ctx->mqtt_cfg, ctx->snap, event_type, event);

    /* Email notification (Sprint 3) — gated by cool-down */
    if (send_notifs && ctx->email_cfg.enabled) {
        int should_send = 1;
        if (strcmp(action, "cleared") == 0 && !ctx->email_cfg.on_clear)
            should_send = 0;
        if (should_send)
            email_send(&ctx->email_cfg, event_type, event, ctx->snap);
    }

    /* Sprint 8 — multi-camera snapshot — gated by cool-down */
    if (send_notifs && ctx->multicam_cfg.enabled)
        multicam_capture(event, action,
                         &ctx->multicam_cfg,
                         ctx->multicam_resolution,
                         ctx->snap_cfg.save_dir,
                         ctx->snap_cfg.max_count,
                         ctx->snap_cfg.on_activate,
                         ctx->snap_cfg.on_clear);

    if (!send_notifs)
        jlog(LOG_INFO,
               "cooldown: suppressed notifications for %s/%s (%d min hold-off)",
               event ? event : "?", action, ctx->cooldown_min);

    /* Sprint 14 — hardware output channels (NOT gated by cool-down; mirrors
     * the same policy as VAPIX virtual port activation — reflects real state) */
    if (strcmp(action, "activated") == 0)
        alertoutput_on_activate(event, headline, &ctx->alertout_cfg);
    else if (strcmp(action, "cleared") == 0)
        alertoutput_on_clear(event, &ctx->alertout_cfg);
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

/* Atomic (temp + rename): the CGI reads this file on every dashboard
 * poll and used to get an empty or truncated body mid-write. */
static void write_status(const WeatherSnapshot *snap,
                         const char *overlay_text,
                         int video_present,
                         const char *last_error) {
    FILE *f = fopen(STATUS_TMP, "w");
    if (!f) return;

    time_t now = time(NULL);
    char   ts[32];
    strftime(ts, sizeof(ts), "%Y-%m-%dT%H:%M:%SZ", gmtime(&now));

    char e_desc[192], e_ov[512], e_err[256];
    json_esc(snap->conditions.description, e_desc, sizeof(e_desc));
    json_esc(overlay_text ? overlay_text : "", e_ov, sizeof(e_ov));
    json_esc(last_error   ? last_error   : "", e_err, sizeof(e_err));

    int any_active = alerts_any_active() || threshold_any_active() || g_lightning_active;

    fprintf(f,
        "{\n"
        "  \"last_poll\": \"%s\",\n"
        "  \"version\": \"%s\",\n"
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
        "  \"alerts_fetch_ok\": %s,\n"
        "  \"any_alert_active\": %s,\n"
        "  \"alerts\": [",
        ts,
        WEATHER_ACAP_VERSION,
        snap->lat, snap->lon,
        video_present < 0 ? "null" : video_present ? "true" : "false",
        snap->conditions.temp_f,
        e_desc,
        snap->conditions.wind_speed_mph,
        snap->conditions.wind_dir_deg,
        weather_wind_dir_str(snap->conditions.wind_dir_deg),
        snap->conditions.humidity_pct,
        snap->conditions.provider,
        snap->conditions.valid ? "true" : "false",
        snap->alerts.count,
        snap->alerts.fetch_ok ? "true" : "false",
        any_active ? "true" : "false");

    for (int i = 0; i < snap->alerts.count; i++) {
        char e_evt[192], e_hdl[512];
        json_esc(snap->alerts.alerts[i].event,    e_evt, sizeof(e_evt));
        json_esc(snap->alerts.alerts[i].headline, e_hdl, sizeof(e_hdl));
        fprintf(f, "%s{\"event\":\"%s\",\"headline\":\"%s\"}",
                i == 0 ? "" : ",", e_evt, e_hdl);
    }

    fprintf(f,
        "],\n"
        "  \"lightning_risk\": \"%s\",\n"
        "  \"lightning_risk_level\": %d,\n"
        "  \"overlay_text\": \"%s\",\n"
        "  \"overlay_id\": %d,\n"
        "  \"overlay_limit_reached\": %s,\n"
        "  \"last_error\": \"%s\"\n"
        "}\n",
        snap->lightning_risk[0] ? snap->lightning_risk : "",
        snap->lightning_risk_level,
        e_ov,
        overlay_current_id(),
        overlay_limit_reached() ? "true" : "false",
        e_err);
    if (fclose(f) == 0)
        rename(STATUS_TMP, STATUS_FILE);
    else
        unlink(STATUS_TMP);
}

/* ── Config file for CGI ────────────────────────────────────────────────── */
/* The CGI cannot use the params store (wrong process context), so the
 * daemon writes current config to a JSON file that the CGI reads.       */

static const char *CONFIG_PARAMS[] = {
    "SystemEnabled", "ZipCode", "LatOverride", "LonOverride",
    "WeatherProvider", "NWSUserAgent", "PollInterval", "AlertMap",
    "OverlayEnabled", "OverlayPosition", "OverlayTemplate",
    "OverlayAlertTemplate", "OverlayMaxAlerts",
    "WebhookEnabled", "WebhookUrl", "WebhookOnAlertsOnly", "WebhookTemplate",
    "VapixUser", "VapixPass", "MockMode",
    /* Sprint 2 — snapshot on alert */
    "SnapshotEnabled", "SnapshotResolution", "SnapshotSaveDir",
    "SnapshotOnActivate", "SnapshotOnClear",
    /* Sprint 3 — MQTT + email */
    "MqttEnabled", "MqttBrokerUrl", "MqttTopic",
    "MqttUser", "MqttPass", "MqttOnAlertOnly", "MqttRetain",
    "EmailEnabled", "EmailSmtpUrl", "EmailFrom",
    "EmailTo", "EmailUser", "EmailPass", "EmailOnClear",
    /* Sprint 5 — threshold alerts + snapshot auto-delete */
    "ThresholdMap", "SnapshotMaxCount",
    /* Sprint 7 — notification cool-down */
    "AlertCooldownMin", "ThresholdCooldownMin",
    /* Sprint 8 — multi-camera snapshot */
    "MultiCamEnabled", "MultiCamList", "MultiCamResolution",
    /* Sprint 9 — native AXIS events */
    "AxisEventsEnabled",
    /* Sprint 12 — lightning alerts */
    "LightningEnabled", "LightningPort", "LightningMinRisk", "LightningPollMult",
    /* Sprint 13 — JSON logging */
    "JsonLogging",
    /* Sprint 14 — hardware alert output */
    "DisplayAlertEnabled", "DisplayAlertDuration",
    "DisplayAlertTextColor", "DisplayAlertBgWarning", "DisplayAlertBgWatch",
    "StrobeAlertEnabled", "StrobeAlertDuration",
    "D4200Enabled", "D4200Host", "D4200User", "D4200Pass",
    "D4200WarningProfile", "D4200WatchProfile",
    "AudioAlertEnabled", "AudioClipWarning", "AudioClipWatch",
    NULL
};

static void write_config_file(void) {
    /* Atomic write: temp + rename so a concurrent CGI read can never see
     * a half-written file.  The file carries credentials, so it is
     * created 0600 — both the daemon and the CGI run as the app user. */
    FILE *f = fopen(CONFIG_TMP, "w");
    if (!f) return;
    fchmod(fileno(f), 0600);
    fprintf(f, "{\n");
    for (int i = 0; CONFIG_PARAMS[i]; i++) {
        char *v = params_get(CONFIG_PARAMS[i]);
        /* Sized for the largest value (AlertMap parses into 4 KB) — the
         * old 1 KB buffer truncated long alert/multicam lists on export,
         * and the next UI save wrote the truncated value back. */
        char esc[8192];
        json_esc(v, esc, sizeof(esc));
        fprintf(f, "  \"%s\": \"%s\"%s\n",
                CONFIG_PARAMS[i], esc,
                CONFIG_PARAMS[i + 1] ? "," : "");
        free(v);
    }
    fprintf(f, "}\n");
    fflush(f);
    fclose(f);
    if (rename(CONFIG_TMP, CONFIG_FILE) != 0) unlink(CONFIG_TMP);
}

static void write_pid_file(void) {
    FILE *f = fopen(PID_FILE, "w");
    if (f) { fprintf(f, "%d\n", (int)getpid()); fclose(f); }
}

/* Apply a save file written by the CGI (JSON object of param → string). */
static void apply_save_file(void) {
    /* Claim the file by renaming it first: the old read-then-unlink had a
     * window where a second save renamed into place between the two and
     * was deleted unread. */
    if (rename(SAVE_FILE, SAVE_CLAIMED) != 0) return;

    char *raw = NULL;
    FILE *f = fopen(SAVE_CLAIMED, "r");
    if (f) {
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
    }
    unlink(SAVE_CLAIMED);
    if (!raw) return;

    cJSON *root = cJSON_Parse(raw);
    free(raw);
    if (!root) {
        jlog(LOG_WARNING, "weather_acap: save file from CGI was not valid JSON — ignored");
        return;
    }

    int applied = 0;
    for (int i = 0; CONFIG_PARAMS[i]; i++) {
        cJSON *v = cJSON_GetObjectItem(root, CONFIG_PARAMS[i]);
        if (cJSON_IsString(v)) {
            params_set_deferred(CONFIG_PARAMS[i],
                                v->valuestring ? v->valuestring : "");
            applied++;
        }
    }
    cJSON_Delete(root);

    /* One write + fsync for the whole batch — previously every key did its
     * own full-file rewrite and fsync (~70 flash syncs per Save click). */
    GError *e = NULL;
    if (!params_flush(&e)) {
        jlog(LOG_WARNING, "weather_acap: persisting %d key(s) FAILED: %s",
             applied, (e && e->message) ? e->message : "(no error message)");
        if (e) g_error_free(e);
    }

    /* Re-export so CGI sees updated values */
    write_config_file();
    jlog(LOG_INFO, "weather_acap: applied save file from CGI (%d keys)", applied);

    /* PollInterval used to take effect only after an app restart. */
    arm_poll_timer(params_get_int("PollInterval", 300));
}

/* (Re)arm the poll timer.  No-op if the interval is unchanged.  Safe to
 * call from inside do_poll itself: GLib tolerates removing the source that
 * is currently dispatching. */
static void arm_poll_timer(int interval) {
    if (interval < MIN_POLL_SEC) interval = MIN_POLL_SEC;
    if (g_timer_id && interval == g_poll_interval) return;
    if (g_timer_id) g_source_remove(g_timer_id);
    g_timer_id = g_timeout_add_seconds((guint)interval, do_poll, NULL);
    if (g_poll_interval && g_poll_interval != interval)
        jlog(LOG_INFO, "weather_acap: poll interval changed %d → %d seconds",
             g_poll_interval, interval);
    g_poll_interval = interval;
}

/* ── Signals (dispatched from the GLib loop, never from a handler) ──────── */

static gboolean on_quit_signal(gpointer ud) {
    jlog(LOG_INFO, "weather_acap: signal %d — shutting down", GPOINTER_TO_INT(ud));
    if (g_loop) g_main_loop_quit(g_loop);
    return G_SOURCE_CONTINUE;
}

static gboolean on_reload_signal(gpointer ud) {
    (void)ud;
    apply_save_file();
    return G_SOURCE_CONTINUE;
}

/* SIGUSR2 from the CGI: "poll now" (Dashboard button, overlay purge). */
static gboolean on_poll_now_signal(gpointer ud) {
    (void)ud;
    jlog(LOG_INFO, "weather_acap: poll requested by UI");
    do_poll(NULL);
    return G_SOURCE_CONTINUE;
}

/* ── Poll callback ───────────────────────────────────────────────────────── */

static gboolean do_poll(gpointer user_data) {
    (void)user_data;

    char *enabled_s = params_get("SystemEnabled");
    int enabled = enabled_s && strcasecmp(enabled_s, "yes") == 0;
    free(enabled_s);

    char *vuser    = params_get("VapixUser");
    char *vpass    = params_get("VapixPass");
    char *alertmap = params_get("AlertMap");
    char *th_map   = params_get("ThresholdMap");

    if (!enabled) {
        jlog(LOG_INFO, "weather_acap: SystemEnabled=no, skipping poll");
        /* Disabled means *off*: drop any active ports and the overlay so
         * the camera does not keep signalling an alert nobody is watching.
         * All three are no-ops when nothing is active. */
        AlertMap map;
        alerts_map_parse(alertmap, &map);
        alerts_clear_all(&map, vuser, vpass);
        ThresholdMap tmap;
        threshold_map_parse(th_map, &tmap);
        threshold_clear_all(&tmap, vuser, vpass);
        if (g_lightning_active) {
            vapix_port_set(params_get_int("LightningPort", 35), 0, vuser, vpass);
            g_lightning_active = 0;
        }
        overlay_delete(vuser, vpass);
        free(vuser); free(vpass); free(alertmap); free(th_map);
        return G_SOURCE_CONTINUE;
    }

    char *zip      = params_get("ZipCode");
    char *lat_ov   = params_get("LatOverride");
    char *lon_ov   = params_get("LonOverride");
    char *provider = params_get("WeatherProvider");
    char *ua       = params_get("NWSUserAgent");
    char *mock     = params_get("MockMode");

    char *ov_enabled = params_get("OverlayEnabled");
    char *ov_pos     = params_get("OverlayPosition");
    char *ov_tmpl    = params_get("OverlayTemplate");
    char *ov_atmpl   = params_get("OverlayAlertTemplate");
    int   ov_max     = params_get_int("OverlayMaxAlerts", 3);

    char *wh_enabled  = params_get("WebhookEnabled");
    char *wh_url      = params_get("WebhookUrl");
    char *wh_alerts   = params_get("WebhookOnAlertsOnly");
    char *wh_template = params_get("WebhookTemplate");   /* Sprint 11 */

    char *sn_enabled  = params_get("SnapshotEnabled");
    char *sn_res      = params_get("SnapshotResolution");
    char *sn_dir      = params_get("SnapshotSaveDir");
    char *sn_activate = params_get("SnapshotOnActivate");
    char *sn_clear    = params_get("SnapshotOnClear");

    char *mq_enabled      = params_get("MqttEnabled");
    char *mq_broker       = params_get("MqttBrokerUrl");
    char *mq_topic        = params_get("MqttTopic");
    char *mq_user         = params_get("MqttUser");
    char *mq_pass         = params_get("MqttPass");
    char *mq_alerts_only  = params_get("MqttOnAlertOnly");
    char *mq_retain       = params_get("MqttRetain");

    char *em_enabled  = params_get("EmailEnabled");
    char *em_smtp     = params_get("EmailSmtpUrl");
    char *em_from     = params_get("EmailFrom");
    char *em_to       = params_get("EmailTo");
    char *em_user     = params_get("EmailUser");
    char *em_pass     = params_get("EmailPass");
    char *em_on_clear = params_get("EmailOnClear");

    /* Sprint 5 — snapshot auto-delete (ThresholdMap is read above) */
    int   sn_max_count = params_get_int("SnapshotMaxCount", 50);

    /* Sprint 7 — notification cool-down */
    int alert_cooldown  = params_get_int("AlertCooldownMin", 10);
    int thresh_cooldown = params_get_int("ThresholdCooldownMin", 10);

    /* Sprint 8 — multi-camera snapshot */
    char *mc_enabled = params_get("MultiCamEnabled");
    char *mc_list    = params_get("MultiCamList");
    char *mc_res     = params_get("MultiCamResolution");

    /* Sprint 9 — native AXIS events */
    char *ax_ev_enabled = params_get("AxisEventsEnabled");
    axisevents_set_enabled(ax_ev_enabled && strcasecmp(ax_ev_enabled, "yes") == 0);

    /* Sprint 12 — lightning alerts */
    char *ln_enabled   = params_get("LightningEnabled");
    int   ln_port      = params_get_int("LightningPort",     35);
    int   ln_min_risk  = params_get_int("LightningMinRisk",   1);
    int   ln_poll_mult = params_get_int("LightningPollMult",  6);

    /* Sprint 14 — hardware alert output */
    char *ao_disp_en    = params_get("DisplayAlertEnabled");
    char *ao_disp_dur   = params_get("DisplayAlertDuration");
    char *ao_disp_tc    = params_get("DisplayAlertTextColor");
    char *ao_disp_bgw   = params_get("DisplayAlertBgWarning");
    char *ao_disp_bgwt  = params_get("DisplayAlertBgWatch");
    char *ao_strobe_en  = params_get("StrobeAlertEnabled");
    char *ao_strobe_dur = params_get("StrobeAlertDuration");
    char *ao_d4_en      = params_get("D4200Enabled");
    char *ao_d4_host    = params_get("D4200Host");
    char *ao_d4_user    = params_get("D4200User");
    char *ao_d4_pass    = params_get("D4200Pass");
    char *ao_d4_wpro    = params_get("D4200WarningProfile");
    char *ao_d4_wtpro   = params_get("D4200WatchProfile");
    char *ao_aud_en     = params_get("AudioAlertEnabled");
    int   ao_aud_clipw  = params_get_int("AudioClipWarning", -1);
    int   ao_aud_clipwt = params_get_int("AudioClipWatch",   -1);

    /* Sprint 13 — JSON structured logging (re-apply every tick so
     * toggling the setting in the UI takes effect without a restart). */
    char *json_log = params_get("JsonLogging");
    jsonlog_init(json_log && strcasecmp(json_log, "yes") == 0, "weather_acap");

    int is_mock = mock && strcasecmp(mock, "yes") == 0;

    WeatherSnapshot snap;
    memset(&snap, 0, sizeof(snap));
    const char *last_error = "";
    int ok;

    if (is_mock) {
        jlog(LOG_INFO, "weather_acap: [MOCK] poll tick");
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
        snap.alerts.count    = 1;
        snap.alerts.fetch_ok = 1;
        ok = 1;
    } else {
        jlog(LOG_INFO, "weather_acap: poll tick (provider=%s)", provider ? provider : "auto");
        ok = weather_api_fetch(provider ? provider : "auto",
                               zip, lat_ov, lon_ov,
                               (ua && *ua) ? ua : "WeatherACAP/" WEATHER_ACAP_VERSION,
                               &snap);
        if (!ok) last_error = "weather fetch failed";
        else if (!snap.alerts.fetch_ok) last_error = "alerts fetch failed (port state held)";
        else if (!snap.conditions.valid) last_error = "conditions unavailable";
    }

    /* Parse current AlertMap */
    AlertMap map;
    alerts_map_parse(alertmap, &map);

    char overlay_text[400] = "";
    int  video_present = -1;   /* -1 unknown, 0 no, 1 yes */

    if (ok) {
        TickCtx ctx = {
            .snap                    = &snap,
            .webhook_enabled         = wh_enabled && strcasecmp(wh_enabled, "yes") == 0,
            .webhook_url             = wh_url,
            .webhook_template        = wh_template,   /* Sprint 11 */
            .webhook_on_alerts_only  = !wh_alerts || strcasecmp(wh_alerts, "yes") == 0,
            .vapix_user              = vuser,
            .vapix_pass              = vpass,
            .snap_cfg = {
                .enabled     = sn_enabled  && strcasecmp(sn_enabled,  "yes") == 0,
                .resolution  = sn_res,
                .save_dir    = sn_dir,
                .on_activate = !sn_activate || strcasecmp(sn_activate, "yes") == 0,
                .on_clear    = sn_clear    && strcasecmp(sn_clear,    "yes") == 0,
                .max_count   = sn_max_count,
            },
            .mqtt_cfg = {
                .enabled      = mq_enabled && strcasecmp(mq_enabled, "yes") == 0,
                .broker_url   = mq_broker,
                .topic        = mq_topic,
                .username     = mq_user,
                .password     = mq_pass,
                .on_alert_only= !mq_alerts_only || strcasecmp(mq_alerts_only, "yes") == 0,
                .retain       = mq_retain && strcasecmp(mq_retain, "yes") == 0,
            },
            .email_cfg = {
                .enabled   = em_enabled && strcasecmp(em_enabled, "yes") == 0,
                .smtp_url  = em_smtp,
                .from      = em_from,
                .to        = em_to,
                .username  = em_user,
                .password  = em_pass,
                .on_clear  = em_on_clear && strcasecmp(em_on_clear, "yes") == 0,
            },
            /* Sprint 7 — NWS alert cool-down */
            .cooldown_min = alert_cooldown,
        };

        /* Sprint 8 — parse and attach multi-camera config */
        multicam_parse(mc_list, &ctx.multicam_cfg);
        ctx.multicam_cfg.enabled  = mc_enabled && strcasecmp(mc_enabled, "yes") == 0;
        ctx.multicam_resolution   = (mc_res && *mc_res) ? mc_res : "1280x720";

        /* Sprint 14 — hardware alert output config */
        memset(&ctx.alertout_cfg, 0, sizeof(ctx.alertout_cfg));
        ctx.alertout_cfg.display_enabled   = ao_disp_en   && strcasecmp(ao_disp_en,   "yes") == 0;
        ctx.alertout_cfg.display_duration_s= ao_disp_dur  && *ao_disp_dur ? atoi(ao_disp_dur) : 30;
        snprintf(ctx.alertout_cfg.display_text_color, 16, "%s",
                 (ao_disp_tc  && *ao_disp_tc)  ? ao_disp_tc  : "#FFFFFF");
        snprintf(ctx.alertout_cfg.display_bg_warning,  16, "%s",
                 (ao_disp_bgw && *ao_disp_bgw) ? ao_disp_bgw : "#CC0000");
        snprintf(ctx.alertout_cfg.display_bg_watch,    16, "%s",
                 (ao_disp_bgwt&& *ao_disp_bgwt)? ao_disp_bgwt: "#FF8800");
        ctx.alertout_cfg.strobe_enabled    = ao_strobe_en  && strcasecmp(ao_strobe_en,  "yes") == 0;
        ctx.alertout_cfg.strobe_duration_s = ao_strobe_dur && *ao_strobe_dur ? atoi(ao_strobe_dur) : 30;
        ctx.alertout_cfg.d4200_enabled     = ao_d4_en      && strcasecmp(ao_d4_en,     "yes") == 0;
        snprintf(ctx.alertout_cfg.d4200_host,            128, "%s", ao_d4_host  ? ao_d4_host  : "");
        snprintf(ctx.alertout_cfg.d4200_user,             64, "%s", ao_d4_user  ? ao_d4_user  : "root");
        snprintf(ctx.alertout_cfg.d4200_pass,             64, "%s", ao_d4_pass  ? ao_d4_pass  : "");
        snprintf(ctx.alertout_cfg.d4200_warning_profile,  64, "%s", ao_d4_wpro  ? ao_d4_wpro  : "emergency");
        snprintf(ctx.alertout_cfg.d4200_watch_profile,    64, "%s", ao_d4_wtpro ? ao_d4_wtpro : "caution");
        ctx.alertout_cfg.audio_enabled     = ao_aud_en     && strcasecmp(ao_aud_en,    "yes") == 0;
        ctx.alertout_cfg.audio_clip_warning = ao_aud_clipw;
        ctx.alertout_cfg.audio_clip_watch   = ao_aud_clipwt;
        ctx.alertout_cfg.forced_tier        = ALERT_TIER_NONE;   /* classify NWS events */
        ctx.alertout_cfg.vapix_user         = vuser;
        ctx.alertout_cfg.vapix_pass         = vpass;

        /* ── Sprint 12 — SPC lightning / convective risk ─────────────────
         * Runs every ln_poll_mult ticks; the last result is held in
         * g_lightning_* so the overlay {lightning} token and status JSON
         * stay populated between checks.  Must run BEFORE the overlay is
         * rendered.  Transitions go through on_alert_transition like any
         * other alert (history, events, notifications, hardware output at
         * WATCH tier).
         *
         * lightning_check() returns 1 at-risk, 0 clear, -1 fetch/parse
         * error.  On -1 the previous state is kept: a failed SPC fetch is
         * not evidence that the risk has passed. */
        int ln_on = ln_enabled && strcasecmp(ln_enabled, "yes") == 0;
        if (ln_poll_mult < 1) ln_poll_mult = 1;
        TickCtx ln_ctx = ctx;
        ln_ctx.alertout_cfg.forced_tier = ALERT_TIER_WATCH;

        if (ln_on && snap.lat != 0.0) {
            g_lightning_tick++;
            if ((g_lightning_tick - 1) % ln_poll_mult == 0) {
                LightningRisk risk = { "", 0 };
                int res = lightning_check(snap.lat, snap.lon, &risk);
                if (res >= 0) {
                    int at_risk = (res == 1 && risk.risk_level >= ln_min_risk);
                    snprintf(g_lightning_label, sizeof(g_lightning_label), "%s",
                             at_risk ? risk.label : "");
                    g_lightning_level = at_risk ? risk.risk_level : 0;

                    if (at_risk && !g_lightning_active) {
                        jlog(LOG_WARNING,
                             "weather_acap: SPC lightning risk %s (level %d) — activating port %d",
                             risk.label, risk.risk_level, ln_port);
                        vapix_port_set(ln_port, 1, vuser, vpass);
                        g_lightning_active = 1;
                        on_alert_transition("SPC Lightning Risk",
                                            lightning_risk_label(risk.risk_level),
                                            "activated", ln_port, &ln_ctx);
                    } else if (!at_risk && g_lightning_active) {
                        jlog(LOG_INFO,
                             "weather_acap: SPC lightning risk cleared — deactivating port %d",
                             ln_port);
                        vapix_port_set(ln_port, 0, vuser, vpass);
                        g_lightning_active = 0;
                        on_alert_transition("SPC Lightning Risk", "",
                                            "cleared", ln_port, &ln_ctx);
                    }
                }
            }
        } else if (!ln_on && g_lightning_active) {
            /* Lightning disabled while port was active — clear it */
            vapix_port_set(ln_port, 0, vuser, vpass);
            g_lightning_active   = 0;
            g_lightning_label[0] = '\0';
            g_lightning_level    = 0;
            on_alert_transition("SPC Lightning Risk", "", "cleared", ln_port, &ln_ctx);
        }
        snprintf(snap.lightning_risk, sizeof(snap.lightning_risk), "%s", g_lightning_label);
        snap.lightning_risk_level = g_lightning_level;

        /* ── NWS alert-type rules — only when the alert list is trustworthy.
         * An empty list from a failed fetch is "unknown", not "no alerts":
         * acting on it used to clear every port, fire "cleared" on every
         * channel, then re-fire everything on the next good poll. */
        if (snap.alerts.fetch_ok)
            alerts_process(&snap, &map, vuser, vpass, on_alert_transition, &ctx);

        /* ── Sprint 5 — numeric threshold rules.
         * Sprint 7 — separate cool-down.  Hardware output at WATCH tier:
         * a humidity crossing must not fire the red strobe / emergency
         * D4200 profile.  threshold_process holds state when conditions
         * are invalid this poll. */
        TickCtx thresh_ctx = ctx;
        thresh_ctx.cooldown_min = thresh_cooldown;
        thresh_ctx.alertout_cfg.forced_tier = ALERT_TIER_WATCH;
        ThresholdMap tmap;
        threshold_map_parse(th_map, &tmap);
        threshold_process(&snap, &tmap, vuser, vpass, on_alert_transition, &thresh_ctx);

        /* ── Overlay */
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
        else
            overlay_delete(vuser, vpass);   /* toggled off → take it down */

        video_present = overlay_video_present();

        if (snap.conditions.valid)
            jlog(LOG_INFO,
                 "weather_acap: %.0fF %s | wind %.0fmph | alerts:%d",
                 snap.conditions.temp_f,
                 snap.conditions.description,
                 snap.conditions.wind_speed_mph,
                 snap.alerts.count);
        else
            jlog(LOG_INFO, "weather_acap: conditions n/a | alerts:%d", snap.alerts.count);

        /* Per-poll publishes (transition-triggered ones happen in
         * on_alert_transition above). */
        if (ctx.mqtt_cfg.enabled && !ctx.mqtt_cfg.on_alert_only)
            mqtt_publish(&ctx.mqtt_cfg, &snap, "poll", "");
        /* WebhookOnAlertsOnly=no was read into the ctx and never honoured. */
        if (ctx.webhook_enabled && ctx.webhook_url && *ctx.webhook_url &&
            !ctx.webhook_on_alerts_only)
            webhook_post(ctx.webhook_url, &snap, "poll", "", "", ctx.webhook_template);
    }

    /* Status file */
    write_status(&snap, overlay_text, video_present, last_error);

    /* Sprint 6 — conditions history (self-gates on conditions.valid) */
    if (ok) condhistory_append(&snap);

    /* Sprint 9 — publish current conditions as a native AXIS event */
    if (ok) axisevents_publish_conditions(&snap);

    /* Heartbeat */
    FILE *hb = fopen(HEARTBEAT_FILE, "w");
    if (hb) { fprintf(hb, "%ld\n", (long)time(NULL)); fclose(hb); }

    free(zip); free(lat_ov); free(lon_ov); free(provider);
    free(ua); free(alertmap); free(vuser); free(vpass); free(mock);
    free(ov_enabled); free(ov_pos); free(ov_tmpl); free(ov_atmpl);
    free(wh_enabled); free(wh_url); free(wh_alerts); free(wh_template);
    free(sn_enabled); free(sn_res); free(sn_dir);
    free(sn_activate); free(sn_clear);
    free(mq_enabled); free(mq_broker); free(mq_topic);
    free(mq_user); free(mq_pass); free(mq_alerts_only); free(mq_retain);
    free(em_enabled); free(em_smtp); free(em_from);
    free(em_to); free(em_user); free(em_pass); free(em_on_clear);
    free(th_map);
    free(mc_enabled); free(mc_list); free(mc_res);
    free(ax_ev_enabled);
    free(ln_enabled);
    free(json_log);
    free(ao_disp_en); free(ao_disp_dur); free(ao_disp_tc);
    free(ao_disp_bgw); free(ao_disp_bgwt);
    free(ao_strobe_en); free(ao_strobe_dur);
    free(ao_d4_en); free(ao_d4_host); free(ao_d4_user); free(ao_d4_pass);
    free(ao_d4_wpro); free(ao_d4_wtpro);
    free(ao_aud_en);

    return G_SOURCE_CONTINUE;
}

/* ── FastCGI child process ──────────────────────────────────────────────────
 * The ACAP runtime starts the appName binary (this daemon) and, for ACAPs
 * with a fastCgi httpConfig entry, sets FCGI_SOCKET_NAME in its env pointing
 * to the Unix socket Apache forwards requests to.  We fork+exec the CGI
 * binary so it inherits that env var and opens the socket.  Apache's
 * 503 Service Unavailable on /local/weather_acap/weather_acap.cgi comes
 * from no process listening on the expected socket.
 */

static gboolean cgi_respawn_cb(gpointer ud) {
    (void)ud;
    g_cgi_respawn_src = 0;
    spawn_fastcgi_child();
    return G_SOURCE_REMOVE;
}

/* GLib child watch: runs in the main loop when the CGI exits.  Respawns
 * with a floor between attempts and a hard cap so a crash-looping CGI
 * cannot peg the CPU or flood the log. */
static void on_cgi_exit(GPid pid, gint status, gpointer ud) {
    (void)ud;
    g_spawn_close_pid(pid);
    jlog(LOG_WARNING, "weather_acap: FastCGI child pid=%d exited status=%d",
         (int)pid, status);
    if (pid == g_cgi_pid) g_cgi_pid = 0;

    if (g_cgi_respawns >= CGI_RESPAWN_MAX) {
        jlog(LOG_ERR,
             "weather_acap: FastCGI backend died %d times — giving up; "
             "web UI will return 503 until the app is restarted",
             CGI_RESPAWN_MAX);
        return;
    }
    g_cgi_respawns++;

    long wait = CGI_RESPAWN_MIN_SEC - (long)(time(NULL) - g_cgi_last_spawn);
    if (wait < 1) wait = 1;
    jlog(LOG_WARNING, "weather_acap: respawning FastCGI backend in %lds (%d/%d)",
         wait, g_cgi_respawns, CGI_RESPAWN_MAX);
    if (!g_cgi_respawn_src)
        g_cgi_respawn_src = g_timeout_add_seconds((guint)wait, cgi_respawn_cb, NULL);
}

static void spawn_fastcgi_child(void) {
    const char *sock = getenv("FCGI_SOCKET_NAME");
    jlog(LOG_INFO, "weather_acap: FCGI_SOCKET_NAME=%s",
           sock ? sock : "(unset)");
    if (!sock || !*sock) {
        jlog(LOG_WARNING,
               "weather_acap: no FCGI_SOCKET_NAME in env; web UI CGI will "
               "not be spawned — expect HTTP 503 on /local/weather_acap/");
        return;
    }

    pid_t pid = fork();
    if (pid < 0) {
        jlog(LOG_ERR, "weather_acap: fork for CGI failed: %m");
        return;
    }
    if (pid == 0) {
        /* Child: exec the CGI binary from the installed package path.
         * The env (including FCGI_SOCKET_NAME) is inherited. */
        const char *cgi_path = "/usr/local/packages/weather_acap/weather_acap.cgi";
        execl(cgi_path, "weather_acap.cgi", (char *)NULL);
        /* Only reached on failure */
        jlog(LOG_ERR, "weather_acap: exec %s failed: %m", cgi_path);
        _exit(1);
    }
    g_cgi_pid        = pid;
    g_cgi_last_spawn = time(NULL);
    g_child_watch_add(pid, on_cgi_exit, NULL);
    jlog(LOG_INFO, "weather_acap: spawned FastCGI child pid=%d on socket=%s",
           (int)pid, sock);
}

/* ── Entry point ─────────────────────────────────────────────────────────── */

int main(void) {
    openlog("weather_acap", LOG_PID | LOG_CONS, LOG_USER);
    /* Sprint 13 — init JSON logging before the first jlog() call.
     * We can't call params_init() yet, so we default to disabled here;
     * do_poll() re-reads JsonLogging every tick and calls jsonlog_init()
     * so the setting takes effect from the first poll onward.           */
    jsonlog_init(0, "weather_acap");
    jlog(LOG_INFO, "weather_acap: starting up (native ACAP v4, version %s)",
         WEATHER_ACAP_VERSION);

    /* A peer half-closing a socket mid-write must not kill the daemon. */
    signal(SIGPIPE, SIG_IGN);

    /* Signal sources are dispatched from the GLib loop, so the callbacks
     * may use syslog/malloc freely.  Installed before the first poll so a
     * SIGTERM during it is deferred, not fatal. */
    g_unix_signal_add(SIGTERM, on_quit_signal,   GINT_TO_POINTER(SIGTERM));
    g_unix_signal_add(SIGINT,  on_quit_signal,   GINT_TO_POINTER(SIGINT));
    g_unix_signal_add(SIGUSR1, on_reload_signal,   NULL);
    g_unix_signal_add(SIGUSR2, on_poll_now_signal, NULL);

    GError *err = NULL;
    if (!params_init(&err)) {
        jlog(LOG_ERR, "weather_acap: params init failed: %s",
               err ? err->message : "unknown");
        if (err) g_error_free(err);
        return 1;
    }

    curl_global_init(CURL_GLOBAL_DEFAULT);

    /* Sprint 9 — initialise native AXIS event handler (before first poll
     * so the events are declared before we try to send them). */
    axisevents_init();

    /* Write PID and config file so the CGI can read them */
    write_pid_file();
    write_config_file();

    /* Spawn the FastCGI child (web UI backend).  Must happen after
     * write_pid_file / write_config_file so the CGI finds valid state
     * on its first request. */
    spawn_fastcgi_child();

    /* Startup port reset: a crash / SIGKILL / power loss skips the clean
     * shutdown below, and this process has no memory of which ports the
     * previous one left ON.  The first poll re-fires anything genuinely
     * active. */
    {
        char *vuser    = params_get("VapixUser");
        char *vpass    = params_get("VapixPass");
        char *alertmap = params_get("AlertMap");
        char *th_map   = params_get("ThresholdMap");
        AlertMap map;
        alerts_map_parse(alertmap, &map);
        alerts_reset_ports(&map, vuser, vpass);
        ThresholdMap tmap;
        threshold_map_parse(th_map, &tmap);
        threshold_reset_ports(&tmap, vuser, vpass);
        vapix_port_set(params_get_int("LightningPort", 35), 0, vuser, vpass);
        free(vuser); free(vpass); free(alertmap); free(th_map);
    }

    do_poll(NULL);   /* immediate first poll */

    g_loop = g_main_loop_new(NULL, FALSE);
    arm_poll_timer(params_get_int("PollInterval", 300));
    jlog(LOG_INFO, "weather_acap: poll interval %d seconds", g_poll_interval);

    g_main_loop_run(g_loop);

    jlog(LOG_INFO, "weather_acap: shutting down");

    if (g_timer_id) g_source_remove(g_timer_id);
    if (g_cgi_respawn_src) g_source_remove(g_cgi_respawn_src);

    /* Clear any active ports */
    char *vuser    = params_get("VapixUser");
    char *vpass    = params_get("VapixPass");
    char *alertmap = params_get("AlertMap");
    char *th_map   = params_get("ThresholdMap");
    AlertMap map;
    alerts_map_parse(alertmap, &map);
    alerts_clear_all(&map, vuser, vpass);
    ThresholdMap tmap;
    threshold_map_parse(th_map, &tmap);
    threshold_clear_all(&tmap, vuser, vpass);   /* was never called on shutdown */
    if (g_lightning_active)
        vapix_port_set(params_get_int("LightningPort", 35), 0, vuser, vpass);
    overlay_delete(vuser, vpass);
    free(vuser); free(vpass); free(alertmap); free(th_map);

    /* Sprint 9 — undeclare AXIS events and free handler */
    axisevents_cleanup();

    /* Stop the web UI backend with us. */
    if (g_cgi_pid > 0) kill(g_cgi_pid, SIGTERM);

    params_cleanup();
    curl_global_cleanup();
    g_main_loop_unref(g_loop);
    closelog();
    return 0;
}
