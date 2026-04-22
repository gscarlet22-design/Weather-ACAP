#ifndef OPENMETEO_H
#define OPENMETEO_H

typedef struct {
    double temp_f;
    char   description[128]; /* derived from WMO weather code */
    double wind_speed_mph;
    int    wind_dir_deg;
    int    humidity_pct;
    /* Sun times — local "HH:MM" 24h, empty if not provided.  Open-Meteo
     * returns ISO timestamps "2026-04-22T06:42" — we store the time part. */
    char   sunrise[8];
    char   sunset[8];
    int    valid;
} OMObservation;

/* Fetch current conditions from Open-Meteo (no API key required). */
void openmeteo_get_observation(double lat, double lon, OMObservation *result);

#endif /* OPENMETEO_H */
