#include "lightning.h"
#include "cJSON.h"
#include "version.h"

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
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 8L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL,       1L);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_MAXREDIRS,      3L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT,      "WeatherACAP/" WEATHER_ACAP_VERSION);
    /* SPC serves over HTTPS — accept its certificate */
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);

    CURLcode rc = curl_easy_perform(curl);
    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
    curl_easy_cleanup(curl);

    if (rc != CURLE_OK) {
        syslog(LOG_WARNING, "lightning: SPC fetch failed: %s",
               curl_easy_strerror(rc));
        free(buf.data);
        return NULL;
    }
    if (http_code >= 400) {
        syslog(LOG_WARNING, "lightning: SPC HTTP %ld", http_code);
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
/* [lon, lat] pair → doubles.  Walks the pair's own child list rather than
 * calling cJSON_GetArrayItem (which is O(n) from the head each time). */
static int get_xy(const cJSON *pt, double *x, double *y) {
    if (!cJSON_IsArray(pt)) return 0;
    const cJSON *a = pt->child;
    const cJSON *b = a ? a->next : NULL;
    if (!cJSON_IsNumber(a) || !cJSON_IsNumber(b)) return 0;
    *x = a->valuedouble;
    *y = b->valuedouble;
    return 1;
}

static int point_in_ring(double lon, double lat, const cJSON *ring) {
    if (!cJSON_IsArray(ring) || !ring->child) return 0;

    /* Previous vertex starts as the last vertex (closes the ring).  A
     * single linked-list walk: SPC rings have thousands of vertices, and
     * the old index-based loop was O(V²) on the single GLib thread. */
    const cJSON *last = ring->child;
    while (last->next) last = last->next;
    double xj, yj;
    if (!get_xy(last, &xj, &yj)) return 0;

    int inside = 0;
    for (const cJSON *pi = ring->child; pi; pi = pi->next) {
        double xi, yi;
        if (!get_xy(pi, &xi, &yi)) continue;
        if (((yi > lat) != (yj > lat)) &&
            (lon < (xj - xi) * (lat - yi) / (yj - yi) + xi))
            inside = !inside;
        xj = xi; yj = yi;
    }
    return inside;
}

/* Test a single GeoJSON Polygon geometry (coordinates = array of rings). */
static int point_in_polygon(double lon, double lat, const cJSON *coords) {
    /* coords[0] = exterior ring, coords[1..] = holes */
    if (!cJSON_IsArray(coords)) return 0;
    const cJSON *exterior = coords->child;
    if (!exterior) return 0;
    if (!point_in_ring(lon, lat, exterior)) return 0;
    for (const cJSON *hole = exterior->next; hole; hole = hole->next)
        if (point_in_ring(lon, lat, hole)) return 0;
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

    for (cJSON *feat = features->child; feat; feat = feat->next) {
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
            for (cJSON *poly = coords->child; poly && !hit; poly = poly->next)
                hit = point_in_polygon(lon, lat, poly);
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
