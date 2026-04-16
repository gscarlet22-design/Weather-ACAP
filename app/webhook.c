#include "webhook.h"
#include "weather_api.h"

#include <curl/curl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>
#include <time.h>

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

long webhook_post(const char *url,
                  const WeatherSnapshot *snap,
                  const char *event_type,
                  const char *alert_event) {
    if (!url || !*url) return 0;

    char body[2048];
    char e_type[64], e_evt[256], e_desc[192], e_prov[32];
    escape_json(event_type,  e_type, sizeof(e_type));
    escape_json(alert_event, e_evt,  sizeof(e_evt));
    escape_json(snap ? snap->conditions.description : "", e_desc, sizeof(e_desc));
    escape_json(snap ? snap->conditions.provider    : "", e_prov, sizeof(e_prov));

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
            "\"wind_dir_deg\":%d,"
            "\"humidity_pct\":%d,"
            "\"provider\":\"%s\""
        "},"
        "\"location\":{\"lat\":%.6f,\"lon\":%.6f},"
        "\"active_alert_count\":%d"
        "}",
        ts, e_type, e_evt,
        snap ? snap->conditions.temp_f : 0.0,
        e_desc,
        snap ? snap->conditions.wind_speed_mph : 0.0,
        snap ? snap->conditions.wind_dir_deg : -1,
        snap ? snap->conditions.humidity_pct : 0,
        e_prov,
        snap ? snap->lat : 0.0,
        snap ? snap->lon : 0.0,
        snap ? snap->alerts.count : 0);

    CURL *curl = curl_easy_init();
    if (!curl) return 0;

    struct curl_slist *hdrs = curl_slist_append(NULL, "Content-Type: application/json");
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
}
