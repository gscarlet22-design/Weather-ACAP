#include "nws.h"
#include "cJSON.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <math.h>
#include <syslog.h>

#ifndef CGI_NO_CURL
#include <curl/curl.h>

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

/* GET url → body (caller frees).  Returns NULL on transport error OR on
 * HTTP >= 400: NWS error bodies are application/problem+json and parsing
 * one as data yields "no features" == "no alerts", which is exactly the
 * failure this must prevent. */
static char *http_get(const char *url, const char *user_agent) {
    CURL *curl = curl_easy_init();
    if (!curl) {
        syslog(LOG_WARNING, "nws/http_get: curl_easy_init failed");
        return NULL;
    }

    Buf buf = { NULL, 0 };
    curl_easy_setopt(curl, CURLOPT_URL,            url);
    curl_easy_setopt(curl, CURLOPT_USERAGENT,      user_agent);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION,  write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA,      &buf);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT,        20L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 8L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL,       1L);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_MAXREDIRS,      3L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);

    struct curl_slist *hdrs = curl_slist_append(NULL, "Accept: application/geo+json,application/json");
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, hdrs);

    CURLcode rc = curl_easy_perform(curl);
    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
    curl_slist_free_all(hdrs);
    curl_easy_cleanup(curl);

    if (rc != CURLE_OK) {
        syslog(LOG_WARNING,
               "nws/http_get FAILED: url=%s curl=%s (rc=%d) http=%ld",
               url, curl_easy_strerror(rc), rc, http_code);
        free(buf.data);
        return NULL;
    }
    if (http_code >= 400) {
        syslog(LOG_WARNING,
               "nws/http_get HTTP %ld: url=%s body=%.120s",
               http_code, url, buf.data ? buf.data : "");
        free(buf.data);
        return NULL;
    }
    syslog(LOG_INFO, "nws/http_get ok: url=%s http=%ld bytes=%zu",
           url, http_code, buf.size);
    return buf.data; /* caller must free */
}

#endif /* CGI_NO_CURL */

/* ── ZIP geocoder ────────────────────────────────────────────────────────── */

void nws_geocode_zip(const char *zip, const char *user_agent, NWSCoords *result) {
    result->valid = 0;
#ifndef CGI_NO_CURL
    if (!zip || !*zip) return;

    /* Only digits are valid in a US ZIP; anything else would need URL
     * encoding and is a configuration error anyway. */
    for (const char *p = zip; *p; p++) {
        if (*p < '0' || *p > '9') {
            syslog(LOG_WARNING, "nws_geocode_zip(\"%s\"): not a numeric ZIP", zip);
            return;
        }
    }

    /* zippopotam.us — free, no auth, ZIP-only. */
    char url[256];
    snprintf(url, sizeof(url), "https://api.zippopotam.us/us/%s", zip);

    char *body = http_get(url, user_agent);
    if (!body) {
        syslog(LOG_WARNING, "nws_geocode_zip(\"%s\"): http_get returned NULL", zip);
        return;
    }

    cJSON *root = cJSON_Parse(body);
    free(body);
    if (!root) {
        syslog(LOG_WARNING, "nws_geocode_zip(\"%s\"): JSON parse failed", zip);
        return;
    }

    /* {"places":[{"latitude":"39.0577","longitude":"-94.6406", ...}]} */
    cJSON *places = cJSON_GetObjectItem(root, "places");
    cJSON *first  = (places && cJSON_GetArraySize(places) > 0)
                    ? cJSON_GetArrayItem(places, 0) : NULL;
    cJSON *lat_s  = first ? cJSON_GetObjectItem(first, "latitude")  : NULL;
    cJSON *lon_s  = first ? cJSON_GetObjectItem(first, "longitude") : NULL;

    if (cJSON_IsString(lat_s) && cJSON_IsString(lon_s)) {
        result->lat   = atof(lat_s->valuestring);
        result->lon   = atof(lon_s->valuestring);
        result->valid = (result->lat != 0.0 || result->lon != 0.0);
    } else {
        syslog(LOG_WARNING,
               "nws_geocode_zip(\"%s\"): no usable places[0] in response", zip);
    }
    cJSON_Delete(root);
#else
    (void)zip; (void)user_agent;
#endif
}

