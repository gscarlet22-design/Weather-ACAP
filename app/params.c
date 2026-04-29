/*
 * params.c — file-backed parameter storage
 *
 * Originally backed by libaxparameter, but on AXIS OS 12 the parameter
 * daemon rejects ax_parameter_set() from sandboxed ACAPs with an opaque
 * "Failed to set parameter X" error (see commit 593bcc2 syslog evidence
 * — every set in a fresh install returned the same message).
 *
 * Replaced with a plain JSON file in the package's localdata directory,
 * which the per-app sandbox user owns and can read/write.  The bundled
 * cJSON is parse-only (no Create/Add/Print), so we keep an in-memory
 * key→value array for the store and serialise it manually.
 *
 * Concurrency: only the daemon process calls these functions.  The CGI
 * uses its own file-based config channel (see config_cgi.c).  No locking
 * needed — daemon main loop is single-threaded.
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

    /* Snapshot on alert */
    { "SnapshotEnabled",    "no"       },
    { "SnapshotResolution", "1280x720" },
    { "SnapshotSaveDir",    ""         },   /* "" = auto-detect SD card */
    { "SnapshotOnActivate", "yes"      },
    { "SnapshotOnClear",    "no"       },

    /* Sprint 3 — MQTT publishing */
    { "MqttEnabled",       "no"  },
    { "MqttBrokerUrl",     ""    },   /* mqtt://host:1883 */
    { "MqttTopic",         "weather/camera/alerts" },
    { "MqttUser",          ""    },
    { "MqttPass",          ""    },
    { "MqttOnAlertOnly",   "yes" },
    { "MqttRetain",        "no"  },

    /* Sprint 3 — Email notifications */
    { "EmailEnabled",  "no"  },
    { "EmailSmtpUrl",  ""    },   /* smtp://host:587 or smtps://host:465 */
    { "EmailFrom",     ""    },
    { "EmailTo",       ""    },
    { "EmailUser",     ""    },
    { "EmailPass",     ""    },
    { "EmailOnClear",  "no"  },

    /* Sprint 5 — Threshold condition alerts */
    { "ThresholdMap",     ""   },   /* "Condition:Op:Value:Port:Enabled|..." */

    /* Sprint 5 — Snapshot auto-delete */
    { "SnapshotMaxCount", "50" },   /* max .jpg files to keep; 0 = unlimited */

    /* Sprint 7 — Notification cool-down */
    { "AlertCooldownMin",     "10" },   /* minutes between repeat notifications; 0 = disabled */
    { "ThresholdCooldownMin", "10" },   /* same for threshold-rule notifications */

    /* Sprint 8 — Multi-camera snapshot */
    { "MultiCamEnabled",    "no" },   /* capture from additional cameras on alert */
    { "MultiCamList",       ""   },   /* "host:user:pass:label|..." */
    { "MultiCamResolution", "1280x720" },

    { NULL, NULL }
};

/* In-memory store — flat array sized to the number of defaults.  Every
 * known key has a slot from init time onwards; values are heap-owned. */
#define STORE_MAX 64
typedef struct { char *name; char *value; } Slot;
static Slot g_store[STORE_MAX];
static int  g_store_n = 0;
static int  g_inited  = 0;

static const char *compiled_default(const char *name) {
    for (int i = 0; DEFAULTS[i].name; i++)
        if (strcmp(DEFAULTS[i].name, name) == 0)
            return DEFAULTS[i].value;
    return "";
}

static int find_slot(const char *name) {
    for (int i = 0; i < g_store_n; i++)
        if (strcmp(g_store[i].name, name) == 0) return i;
    return -1;
}

static void set_slot(const char *name, const char *value) {
    int idx = find_slot(name);
    if (idx >= 0) {
        free(g_store[idx].value);
        g_store[idx].value = strdup(value ? value : "");
        return;
    }
    if (g_store_n >= STORE_MAX) return;
    g_store[g_store_n].name  = strdup(name);
    g_store[g_store_n].value = strdup(value ? value : "");
    g_store_n++;
}

