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
    double wind_speed_mph;    /* -1 = not reported */
    int    wind_dir_deg;      /* 0-359, -1 = not reported */
    int    humidity_pct;      /* -1 = not reported */
    int    valid;             /* 1 only when temperature was a real number */
} NWSObservation;

/* One active NWS alert. */
typedef struct {
    char event[128];     /* e.g., "Tornado Warning" */
    char headline[256];
} NWSAlert;

#define NWS_MAX_ALERTS 32

typedef struct {
    NWSAlert alerts[NWS_MAX_ALERTS];
    int      count;
    /* 1 when the /alerts/active request succeeded and parsed.  0 means
     * "unknown" — callers MUST NOT treat an empty list as "no alerts" when
     * this is 0, or a transient NWS outage clears every active port. */
    int      fetch_ok;
} NWSAlertSet;

/* ── Functions ───────────────────────────────────────────────────────────── */

/* Resolve a US ZIP code to lat/lon.  Sets result->valid = 1 on success. */
void nws_geocode_zip(const char *zip, const char *user_agent, NWSCoords *result);

/* Fetch current observations for the nearest NWS station.  The station
 * lookup (2 round trips) is cached per coordinate pair for the life of the
 * process and re-resolved if the cached station stops answering. */
void nws_get_observation(double lat, double lon, const char *user_agent,
                         NWSObservation *result);

/* Fetch active NWS alerts for a point.  Only status=Actual products are
 * returned; Cancel/Expire message types are skipped. */
void nws_get_alerts(double lat, double lon, const char *user_agent,
                    NWSAlertSet *result);

#endif /* NWS_H */
