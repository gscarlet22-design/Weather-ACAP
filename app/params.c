/*
 * params.c — file-backed parameter storage
 *
 * Originally backed by libaxparameter, but on AXIS OS 12 the parameter
 * daemon rejects ax_parameter_set() from sandboxed ACAPs with an opaque
 * "Failed to set parameter X" error (see syslog evidence in commit
 * 593bcc2 — every set in a fresh install returns the same message).
 *
 * Replaced with a plain JSON file in the package's localdata directory,
 * which the per-app sandbox user owns and can read/write.  Public API
 * (params_init, params_get, params_set, params_get_int) unchanged.
 *
 * Concurrency: only the daemon process calls these functions.  The CGI
 * uses its own file-based config channel (see config_cgi.c).  No locking
 * needed within the daemon — its main loop is single-threaded.
 */

#include "params.h"
#include "cJSON.h"

#include <glib.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>
#include <sys/stat.h>
#include <unistd.h>
#include <errno.h>

/* Backing store path.  /usr/local/packages/<app>/localdata/ is the
 * canonical persistent location for ACAP application data.            */
#define PARAMS_DIR  "/usr/local/packages/weather_acap/localdata"
#define PARAMS_FILE PARAMS_DIR "/params.json"
#define PARAMS_TMP  PARAMS_DIR "/params.json.tmp"

/* In-memory cache.  Loaded at init, mutated on every set, persisted to
 * PARAMS_FILE atomically (write tmp → rename). */
static cJSON *g_store = NULL;

/* Compiled-in defaults — used when a key is missing from the file. */
static const struct { const char *name; const char *value; } DEFAULTS[] = {
    { "SystemEnabled",   "yes" },

    /* Location */
    { "ZipCode",         "" },
    { "LatOverride",     "" },
    { "LonOverride",     "" },

    /* Weather provider */
    { "WeatherProvider", "auto" },
    { "NWSUserAgent",    "WeatherACAP/2.0 (admin@example.com)" },
    { "PollInterval",    "300" },

    /* Alert → port map: "Type:Port:Enabled|Type:Port:Enabled|..." */
    { "AlertMap",
      "Tornado Warning:20:1"
      "|Severe Thunderstorm Warning:21:1"
      "|Flash Flood Warning:22:1"
      "|Tornado Watch:23:0"
      "|Severe Thunderstorm Watch:24:0"
      "|Flash Flood Watch:25:0"
      "|Flood Warning:26:0"
      "|Winter Storm Warning:27:0"
      "|Blizzard Warning:28:0"
      "|Ice Storm Warning:29:0"
      "|High Wind Warning:30:0"
      "|Hurricane Warning:31:0"
      "|Tropical Storm Warning:32:0"
      "|Excessive Heat Warning:33:0"
      "|Red Flag Warning:34:0" },

    /* Overlay */
    { "OverlayEnabled",       "yes" },
    { "OverlayPosition",      "topLeft" },
    { "OverlayTemplate",      "Temp: {temp}F | {cond} | Wind: {wind}mph {dir} | Hum: {hum}%" },
    { "OverlayAlertTemplate", "[ALERT: {alert_type}] " },
    { "OverlayMaxAlerts",     "3" },

    /* Webhook */
    { "WebhookEnabled",      "no" },
    { "WebhookUrl",          "" },
    { "WebhookOnAlertsOnly", "yes" },

    /* VAPIX localhost auth */
    { "VapixUser",  "root" },
    { "VapixPass",  "" },

    /* Testing */
    { "MockMode",   "no" },

    { NULL, NULL }
};

static const char *compiled_default(const char *name) {
    for (int i = 0; DEFAULTS[i].name; i++)
        if (strcmp(DEFAULTS[i].name, name) == 0)
            return DEFAULTS[i].value;
    return "";
}

/* Read entire file → heap string.  Returns NULL on missing/empty. */
static char *slurp(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    rewind(f);
    if (sz <= 0) { fclose(f); return NULL; }
    char *buf = (char *)malloc(sz + 1);
    if (!buf) { fclose(f); return NULL; }
    size_t n = fread(buf, 1, sz, f);
    buf[n] = '\0';
    fclose(f);
    return buf;
}