/* ── NWS /points → nearest observation station (cached) ─────────────────── */

#ifndef CGI_NO_CURL

/* The station for a fixed camera never changes, so the two round trips
 * (/points, then the stations list) are done once per coordinate pair.
 * The cache is dropped if the observation call for that station fails so
 * a decommissioned station is re-resolved on the next poll. */
static double s_sta_lat = 0.0, s_sta_lon = 0.0;
static char   s_sta_id[32] = "";

static char *nws_get_station_id(double lat, double lon, const char *user_agent) {
    if (s_sta_id[0] && s_sta_lat == lat && s_sta_lon == lon)
        return strdup(s_sta_id);

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
        if (cJSON_IsString(sid) && sid->valuestring && *sid->valuestring)
            station_id = strdup(sid->valuestring);
    }
    cJSON_Delete(root);

    if (station_id) {
        snprintf(s_sta_id, sizeof(s_sta_id), "%s", station_id);
        s_sta_lat = lat;
        s_sta_lon = lon;
        syslog(LOG_INFO, "nws: using observation station %s for %.4f,%.4f",
               s_sta_id, lat, lon);
    }
    return station_id;
}

/* Read a {"value":N,"unitCode":"wmoUnit:..."} quantity.  Returns 1 and
 * fills *out (converted with `factor(unit)`) when value is a number. */
static int read_quantity(const cJSON *props, const char *key,
                         double *out, const char **unit_out) {
    cJSON *q = cJSON_GetObjectItem(props, key);
    cJSON *v = q ? cJSON_GetObjectItem(q, "value") : NULL;
    cJSON *u = q ? cJSON_GetObjectItem(q, "unitCode") : NULL;
    if (unit_out) *unit_out = (cJSON_IsString(u) && u->valuestring) ? u->valuestring : "";
    if (!cJSON_IsNumber(v)) return 0;
    *out = v->valuedouble;
    return 1;
}

#endif /* CGI_NO_CURL */

/* ── NWS latest observation ──────────────────────────────────────────────── */

void nws_get_observation(double lat, double lon, const char *user_agent,
                         NWSObservation *result) {
    memset(result, 0, sizeof(*result));
    result->wind_speed_mph = -1;
    result->wind_dir_deg   = -1;
    result->humidity_pct   = -1;
#ifndef CGI_NO_CURL
    char *station_id = nws_get_station_id(lat, lon, user_agent);
    if (!station_id) return;

    char url[256];
    snprintf(url, sizeof(url),
        "https://api.weather.gov/stations/%s/observations/latest", station_id);
    free(station_id);

    char *body = http_get(url, user_agent);
    if (!body) {
        /* Station may have been retired — force re-resolution next time. */
        s_sta_id[0] = '\0';
        return;
    }

    cJSON *root  = cJSON_Parse(body);
    free(body);
    if (!root) return;

    cJSON *props = cJSON_GetObjectItem(root, "properties");
    if (!props || cJSON_IsNull(props)) { cJSON_Delete(root); return; }

    double v; const char *unit;

    /* Temperature — required.  A null here (QC-failed sensor, partial
     * SPECI) used to yield 0 °F with valid=1, which tripped "TempF < 32"
     * thresholds and logged a bogus dip.  Now the observation is invalid
     * and the caller falls back to Open-Meteo. */
    if (!read_quantity(props, "temperature", &v, &unit)) {
        syslog(LOG_INFO, "nws: observation has no temperature; treating as invalid");
        cJSON_Delete(root);
        return;
    }
    result->temp_f = strstr(unit, "degF") ? v : v * 9.0 / 5.0 + 32.0;

    /* Text description */
    cJSON *desc = cJSON_GetObjectItem(props, "textDescription");
    if (cJSON_IsString(desc) && desc->valuestring && *desc->valuestring)
        snprintf(result->description, sizeof(result->description), "%s", desc->valuestring);
    else
        snprintf(result->description, sizeof(result->description), "Unknown");

    /* Wind speed — honour unitCode.  The API reports km/h (it changed from
     * m/s in 2019); the old fixed m/s→mph factor over-reported by 3.6×. */
    if (read_quantity(props, "windSpeed", &v, &unit)) {
        if      (strstr(unit, "km_h")) result->wind_speed_mph = v * 0.621371;
        else if (strstr(unit, "m_s"))  result->wind_speed_mph = v * 2.23694;
        else if (strstr(unit, "mi_h")) result->wind_speed_mph = v;
        else                           result->wind_speed_mph = v * 0.621371;
    }

    /* Wind direction (degrees) */
    if (read_quantity(props, "windDirection", &v, NULL))
        result->wind_dir_deg = ((int)v % 360 + 360) % 360;

    /* Relative humidity */
    if (read_quantity(props, "relativeHumidity", &v, NULL))
        result->humidity_pct = (int)(v + 0.5);

    result->valid = 1;
    cJSON_Delete(root);
#else
    (void)lat; (void)lon; (void)user_agent;
#endif
}

