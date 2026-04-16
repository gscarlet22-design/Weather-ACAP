#ifndef NWS_H
#define NWS_H

#include <stddef.h>

/* ── Data structures ─────────────────────────────────────────────────────── */

typedef struct {
    double lat;
    double lon;
    int    valid;   /* 1 if geocoding succeeded */
} NWSCoords;

typedef struct {
    double temp_f;
    char   description[128]; /* e.g., "Mostly Cloudy" */
    double wind_speed_mph;
    int    wind_dir_deg;      /* 0-359 */
    int    humidity_pct;
    int    valid;
} NWSObservation;

/* One active NWS alert. */
typedef struct {
    char event[128];     /* e.g., "Tornado Warning" */
    char headline[256];
} NWSAlert;

#define NWS_MAX_ALERTS 16

typedef struct {
    NWSAlert alerts[NWS_MAX_ALERTS];
    int      count;
} NWSAlertSet;

/* ── Functions ───────────────────────────────────────────────────────────── */

/* Resolve a US ZIP code to lat/lon via Census Geocoder.
 * Sets result->valid = 1 on success. */
void nws_geocode_zip(const char *zip, const char *user_agent, NWSCoords *result);

/* Fetch current observations for the nearest NWS station. */
void nws_get_observation(double lat, double lon, const char *user_agent,
                         NWSObservation *result);

/* Fetch active NWS alerts for a point. */
void nws_get_alerts(double lat, double lon, const char *user_agent,
                    NWSAlertSet *result);

#endif /* NWS_H */
