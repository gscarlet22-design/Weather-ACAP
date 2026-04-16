#include "params.h"

#include <axparameter.h>
#include <glib.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define APP_NAME "weather_acap"

static AXParameter *g_axparam = NULL;

/* Compiled-in defaults used when a parameter has no stored value.
 *
 * AlertMap is a pipe-delimited list of "Type:Port:Enabled" tuples.
 * Enabled is 0 or 1. Ports can be anywhere in the device's virtual port range.
 * The UI edits this string as structured data.
 */
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
    { "OverlayPosition",      "topLeft" },   /* topLeft|topRight|bottomLeft|bottomRight */
    { "OverlayTemplate",      "Temp: {temp}F | {cond} | Wind: {wind}mph {dir} | Hum: {hum}%" },
    { "OverlayAlertTemplate", "[ALERT: {alert_type}] " },
    { "OverlayMaxAlerts",     "3" },

    /* Webhook — POST JSON on each alert transition */
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

gboolean params_init(GError **error) {
    g_axparam = ax_parameter_new(APP_NAME, error);
    if (!g_axparam) return FALSE;

    /* Ensure every parameter exists so ax_parameter_set() works reliably from
     * the CGI binary.  On first install none of these have been created yet. */
    for (int i = 0; DEFAULTS[i].name; i++) {
        gchar  *existing = NULL;
        GError *e        = NULL;
        gboolean ok = ax_parameter_get(g_axparam, DEFAULTS[i].name, &existing, &e);
        if (!ok || !existing) {
            if (e) g_error_free(e);
            GError *se = NULL;
            ax_parameter_set(g_axparam, DEFAULTS[i].name, DEFAULTS[i].value, TRUE, &se);
            if (se) g_error_free(se);
        } else {
            g_free(existing);
        }
    }
    return TRUE;
}

void params_cleanup(void) {
    if (g_axparam) {
        ax_parameter_free(g_axparam);
        g_axparam = NULL;
    }
}

char *params_get(const char *name) {
    if (!g_axparam) return strdup(compiled_default(name));

    GError *err = NULL;
    gchar  *val = NULL;
    if (!ax_parameter_get(g_axparam, name, &val, &err) || !val) {
        if (err) g_error_free(err);
        return strdup(compiled_default(name));
    }
    char *result = strdup(val);
    g_free(val);
    return result;
}

gboolean params_set(const char *name, const char *value, GError **error) {
    if (!g_axparam) {
        if (error)
            *error = g_error_new(G_FILE_ERROR, G_FILE_ERROR_FAILED, "axparameter not initialized");
        return FALSE;
    }
    return ax_parameter_set(g_axparam, name, value, TRUE, error);
}

int params_get_int(const char *name, int default_val) {
    char *s = params_get(name);
    int   v = default_val;
    if (s && *s) v = atoi(s);
    free(s);
    return v;
}
