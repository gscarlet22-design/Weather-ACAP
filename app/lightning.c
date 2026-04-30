#include "lightning.h"
#include "cJSON.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>

#ifndef CGI_NO_CURL
#include <curl/curl.h>

#define SPC_URL "https://www.spc.noaa.gov/products/outlook/day1otlk_cat.nolyr.geojson"

/* ── libcurl write buffer ───────────────────────────────────────────────── */
typedef struct { char *data; size_t len; size_t cap; } CurlBuf;

static size_t write_cb(void *ptr, size_t sz, size_t nmemb, void *ud) {
    CurlBuf *b = (CurlBuf *)ud;
    size_t n = sz * nmemb;
    if (b->len + n + 1 > b->cap) {
        size_t newcap = b->cap ? b->cap * 2 : 65536;
        while (newcap < b->len + n + 1) newcap *= 2;
        char *p = (char *)realloc(b->data, newcap);
        if (!p) return 0;
        b->data = p;
        b->cap  = newcap;
    }
    memcpy(b->data + b->len, ptr, n);
    b->len += n;
    b->data[b->len] = '\0';
    return n;
}

static char *fetch_spc(void) {
    CURL *curl = curl_easy_init();
    if (!curl) return NULL;

    CurlBuf buf = {NULL, 0, 0};
    curl_easy_setopt(curl, CURLOPT_URL,            SPC_URL);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION,  write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA,      &buf);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT,        15L);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT,      "WeatherACAP/2.0");
    /* SPC serves over HTTPS — accept its certificate */
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);

    CURLcode rc = curl_easy_perform(curl);
    curl_easy_cleanup(curl);

    if (rc != CURLE_OK) {
        syslog(LOG_WARNING, "lightning: SPC fetch failed: %s",
               curl_easy_strerror(rc));
        free(buf.data);
        return NULL;
    }
    return buf.data;  /* caller frees */
}

#endif /* CGI_NO_CURL */

/* ── Risk level helpers ─────────────────────────────────────────────────── */

static int label_to_level(const char *label) {
    if (!label) return 0;
    if (strcmp(label, "TSTM") == 0) return 1;
    if (strcmp(label, "MRGL") == 0) return 2;
    if (strcmp(label, "SLGT") == 0) return 3;
    if (strcmp(label, "ENH")  == 0) return 4;
    if (strcmp(label, "MDT")  == 0) return 5;
    if (strcmp(label, "HIGH") == 0) return 6;
    return 0;
}

const char *lightning_risk_label(int risk_level) {
    switch (risk_level) {
        case 1: return "General Thunderstorm";
        case 2: return "Marginal Risk";
        case 3: return "Slight Risk";
        case 4: return "Enhanced Risk";
        case 5: return "Moderate Risk";
        case 6: return "High Risk";
        default: return "";
    }
}

/* ── Point-in-polygon (ray casting) ────────────────────────────────────── */
/*
 * GeoJSON coordinates are [longitude, latitude] order.
 * A ring is a cJSON array of [lon, lat] pairs.
 * Returns 1 if (lon, lat) is inside the ring.
 */
static int point_in_ring(double lon, double lat, cJSON *ring) {
    int n = cJSON_GetArraySize(ring);
    if (n < 3) return 0;
    int inside = 0;
    int j = n - 1;
    for (int i = 0; i < n; i++) {
        cJSON *pi = cJSON_GetArrayItem(ring, i);
        cJSON *pj = cJSON_GetArrayItem(ring, j);
        if (!pi || !pj) { j = i; continue; }
        cJSON *xi_j = cJSON_GetArrayItem(pi, 0);
        cJSON *yi_j = cJSON_GetArrayItem(pi, 1);
        cJSON *xj_j = cJSON_GetArrayItem(pj, 0);
        cJSON *yj_j = cJSON_GetArrayItem(pj, 1);
        if (!cJSON_IsNumber(xi_j) || !cJSON_IsNumber(yi_j) ||
            !cJSON_IsNumber(xj_j) || !cJSON_IsNumber(yj_j)) {
            j = i;
            continue;
        }
        double xi = xi_j->valuedouble, yi = yi_j->valuedouble;
        double xj = xj_j->valuedouble, yj = yj_j->valuedouble;
        if (((yi > lat) != (yj > lat)) &&
            (lon < (xj - xi) * (lat - yi) / (yj - yi) + xi))
            inside = !inside;
        j = i;
    }
    return inside;
}

