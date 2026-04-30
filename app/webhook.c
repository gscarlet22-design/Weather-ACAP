#include "webhook.h"
#include "weather_api.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>
#include <time.h>

#ifndef CGI_NO_CURL
#include <curl/curl.h>

static size_t discard_cb(void *p, size_t sz, size_t n, void *ud) {
    (void)p; (void)ud;
    return sz * n;
}

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

#endif /* CGI_NO_CURL */

/* ── Template renderer ──────────────────────────────────────────────────── */
/*
 * Replace {token} placeholders in tmpl with real values.
 * Supported tokens:
 *   {timestamp}   ISO-8601 UTC
 *   {event_type}  e.g. "alert_activated"
 *   {alert_type}  alert/threshold name
 *   {headline}    NWS headline text (may be "")
 *   {temp_f}      temperature (no quotes — numeric)
 *   {wind_mph}    wind speed  (no quotes — numeric)
 *   {humidity_pct} humidity % (no quotes — numeric)
 *   {description} conditions description
 *   {active_count} count of active alerts (no quotes — numeric)
 */
size_t webhook_render_template(const char *tmpl,
                               const WeatherSnapshot *snap,
                               const char *event_type,
                               const char *alert_event,
                               const char *headline,
                               char *buf, size_t outlen) {
    if (!tmpl || !buf || outlen == 0) return 0;

    char ts[32] = "";
    time_t now = time(NULL);
    strftime(ts, sizeof(ts), "%Y-%m-%dT%H:%M:%SZ", gmtime(&now));

    /* Build numeric strings for tokens that appear unquoted */
    char s_temp[32], s_wind[32], s_hum[16], s_cnt[16];
    snprintf(s_temp, sizeof(s_temp), "%.1f",  snap ? snap->conditions.temp_f        : 0.0);
    snprintf(s_wind, sizeof(s_wind), "%.1f",  snap ? snap->conditions.wind_speed_mph: 0.0);
    snprintf(s_hum,  sizeof(s_hum),  "%d",    snap ? snap->conditions.humidity_pct  : 0);
    snprintf(s_cnt,  sizeof(s_cnt),  "%d",    snap ? snap->alerts.count             : 0);

    const char *desc = (snap && snap->conditions.description[0])
                       ? snap->conditions.description : "";

    /* Token table — order matters (longer names first to avoid prefix clashes) */
    static const char *keys[] = {
        "{timestamp}", "{event_type}", "{alert_type}", "{headline}",
        "{temp_f}", "{wind_mph}", "{humidity_pct}", "{description}", "{active_count}",
        NULL
    };
    const char *vals[9];
    vals[0] = ts;
    vals[1] = event_type  ? event_type  : "";
    vals[2] = alert_event ? alert_event : "";
    vals[3] = headline    ? headline    : "";
    vals[4] = s_temp;
    vals[5] = s_wind;
    vals[6] = s_hum;
    vals[7] = desc;
    vals[8] = s_cnt;

    size_t written = 0;
    const char *p = tmpl;
    while (*p && written + 1 < outlen) {
        if (*p == '{') {
            int matched = 0;
            for (int i = 0; keys[i]; i++) {
                size_t klen = strlen(keys[i]);
                if (strncmp(p, keys[i], klen) == 0) {
                    const char *v = vals[i];
                    size_t vlen = strlen(v);
                    if (written + vlen >= outlen) vlen = outlen - written - 1;
                    memcpy(buf + written, v, vlen);
                    written += vlen;
                    p += klen;
                    matched = 1;
                    break;
                }
            }
            if (!matched) buf[written++] = *p++;
        } else {
            buf[written++] = *p++;
        }
    }
    buf[written] = '\0';
    return written;
}

/* ── webhook_post ───────────────────────────────────────────────────────── */
long webhook_post(const char *url,
                  const WeatherSnapshot *snap,
                  const char *event_type,
                  const char *alert_event,
                  const char *headline,
                  const char *tmpl) {
    if (!url || !*url) return 0;
#ifdef CGI_NO_CURL
    (void)snap; (void)event_type; (void)alert_event;
    (void)headline; (void)tmpl;
    return 0;
#else
    char body[4096];
    const char *content_type = "application/json";

    if (tmpl && *tmpl) {
        /* Render user-supplied template */
        webhook_render_template(tmpl, snap, event_type, alert_event, headline,
                                body, sizeof(body));
        /* Detect content type: if rendered output starts with { it's JSON */
        content_type = (body[0] == '{' || body[0] == '[')
                       ? "application/json" : "text/plain";
    } else {
        /* Built-in JSON payload */
        char e_type[64], e_evt[256], e_desc[192], e_prov[32], e_head[512];
        escape_json(event_type,  e_type, sizeof(e_type));
        escape_json(alert_event, e_evt,  sizeof(e_evt));
        escape_json(snap ? snap->conditions.description : "", e_desc, sizeof(e_desc));
        escape_json(snap ? snap->conditions.provider    : "", e_prov, sizeof(e_prov));
        escape_json(headline ? headline : "",                  e_head, sizeof(e_head));

        time_t now = time(NULL);
        char   ts[32];
        strftime(ts, sizeof(ts), "%Y-%m-%dT%H:%M:%SZ", gmtime(&now));

        snprintf(body, sizeof(body),
            "{"
            "\"timestamp\":\"%s\","
            "\"event_type\":\"%s\","
            "\"alert\":\"%s\","
            "\"headline\":\"%s\","
            "\"conditions\":{"
                "\"temp_f\":%.1f,"
                "\"description\":\"%s\","
                "\"wind_mph\":%.1f,"
                "\"wind_dir_deg\":%d,"
                "\"humidity_pct\":%d,"
                "\"provider\":\"%s\""
            "},"
            "\"location\":{\"lat\":%.6f,\"lon\":%.6f},"
            "\"active_alert_count\":%d"
            "}",
            ts, e_type, e_evt, e_head,
            snap ? snap->conditions.temp_f : 0.0,
            e_desc,
            snap ? snap->conditions.wind_speed_mph : 0.0,
            snap ? snap->conditions.wind_dir_deg : -1,
            snap ? snap->conditions.humidity_pct : 0,
            e_prov,
            snap ? snap->lat : 0.0,
            snap ? snap->lon : 0.0,
            snap ? snap->alerts.count : 0);
    }

    CURL *curl = curl_easy_init();
    if (!curl) return 0;

    char ct_hdr[64];
    snprintf(ct_hdr, sizeof(ct_hdr), "Content-Type: %s", content_type);
    struct curl_slist *hdrs = curl_slist_append(NULL, ct_hdr);
    hdrs = curl_slist_append(hdrs, "User-Agent: WeatherACAP/2.0");

    curl_easy_setopt(curl, CURLOPT_URL,            url);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER,     hdrs);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS,     body);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT,        8L);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION,  discard_cb);

    CURLcode rc = curl_easy_perform(curl);
    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);

    curl_slist_free_all(hdrs);
    curl_easy_cleanup(curl);

    if (rc != CURLE_OK)
        syslog(LOG_WARNING, "weather_acap: webhook POST failed (%s): %s",
               url, curl_easy_strerror(rc));
    else
        syslog(LOG_INFO, "weather_acap: webhook POST → %s : HTTP %ld",
               url, http_code);

    return (rc == CURLE_OK) ? http_code : 0;
#endif /* CGI_NO_CURL */
}