/* JSON-escape a string into f. */
static void json_esc_to(FILE *f, const char *s) {
    if (!s) return;
    for (const char *p = s; *p; p++) {
        unsigned char c = (unsigned char)*p;
        switch (c) {
            case '"':  fputs("\\\"", f); break;
            case '\\': fputs("\\\\", f); break;
            case '\n': fputs("\\n",  f); break;
            case '\r': fputs("\\r",  f); break;
            case '\t': fputs("\\t",  f); break;
            default:
                if (c < 0x20) fprintf(f, "\\u%04x", c);
                else          fputc(c, f);
        }
    }
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
    /* Ensure dir exists. */
    struct stat st;
    if (stat(PARAMS_DIR, &st) != 0) {
        if (mkdir(PARAMS_DIR, 0755) != 0 && errno != EEXIST) {
            syslog(LOG_WARNING, "params: mkdir(%s) failed: %s",
                   PARAMS_DIR, strerror(errno));
            return -1;
        }
    }

    FILE *f = fopen(PARAMS_TMP, "wb");
    if (!f) {
        syslog(LOG_WARNING, "params: open(%s) for write failed: %s",
               PARAMS_TMP, strerror(errno));
        return -1;
    }
    fputs("{\n", f);
    for (int i = 0; i < g_store_n; i++) {
        fputs("  \"", f);
        json_esc_to(f, g_store[i].name);
        fputs("\": \"", f);
        json_esc_to(f, g_store[i].value);
        fputs("\"", f);
        fputs(i + 1 < g_store_n ? ",\n" : "\n", f);
    }
    fputs("}\n", f);
    fflush(f);
    fsync(fileno(f));
    fclose(f);

    if (rename(PARAMS_TMP, PARAMS_FILE) != 0) {
        syslog(LOG_WARNING, "params: rename(%s → %s) failed: %s",
               PARAMS_TMP, PARAMS_FILE, strerror(errno));
        unlink(PARAMS_TMP);
        return -1;
    }
    return 0;
}

/* Load PARAMS_FILE into g_store.  Missing/corrupt → empty store.       */
static void store_load(void) {
    /* Reset slots */
    for (int i = 0; i < g_store_n; i++) {
        free(g_store[i].name);
        free(g_store[i].value);
    }
    g_store_n = 0;

    char *raw = slurp(PARAMS_FILE);
    if (!raw) return;
    cJSON *root = cJSON_Parse(raw);
    free(raw);
    if (!root) {
        syslog(LOG_WARNING, "params: %s exists but is unparseable; ignoring",
               PARAMS_FILE);
        return;
    }
    /* Walk children: each is a key→string member.  cJSON exposes this
     * via item->child / ->next chain with ->string set to the key.    */
    for (cJSON *it = root->child; it; it = it->next) {
        if (cJSON_IsString(it) && it->string) {
            set_slot(it->string, it->valuestring ? it->valuestring : "");
        }
    }
    cJSON_Delete(root);
}

gboolean params_init(GError **error) {
    (void)error;
    store_load();

    /* Seed any missing defaults so subsequent gets always have a value
     * in the file (not just falling back to compiled defaults). */
    int seeded = 0;
    for (int i = 0; DEFAULTS[i].name; i++) {
        if (find_slot(DEFAULTS[i].name) < 0) {
            set_slot(DEFAULTS[i].name, DEFAULTS[i].value);
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
        syslog(LOG_INFO, "params: loaded %d keys from %s",
               g_store_n, PARAMS_FILE);
    }
    g_inited = 1;
    return TRUE;
}

gboolean params_init_readonly(void) {
    /* CGI doesn't actually call this — it uses cfg_get on the /tmp
     * config file written by the daemon.  Kept for ABI compat. */
    store_load();
    g_inited = 1;
    return TRUE;
}

void params_cleanup(void) {
    for (int i = 0; i < g_store_n; i++) {
        free(g_store[i].name);
        free(g_store[i].value);
    }
    g_store_n = 0;
    g_inited  = 0;
}

char *params_get(const char *name) {
    int idx = find_slot(name);
    if (idx >= 0) return strdup(g_store[idx].value ? g_store[idx].value : "");
    return strdup(compiled_default(name));
}

gboolean params_set(const char *name, const char *value, GError **error) {
    if (!g_inited) {
        syslog(LOG_WARNING, "params_set(%s): store not initialized", name);
        if (error)
            *error = g_error_new(G_FILE_ERROR, G_FILE_ERROR_FAILED,
                                 "params store not initialized");
        return FALSE;
    }
    set_slot(name, value);
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
