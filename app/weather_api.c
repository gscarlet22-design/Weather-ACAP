#include "weather_api.h"
#include "nws.h"
#include "openmeteo.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>

const char *weather_wind_dir_str(int deg) {
    if (deg < 0) return "---";
    static const char *d[] = { "N","NNE","NE","ENE","E","ESE","SE","SSE",
                                "S","SSW","SW","WSW","W","WNW","NW","NNW" };
    return d[((deg + 11) % 360) / 23];
}

/* 8-way arrow.  Wind direction is "from" — arrow points the way the
 * wind is going (N wind blows southward → ↓).  Useful in overlays
 * where a glyph reads faster than the abbreviation. */
const char *weather_wind_dir_arrow(int deg) {
    if (deg < 0) return "·";
    static const char *a[] = { "↓","↙","←","↖","↑","↗","→","↘" };
    return a[((deg + 22) % 360) / 45];
}

/* ── coordinate resolution ───────────────────────────────────────────────── */

static int resolve_coords(const char *zip,
                           const char *lat_ov, const char *lon_ov,
                           const char *user_agent,
                           double *lat_out, double *lon_out) {
    if (lat_ov && *lat_ov && lon_ov && *lon_ov) {
        *lat_out = atof(lat_ov);
        *lon_out = atof(lon_ov);
        int ok = (*lat_out != 0.0 || *lon_out != 0.0);
        syslog(LOG_INFO,
               "weather: resolve_coords via override → lat=%.6f lon=%.6f ok=%d",
               *lat_out, *lon_out, ok);
        return ok;
    }
    if (!zip || !*zip) {
        syslog(LOG_WARNING,
               "weather: resolve_coords FAILED: no ZIP and no lat/lon override");
        return 0;
    }

    NWSCoords c;
    nws_geocode_zip(zip, user_agent, &c);
    if (!c.valid) {
        syslog(LOG_WARNING,
               "weather: nws_geocode_zip(\"%s\") FAILED: invalid response",
               zip);
        return 0;
    }
    *lat_out = c.lat;
    *lon_out = c.lon;
    syslog(LOG_INFO,
           "weather: nws_geocode_zip(\"%s\") → lat=%.6f lon=%.6f",
           zip, c.lat, c.lon);
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
        syslog(LOG_INFO,
               "weather: nws_get_observation lat=%.4f lon=%.4f → valid=%d",
               lat, lon, obs.valid);
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
        syslog(LOG_INFO,
               "weather: openmeteo_get_observation lat=%.4f lon=%.4f → valid=%d",
               lat, lon, om.valid);
        if (om.valid) {
            snap->conditions.temp_f        = om.temp_f;
            snap->conditions.wind_speed_mph = om.wind_speed_mph;
            snap->conditions.wind_dir_deg  = om.wind_dir_deg;
            snap->conditions.humidity_pct  = om.humidity_pct;
            snprintf(snap->conditions.description, sizeof(snap->conditions.description),
                     "%s", om.description);
            snprintf(snap->conditions.provider, sizeof(snap->conditions.provider), "openmeteo");
            snprintf(snap->conditions.sunrise, sizeof(snap->conditions.sunrise),
                     "%s", om.sunrise);
            snprintf(snap->conditions.sunset, sizeof(snap->conditions.sunset),
                     "%s", om.sunset);
            snap->conditions.valid = 1;
        }
    }

    /* Even when NWS supplied conditions, fall back to Open-Meteo just for
     * sun times (NWS doesn't expose them).  Cheap second call, free tier. */
    if (snap->conditions.valid && !snap->conditions.sunrise[0]) {
        OMObservation om;
        openmeteo_get_observation(lat, lon, &om);
        if (om.valid) {
            snprintf(snap->conditions.sunrise, sizeof(snap->conditions.sunrise),
                     "%s", om.sunrise);
            snprintf(snap->conditions.sunset, sizeof(snap->conditions.sunset),
                     "%s", om.sunset);
        }
    }

    /* ── Alerts (NWS only — no open-meteo alerts) ────────────────────────── */
    if (use_nws)
        nws_get_alerts(lat, lon, user_agent, &snap->alerts);

    return snap->conditions.valid || snap->alerts.count > 0;
}