/* Test a single GeoJSON Polygon geometry (coordinates = array of rings). */
static int point_in_polygon(double lon, double lat, cJSON *coords) {
    /* coords[0] = exterior ring, coords[1..] = holes */
    cJSON *exterior = cJSON_GetArrayItem(coords, 0);
    if (!exterior) return 0;
    if (!point_in_ring(lon, lat, exterior)) return 0;
    /* If inside exterior, check it's not in a hole */
    int nholes = cJSON_GetArraySize(coords);
    for (int h = 1; h < nholes; h++) {
        cJSON *hole = cJSON_GetArrayItem(coords, h);
        if (hole && point_in_ring(lon, lat, hole)) return 0;
    }
    return 1;
}

/* ── Public API ─────────────────────────────────────────────────────────── */

int lightning_check(double lat, double lon, LightningRisk *out) {
    if (out) { out->label[0] = '\0'; out->risk_level = 0; }

#ifdef CGI_NO_CURL
    (void)lat; (void)lon;
    return -1;
#else
    char *raw = fetch_spc();
    if (!raw) return -1;

    cJSON *root = cJSON_Parse(raw);
    free(raw);
    if (!root) {
        syslog(LOG_WARNING, "lightning: failed to parse SPC GeoJSON");
        return -1;
    }

    int best_level = 0;
    char best_label[8] = "";

    cJSON *features = cJSON_GetObjectItem(root, "features");
    if (!cJSON_IsArray(features)) { cJSON_Delete(root); return 0; }

    int nf = cJSON_GetArraySize(features);
    for (int fi = 0; fi < nf; fi++) {
        cJSON *feat = cJSON_GetArrayItem(features, fi);
        if (!feat) continue;

        /* Get risk label from properties */
        cJSON *props = cJSON_GetObjectItem(feat, "properties");
        cJSON *label_j = props ? cJSON_GetObjectItem(props, "LABEL") : NULL;
        if (!cJSON_IsString(label_j)) continue;
        const char *lbl = label_j->valuestring;
        int level = label_to_level(lbl);
        if (level == 0) continue;  /* unknown label */

        cJSON *geom = cJSON_GetObjectItem(feat, "geometry");
        if (!geom) continue;
        cJSON *type_j = cJSON_GetObjectItem(geom, "type");
        cJSON *coords = cJSON_GetObjectItem(geom, "coordinates");
        if (!cJSON_IsString(type_j) || !cJSON_IsArray(coords)) continue;

        const char *gtype = type_j->valuestring;
        int hit = 0;

        if (strcmp(gtype, "Polygon") == 0) {
            hit = point_in_polygon(lon, lat, coords);
        } else if (strcmp(gtype, "MultiPolygon") == 0) {
            int np = cJSON_GetArraySize(coords);
            for (int pi = 0; pi < np && !hit; pi++) {
                cJSON *poly = cJSON_GetArrayItem(coords, pi);
                if (poly) hit = point_in_polygon(lon, lat, poly);
            }
        }

        if (hit && level > best_level) {
            best_level = level;
            snprintf(best_label, sizeof(best_label), "%s", lbl);
        }
    }

    cJSON_Delete(root);

    if (out && best_level > 0) {
        snprintf(out->label, sizeof(out->label), "%s", best_label);
        out->risk_level = best_level;
    }

    syslog(LOG_DEBUG, "lightning: SPC check at (%.4f, %.4f): level=%d (%s)",
           lat, lon, best_level, best_label[0] ? best_label : "none");

    return best_level > 0 ? 1 : 0;
#endif /* CGI_NO_CURL */
}