/* Persist g_store to PARAMS_FILE atomically.  Returns 0 on success. */
static int store_persist(void) {
    if (!g_store) return -1;

    /* Ensure dir exists (mkdir -p semantics for one level). */
    struct stat st;
    if (stat(PARAMS_DIR, &st) != 0) {
        if (mkdir(PARAMS_DIR, 0755) != 0 && errno != EEXIST) {
            syslog(LOG_WARNING, "params: mkdir(%s) failed: %s",
                   PARAMS_DIR, strerror(errno));
            return -1;
        }
    }

    char *json = cJSON_PrintUnformatted(g_store);
    if (!json) return -1;

    FILE *f = fopen(PARAMS_TMP, "wb");
    if (!f) {
        syslog(LOG_WARNING, "params: open(%s) for write failed: %s",
               PARAMS_TMP, strerror(errno));
        free(json);
        return -1;
    }
    size_t n = fwrite(json, 1, strlen(json), f);
    int werr = (n != strlen(json));
    fflush(f);
    fsync(fileno(f));
    fclose(f);
    free(json);

    if (werr) {
        unlink(PARAMS_TMP);
        return -1;
    }
    if (rename(PARAMS_TMP, PARAMS_FILE) != 0) {
        syslog(LOG_WARNING, "params: rename(%s → %s) failed: %s",
               PARAMS_TMP, PARAMS_FILE, strerror(errno));
        unlink(PARAMS_TMP);
        return -1;
    }
    return 0;
}

/* Load PARAMS_FILE into g_store.  If missing/corrupt, start empty. */
static void store_load(void) {
    if (g_store) { cJSON_Delete(g_store); g_store = NULL; }
    char *raw = slurp(PARAMS_FILE);
    if (raw) {
        g_store = cJSON_Parse(raw);
        free(raw);
    }
    if (!g_store) g_store = cJSON_CreateObject();
}

gboolean params_init(GError **error) {
    (void)error;
    store_load();

    /* Seed any missing defaults so subsequent gets always have a value
     * in the file (not just falling back to compiled defaults). */
    int seeded = 0;
    for (int i = 0; DEFAULTS[i].name; i++) {
        cJSON *v = cJSON_GetObjectItem(g_store, DEFAULTS[i].name);
        if (!cJSON_IsString(v)) {
            cJSON_DeleteItemFromObject(g_store, DEFAULTS[i].name);
            cJSON_AddStringToObject(g_store, DEFAULTS[i].name, DEFAULTS[i].value);
            seeded++;
        }
    }
    if (seeded > 0) {
        if (store_persist() == 0)
            syslog(LOG_INFO, "params: seeded %d default(s) into %s",
                   seeded, PARAMS_FILE);
        else
            syslog(LOG_WARNING, "params: seeded %d default(s) but persist FAILED",
                   seeded);
    } else {
        syslog(LOG_INFO, "params: loaded %s",
               PARAMS_FILE);
    }
    return TRUE;
}

gboolean params_init_readonly(void) {
    /* CGI doesn't actually call this anymore — it uses cfg_get on the
     * /tmp config file written by the daemon.  Kept for ABI compat with
     * any caller that still references it. */
    store_load();
    return TRUE;
}

void params_cleanup(void) {
    if (g_store) {
        cJSON_Delete(g_store);
        g_store = NULL;
    }
}

char *params_get(const char *name) {
    if (g_store) {
        cJSON *v = cJSON_GetObjectItem(g_store, name);
        if (cJSON_IsString(v) && v->valuestring)
            return strdup(v->valuestring);
    }
    return strdup(compiled_default(name));
}

gboolean params_set(const char *name, const char *value, GError **error) {
    if (!g_store) {
        if (error)
            *error = g_error_new(G_FILE_ERROR, G_FILE_ERROR_FAILED,
                                 "params store not initialized");
        return FALSE;
    }
    /* cJSON's API has no direct "set string" — replace by delete+add. */
    cJSON_DeleteItemFromObject(g_store, name);
    if (!cJSON_AddStringToObject(g_store, name, value ? value : "")) {
        if (error)
            *error = g_error_new(G_FILE_ERROR, G_FILE_ERROR_FAILED,
                                 "cJSON add failed for %s", name);
        return FALSE;
    }
    if (store_persist() != 0) {
        syslog(LOG_WARNING, "params_set(%s): persist failed", name);
        if (error)
            *error = g_error_new(G_FILE_ERROR, G_FILE_ERROR_FAILED,
                                 "could not write %s", PARAMS_FILE);
        return FALSE;
    }
    return TRUE;
}

int params_get_int(const char *name, int default_val) {
    char *s = params_get(name);
    int   v = default_val;
    if (s && *s) v = atoi(s);
    free(s);
    return v;
}