/* ── NWS active alerts ───────────────────────────────────────────────────── */

void nws_get_alerts(double lat, double lon, const char *user_agent,
                    NWSAlertSet *result) {
    memset(result, 0, sizeof(*result));
#ifndef CGI_NO_CURL
    char url[256];
    snprintf(url, sizeof(url),
        "https://api.weather.gov/alerts/active?point=%.4f,%.4f", lat, lon);

    char *body = http_get(url, user_agent);
    if (!body) return;                       /* fetch_ok stays 0 */

    cJSON *root = cJSON_Parse(body);
    free(body);
    if (!root) return;

    cJSON *features = cJSON_GetObjectItem(root, "features");
    if (!cJSON_IsArray(features)) { cJSON_Delete(root); return; }

    int n = cJSON_GetArraySize(features);
    for (int i = 0; i < n && result->count < NWS_MAX_ALERTS; i++) {
        cJSON *feat  = cJSON_GetArrayItem(features, i);
        cJSON *props = feat ? cJSON_GetObjectItem(feat, "properties") : NULL;
        if (!props || cJSON_IsNull(props)) continue;

        cJSON *event    = cJSON_GetObjectItem(props, "event");
        cJSON *headline = cJSON_GetObjectItem(props, "headline");
        cJSON *status   = cJSON_GetObjectItem(props, "status");
        cJSON *mtype    = cJSON_GetObjectItem(props, "messageType");

        if (!cJSON_IsString(event) || !event->valuestring) continue;

        /* Skip Test/Exercise/System products and cancellations. */
        if (cJSON_IsString(status) && status->valuestring &&
            strcasecmp(status->valuestring, "Actual") != 0) continue;
        if (cJSON_IsString(mtype) && mtype->valuestring &&
            (strcasecmp(mtype->valuestring, "Cancel") == 0 ||
             strcasecmp(mtype->valuestring, "Expire") == 0)) continue;

        NWSAlert *a = &result->alerts[result->count++];
        snprintf(a->event,    sizeof(a->event),    "%s", event->valuestring);
        snprintf(a->headline, sizeof(a->headline), "%s",
                 (cJSON_IsString(headline) && headline->valuestring)
                 ? headline->valuestring : "");
    }
    if (n > NWS_MAX_ALERTS)
        syslog(LOG_WARNING, "nws: %d active products, keeping first %d",
               n, NWS_MAX_ALERTS);
    result->fetch_ok = 1;
    cJSON_Delete(root);
#else
    (void)lat; (void)lon; (void)user_agent;
#endif
}
