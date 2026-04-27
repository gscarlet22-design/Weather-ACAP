/*
 * mqtt.c — Publish weather/alert data to an MQTT broker via libcurl.
 *
 * libcurl's MQTT support is experimental (since 7.71.0, enabled at
 * compile time with --with-mqtt / CURL_ENABLE_MQTT).  On AXIS OS 12+
 * the bundled libcurl should be recent enough.  If the protocol is
 * disabled in the device's libcurl, curl_easy_perform() returns
 * CURLE_UNSUPPORTED_PROTOCOL and we log a clear warning.
 *
 * Publish URL: <broker_url>/<topic>
 * e.g. mqtt://192.168.1.100:1883/weather/camera/alerts
 *
 * Payload is the same JSON shape as the webhook payload for consistency.
 *
 * CURLOPT_MQTT_RETAIN was added in curl 7.82.0; we guard it with a
 * compile-time version check so the code still compiles against older headers.
 */
#include "mqtt.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>
#include <time.h>

#ifndef CGI_NO_CURL
#include <curl/curl.h>

/* Discard the tiny MQTT response/ack body. */
static size_t discard_cb(void *p, size_t sz, size_t n, void *ud)
{
    (void)p; (void)ud;
    return sz * n;
}

/* Minimal JSON escaper — scope-local so there is no link conflict with
 * the identically-named helper in webhook.c. */
static void mqtt_esc(const char *in, char *out, size_t outlen)
{
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

#endif /* CGI_NO_CURL */

int mqtt_publish(const MqttConfig *cfg,
                 const WeatherSnapshot *snap,
                 const char *event_type,
                 const char *alert_event)
{
    if (!cfg || !cfg->enabled) return 0;
    if (!cfg->broker_url || !*cfg->broker_url) return 0;
    if (!cfg->topic      || !*cfg->topic)      return 0;

#ifdef CGI_NO_CURL
    (void)snap; (void)event_type; (void)alert_event;
    return 0;
#else
    /* Build the full MQTT URL: strip any trailing slash from broker_url,
     * then append the topic as the URL path component. */
    char url[512];
    size_t blen = strlen(cfg->broker_url);
    const char *base = cfg->broker_url;
    if (blen > 0 && base[blen - 1] == '/')
        snprintf(url, sizeof(url), "%.*s/%s", (int)(blen - 1), base, cfg->topic);
    else
        snprintf(url, sizeof(url), "%s/%s", base, cfg->topic);

    /* Build JSON payload */
    char body[2048];
    char e_type[64], e_evt[256], e_desc[192], e_prov[32];
    mqtt_esc(event_type,  e_type, sizeof(e_type));
    mqtt_esc(alert_event, e_evt,  sizeof(e_evt));
    mqtt_esc(snap ? snap->conditions.description : "", e_desc, sizeof(e_desc));
    mqtt_esc(snap ? snap->conditions.provider    : "", e_prov, sizeof(e_prov));

    time_t now = time(NULL);
    char   ts[32];
    strftime(ts, sizeof(ts), "%Y-%m-%dT%H:%M:%SZ", gmtime(&now));

    snprintf(body, sizeof(body),
        "{"
        "\"timestamp\":\"%s\","
        "\"event_type\":\"%s\","
        "\"alert\":\"%s\","
        "\"conditions\":{"
            "\"temp_f\":%.1f,"
            "\"description\":\"%s\","
            "\"wind_mph\":%.1f,"
            "\"humidity_pct\":%d,"
            "\"provider\":\"%s\""
        "},"
        "\"active_alert_count\":%d"
        "}",
        ts, e_type, e_evt,
        snap ? snap->conditions.temp_f         : 0.0,
        e_desc,
        snap ? snap->conditions.wind_speed_mph : 0.0,
        snap ? snap->conditions.humidity_pct   : 0,
        e_prov,
        snap ? snap->alerts.count              : 0);

    CURL *curl = curl_easy_init();
    if (!curl) return 0;

    /* Optional broker authentication */
    char userpwd[256] = "";
    if (cfg->username && *cfg->username) {
        snprintf(userpwd, sizeof(userpwd), "%s:%s",
                 cfg->username,
                 cfg->password ? cfg->password : "");
        curl_easy_setopt(curl, CURLOPT_USERPWD, userpwd);
    }

    curl_easy_setopt(curl, CURLOPT_URL,          url);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS,    body);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long)strlen(body));
    curl_easy_setopt(curl, CURLOPT_TIMEOUT,       8L);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, discard_cb);

    /* CURLOPT_MQTT_RETAIN was added in curl 7.82.0, but some SDK sysroots
     * define a version number >= 7.82.0 while still not exposing the constant
     * (MQTT compiled out of the sysroot headers).  Use #ifdef rather than a
     * version-number guard so we compile cleanly against any SDK curl.h. */
#ifdef CURLOPT_MQTT_RETAIN
    if (cfg->retain)
        curl_easy_setopt(curl, CURLOPT_MQTT_RETAIN, 1L);
#endif

    CURLcode rc = curl_easy_perform(curl);
    curl_easy_cleanup(curl);

    if (rc != CURLE_OK) {
        syslog(LOG_WARNING, "mqtt: publish to %s failed: %s",
               url, curl_easy_strerror(rc));
        if (rc == CURLE_UNSUPPORTED_PROTOCOL)
            syslog(LOG_WARNING,
                   "mqtt: libcurl on this device was compiled without MQTT "
                   "support — use the webhook integration instead");
        return 0;
    }

    syslog(LOG_INFO, "mqtt: published to %s (event=%s alert=%s)",
           url,
           event_type  ? event_type  : "?",
           alert_event ? alert_event : "");
    return 1;
#endif /* CGI_NO_CURL */
}
