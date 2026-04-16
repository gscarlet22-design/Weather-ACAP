#include "weather_api.h"
#include "nws.h"
#include "openmeteo.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

const char *weather_wind_dir_str(int deg) {
    if (deg < 0) return "---";
    static const char *d[] = { "N","NNE","NE","ENE","E","ESE","SE","SSE",
                                "S","SSW","SW","WSW","W","WNW","NW","NNW" };
    return d[((deg + 11) % 360) / 23];
}

/* ── coordinate resolution ───────────────────────────────────────────────── */

static int resolve_coords(const char *zip,
                           const char *lat_ov, const char *lon_ov,
                           const char *user_agent,
                           double *lat_out, double *lon_out) {
    if (lat_ov && *lat_ov && lon_ov && *lon_ov) {
        *lat_out = atof(lat_ov);
        *lon_out = atof(lon_ov);
        return (*lat_out != 0.0 || *lon_out != 0.0);
    }
    if (!zip || !*zip) return 0;

    NWSCoords c;
    nws_geocode_zip(zip, user_agent, &c);
    if (!c.valid) return 0;
    *lat_out = c.lat;
    *lon_out = c.lon;
    return 1;
}

/* ── main fetch ──────────────────────────────────────────────────────────── */

int weather_api_fetch(const char *provider,
                      const char *zip,
                      const char *lat_override,
                      const char *lon_override,
                      const char *user_agent,
                      WeatherSnapshot *snap) {
    memset(snap, 0, sizeof(*snap));
    snap->conditions.wind_dir_deg = -1;

    double lat = 0.0, lon = 0.0;
    if (!resolve_coords(zip, lat_override, lon_override, user_agent, &lat, &lon))
        return 0;

    snap->lat = lat;
    snap->lon = lon;

    /* ── Conditions ─────────────────────────────────────────────────────── */
    int use_nws = (strcmp(provider, "openmeteo") != 0);
    int use_om  = (strcmp(provider, "nws")       != 0);

    if (use_nws) {
        NWSObservation obs;
        nws_get_observation(lat, lon, user_agent, &obs);
        if (obs.valid) {
            snap->conditions.temp_f        = obs.temp_f;
            snap->conditions.wind_speed_mph = obs.wind_speed_mph;
            snap->conditions.wind_dir_deg  = obs.wind_dir_deg;
            snap->conditions.humidity_pct  = obs.humidity_pct;
            snprintf(snap->conditions.description, sizeof(snap->conditions.description),
                     "%s", obs.description);
            snprintf(snap->conditions.provider, sizeof(snap->conditions.provider), "nws");
            snap->conditions.valid = 1;
        }
    }

    if (!snap->conditions.valid && use_om) {
        OMObservation om;
        openmeteo_get_observation(lat, lon, &om);
        if (om.valid) {
            snap->conditions.temp_f        = om.temp_f;
            snap->conditions.wind_speed_mph = om.wind_speed_mph;
            snap->conditions.wind_dir_deg  = om.wind_dir_deg;
            snap->conditions.humidity_pct  = om.humidity_pct;
            snprintf(snap->conditions.description, sizeof(snap->conditions.description),
                     "%s", om.description);
            snprintf(snap->conditions.provider, sizeof(snap->conditions.provider), "openmeteo");
            snap->conditions.valid = 1;
        }
    }

    /* ── Alerts (NWS only — no open-meteo alerts) ────────────────────────── */
    if (use_nws)
        nws_get_alerts(lat, lon, user_agent, &snap->alerts);

    return snap->conditions.valid || snap->alerts.count > 0;
}
