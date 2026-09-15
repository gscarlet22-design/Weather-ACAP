#include "openmeteo.h"
#include "cJSON.h"
#include "version.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>

#ifndef CGI_NO_CURL
#include <curl/curl.h>

typedef struct { char *data; size_t size; } Buf;

static size_t write_cb(void *ptr, size_t sz, size_t nmemb, void *ud) {
    Buf *b   = (Buf *)ud;
    size_t n = sz * nmemb;
    char  *p = realloc(b->data, b->size + n + 1);
    if (!p) return 0;
    b->data = p;
    memcpy(b->data + b->size, ptr, n);
    b->size += n;
    b->data[b->size] = '\0';
    return n;
}

/* WMO weather interpretation codes → human-readable string */
static const char *wmo_description(int code) {
    switch (code) {
        case 0:  return "Clear Sky";
        case 1:  return "Mainly Clear";
        case 2:  return "Partly Cloudy";
        case 3:  return "Overcast";
        case 45: case 48: return "Foggy";
        case 51: case 53: case 55: return "Drizzle";
        case 61: case 63: case 65: return "Rain";
        case 66: case 67: return "Freezing Rain";
        case 71: case 73: case 75: return "Snow";
        case 77: return "Snow Grains";
        case 80: case 81: case 82: return "Rain Showers";
        case 85: case 86: return "Snow Showers";
        case 95: return "Thunderstorm";
        case 96: case 99: return "Thunderstorm with Hail";
        default: return "Unknown";
    }
}

#endif /* CGI_NO_CURL */

void openmeteo_get_observation(double lat, double lon, OMObservation *result) {
    memset(result, 0, sizeof(*result));
    result->wind_speed_mph = -1;
    result->wind_dir_deg   = -1;
    result->humidity_pct   = -1;
#ifndef CGI_NO_CURL
    char url[640];
    snprintf(url, sizeof(url),
        "https://api.open-meteo.com/v1/forecast"
        "?latitude=%.4f&longitude=%.4f"
        "&current=temperature_2m,relative_humidity_2m,weather_code,"
        "wind_speed_10m,wind_direction_10m"
        "&daily=sunrise,sunset&forecast_days=1&timezone=auto"
        "&temperature_unit=fahrenheit&wind_speed_unit=mph",
        lat, lon);

    CURL *curl = curl_easy_init();
    if (!curl) return;

    Buf buf = { NULL, 0 };
    curl_easy_setopt(curl, CURLOPT_URL,            url);
    curl_easy_setopt(curl, CURLOPT_USERAGENT,      "WeatherACAP/" WEATHER_ACAP_VERSION);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION,  write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA,      &buf);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT,        20L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 8L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL,       1L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);

    CURLcode rc = curl_easy_perform(curl);
    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
    curl_easy_cleanup(curl);

    if (rc != CURLE_OK || !buf.data) {
        syslog(LOG_WARNING, "openmeteo: fetch failed: %s", curl_easy_strerror(rc));
        free(buf.data);
        return;
    }
    if (http_code >= 400) {
        syslog(LOG_WARNING, "openmeteo: HTTP %ld body=%.120s", http_code, buf.data);
        free(buf.data);
        return;
    }

    cJSON *root = cJSON_Parse(buf.data);
    free(buf.data);
    if (!root) return;

    cJSON *current = cJSON_GetObjectItem(root, "current");
    if (!current || cJSON_IsNull(current)) { cJSON_Delete(root); return; }

    cJSON *t   = cJSON_GetObjectItem(current, "temperature_2m");
    cJSON *rh  = cJSON_GetObjectItem(current, "relative_humidity_2m");
    cJSON *wc  = cJSON_GetObjectItem(current, "weather_code");
    cJSON *ws  = cJSON_GetObjectItem(current, "wind_speed_10m");
    cJSON *wd  = cJSON_GetObjectItem(current, "wind_direction_10m");

    /* Temperature is required for a valid observation; the rest are
     * reported as -1 when absent so thresholds can skip them. */
    if (!cJSON_IsNumber(t)) { cJSON_Delete(root); return; }
    result->temp_f = t->valuedouble;
    if (cJSON_IsNumber(rh)) result->humidity_pct    = (int)(rh->valuedouble + 0.5);
    if (cJSON_IsNumber(ws)) result->wind_speed_mph  = ws->valuedouble;
    if (cJSON_IsNumber(wd)) result->wind_dir_deg    = ((int)wd->valuedouble % 360 + 360) % 360;

    int code = cJSON_IsNumber(wc) ? (int)wc->valuedouble : -1;
    snprintf(result->description, sizeof(result->description), "%s", wmo_description(code));

    /* Daily.sunrise[0] / Daily.sunset[0] arrive as ISO "2026-04-22T06:42"
     * (sometimes with seconds).  Keep just "HH:MM". */
    cJSON *daily = cJSON_GetObjectItem(root, "daily");
    if (daily) {
        cJSON *sr_arr = cJSON_GetObjectItem(daily, "sunrise");
        cJSON *ss_arr = cJSON_GetObjectItem(daily, "sunset");
        cJSON *sr0 = (sr_arr && cJSON_GetArraySize(sr_arr) > 0)
                      ? cJSON_GetArrayItem(sr_arr, 0) : NULL;
        cJSON *ss0 = (ss_arr && cJSON_GetArraySize(ss_arr) > 0)
                      ? cJSON_GetArrayItem(ss_arr, 0) : NULL;
        if (cJSON_IsString(sr0) && sr0->valuestring) {
            const char *tp = strchr(sr0->valuestring, 'T');
            snprintf(result->sunrise, sizeof(result->sunrise), "%.5s",
                     tp ? tp + 1 : sr0->valuestring);
        }
        if (cJSON_IsString(ss0) && ss0->valuestring) {
            const char *tp = strchr(ss0->valuestring, 'T');
            snprintf(result->sunset, sizeof(result->sunset), "%.5s",
                     tp ? tp + 1 : ss0->valuestring);
        }
    }

    result->valid = 1;
    cJSON_Delete(root);
#else
    (void)lat; (void)lon;
#endif
}
