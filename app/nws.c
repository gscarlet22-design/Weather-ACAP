#include "nws.h"
#include "cJSON.h"

#include <curl/curl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

/* ── libcurl response buffer ─────────────────────────────────────────────── */

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

static char *http_get(const char *url, const char *user_agent) {
    CURL *curl = curl_easy_init();
    if (!curl) return NULL;

    Buf buf = { NULL, 0 };
    curl_easy_setopt(curl, CURLOPT_URL,           url);
    curl_easy_setopt(curl, CURLOPT_USERAGENT,     user_agent);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA,     &buf);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT,       20L);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);

    struct curl_slist *hdrs = curl_slist_append(NULL, "Accept: application/geo+json,application/json");
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, hdrs);

    CURLcode rc = curl_easy_perform(curl);
    curl_slist_free_all(hdrs);
    curl_easy_cleanup(curl);

    if (rc != CURLE_OK) { free(buf.data); return NULL; }
    return buf.data; /* caller must free */
}

/* ── Census Geocoder ─────────────────────────────────────────────────────── */

void nws_geocode_zip(const char *zip, const char *user_agent, NWSCoords *result) {
    result->valid = 0;
    if (!zip || !*zip) return;

    char url[256];
    snprintf(url, sizeof(url),
        "https://geocoding.geo.census.gov/geocoder/locations/address"
        "?benchmark=2020&format=json&zip=%s", zip);

    char *body = http_get(url, user_agent);
    if (!body) return;

    cJSON *root = cJSON_Parse(body);
    free(body);
    if (!root) return;

    /* result.addressMatches[0].coordinates.{x=lon, y=lat} */
    cJSON *res     = cJSON_GetObjectItem(root, "result");
    cJSON *matches = res ? cJSON_GetObjectItem(res, "addressMatches") : NULL;
    cJSON *first   = (matches && cJSON_GetArraySize(matches) > 0)
                     ? cJSON_GetArrayItem(matches, 0) : NULL;
    cJSON *coords  = first ? cJSON_GetObjectItem(first, "coordinates") : NULL;
    cJSON *x       = coords ? cJSON_GetObjectItem(coords, "x") : NULL;
    cJSON *y       = coords ? cJSON_GetObjectItem(coords, "y") : NULL;

    if (cJSON_IsNumber(x) && cJSON_IsNumber(y)) {
        result->lon   = x->valuedouble;
        result->lat   = y->valuedouble;
        result->valid = 1;
    }
    cJSON_Delete(root);
}

/* ── NWS /points → nearest observation station ───────────────────────────── */

static char *nws_get_station_id(double lat, double lon, const char *user_agent) {
    /* Step 1: /points to get observationStations URL */
    char url[256];
    snprintf(url, sizeof(url), "https://api.weather.gov/points/%.4f,%.4f", lat, lon);

    char *body = http_get(url, user_agent);
    if (!body) return NULL;

    cJSON *root  = cJSON_Parse(body);
    free(body);
    if (!root) return NULL;

    cJSON *props  = cJSON_GetObjectItem(root, "properties");
    cJSON *sta_url = props ? cJSON_GetObjectItem(props, "observationStations") : NULL;
    char  *sta_url_str = NULL;
    if (cJSON_IsString(sta_url))
        sta_url_str = strdup(sta_url->valuestring);
    cJSON_Delete(root);
    if (!sta_url_str) return NULL;

    /* Step 2: fetch the stations list, take the first station */
    body = http_get(sta_url_str, user_agent);
    free(sta_url_str);
    if (!body) return NULL;

    root = cJSON_Parse(body);
    free(body);
    if (!root) return NULL;

    char *station_id = NULL;
    cJSON *features = cJSON_GetObjectItem(root, "features");
    if (cJSON_IsArray(features) && cJSON_GetArraySize(features) > 0) {
        cJSON *feat  = cJSON_GetArrayItem(features, 0);
        cJSON *props2 = feat ? cJSON_GetObjectItem(feat, "properties") : NULL;
        cJSON *sid    = props2 ? cJSON_GetObjectItem(props2, "stationIdentifier") : NULL;
        if (cJSON_IsString(sid))
            station_id = strdup(sid->valuestring);
    }
    cJSON_Delete(root);
    return station_id;
}

