#ifndef WEATHER_API_H
#define WEATHER_API_H

#include "nws.h"

/* Unified current conditions — filled by whichever provider succeeds. */
typedef struct {
    double temp_f;
    char   description[128];
    double wind_speed_mph;
    int    wind_dir_deg;        /* -1 = unknown */
    int    humidity_pct;
    char   provider[16];        /* "nws" or "openmeteo" */
    int    valid;
} WeatherConditions;

/* Full weather snapshot returned by weather_api_fetch(). */
typedef struct {
    WeatherConditions conditions;
    NWSAlertSet       alerts;
    double            lat;
    double            lon;
} WeatherSnapshot;

/*
 * Fetch current conditions + alerts.
 *
 * provider: "auto" | "nws" | "openmeteo"
 *   auto  = try NWS first; fall back to Open-Meteo if NWS fails
 *   nws   = NWS only
 *   openmeteo = Open-Meteo only (no alerts)
 *
 * zip, lat_override, lon_override: set lat_override/lon_override to non-empty
 * strings to skip ZIP geocoding and use the given coordinates directly.
 *
 * Returns 1 on success (at least conditions.valid or alerts.count > 0), 0 on total failure.
 */
int weather_api_fetch(const char *provider,
                      const char *zip,
                      const char *lat_override,
                      const char *lon_override,
                      const char *user_agent,
                      WeatherSnapshot *snap);

/* Wind direction degrees → compass abbreviation (e.g., 225 → "SW"). */
const char *weather_wind_dir_str(int deg);

#endif /* WEATHER_API_H */
