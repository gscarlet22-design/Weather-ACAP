#ifndef OPENMETEO_H
#define OPENMETEO_H

typedef struct {
    double temp_f;
    char   description[128]; /* derived from WMO weather code */
    double wind_speed_mph;
    int    wind_dir_deg;
    int    humidity_pct;
    int    valid;
} OMObservation;

/* Fetch current conditions from Open-Meteo (no API key required). */
void openmeteo_get_observation(double lat, double lon, OMObservation *result);

#endif /* OPENMETEO_H */