/* ── NWS latest observation ──────────────────────────────────────────────── */

void nws_get_observation(double lat, double lon, const char *user_agent,
                         NWSObservation *result) {
    memset(result, 0, sizeof(*result));

    char *station_id = nws_get_station_id(lat, lon, user_agent);
    if (!station_id) return;

    char url[256];
    snprintf(url, sizeof(url),
        "https://api.weather.gov/stations/%s/observations/latest", station_id);
    free(station_id);

    char *body = http_get(url, user_agent);
    if (!body) return;

    cJSON *root  = cJSON_Parse(body);
    free(body);
    if (!root) return;

    cJSON *props = cJSON_GetObjectItem(root, "properties");
    if (!props) { cJSON_Delete(root); return; }

    /* Temperature (Celsius → Fahrenheit) */
    cJSON *temp  = cJSON_GetObjectItem(props, "temperature");
    cJSON *tval  = temp ? cJSON_GetObjectItem(temp, "value") : NULL;
    if (cJSON_IsNumber(tval) && !cJSON_IsNull(tval))
        result->temp_f = tval->valuedouble * 9.0 / 5.0 + 32.0;

    /* Text description */
    cJSON *desc = cJSON_GetObjectItem(props, "textDescription");
    if (cJSON_IsString(desc))
        snprintf(result->description, sizeof(result->description), "%s", desc->valuestring);
    else
        snprintf(result->description, sizeof(result->description), "Unknown");

    /* Wind speed (m/s → mph) */
    cJSON *wspd  = cJSON_GetObjectItem(props, "windSpeed");
    cJSON *wval  = wspd ? cJSON_GetObjectItem(wspd, "value") : NULL;
    if (cJSON_IsNumber(wval))
        result->wind_speed_mph = wval->valuedouble * 2.23694;

    /* Wind direction (degrees) */
    cJSON *wdir  = cJSON_GetObjectItem(props, "windDirection");
    cJSON *wdval = wdir ? cJSON_GetObjectItem(wdir, "value") : NULL;
    if (cJSON_IsNumber(wdval))
        result->wind_dir_deg = (int)wdval->valuedouble;
    else
        result->wind_dir_deg = -1;

    /* Relative humidity */
    cJSON *rh   = cJSON_GetObjectItem(props, "relativeHumidity");
    cJSON *rhv  = rh ? cJSON_GetObjectItem(rh, "value") : NULL;
    if (cJSON_IsNumber(rhv))
        result->humidity_pct = (int)rhv->valuedouble;

    result->valid = 1;
    cJSON_Delete(root);
}

/* ── NWS active alerts ───────────────────────────────────────────────────── */

void nws_get_alerts(double lat, double lon, const char *user_agent,
                    NWSAlertSet *result) {
    memset(result, 0, sizeof(*result));

    char url[256];
    snprintf(url, sizeof(url),
        "https://api.weather.gov/alerts/active?point=%.4f,%.4f", lat, lon);

    char *body = http_get(url, user_agent);
    if (!body) return;

    cJSON *root = cJSON_Parse(body);
    free(body);
    if (!root) return;

    cJSON *features = cJSON_GetObjectItem(root, "features");
    if (!cJSON_IsArray(features)) { cJSON_Delete(root); return; }

    int n = cJSON_GetArraySize(features);
    for (int i = 0; i < n && result->count < NWS_MAX_ALERTS; i++) {
        cJSON *feat  = cJSON_GetArrayItem(features, i);
        cJSON *props = feat ? cJSON_GetObjectItem(feat, "properties") : NULL;
        if (!props) continue;

        cJSON *event    = cJSON_GetObjectItem(props, "event");
        cJSON *headline = cJSON_GetObjectItem(props, "headline");

        if (!cJSON_IsString(event)) continue;

        NWSAlert *a = &result->alerts[result->count++];
        snprintf(a->event,    sizeof(a->event),    "%s", event->valuestring);
        snprintf(a->headline, sizeof(a->headline), "%s",
                 cJSON_IsString(headline) ? headline->valuestring : "");
    }
    cJSON_Delete(root);
}
