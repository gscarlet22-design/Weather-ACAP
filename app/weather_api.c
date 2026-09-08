#include "weather_api.h"
#include "nws.h"
#include "openmeteo.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>
#include <time.h>

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

    /* A ZIP never moves; geocode it once per process. */
    static char   s_zip[16] = "";
    static double s_lat = 0.0, s_lon = 0.0;
    if (s_zip[0] && strcmp(s_zip, zip) == 0) {
        *lat_out = s_lat;
        *lon_out = s_lon;
        return 1;
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
    snprintf(s_zip, sizeof(s_zip), "%s", zip);
    s_lat = c.lat;
    s_lon = c.lon;
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
    snap->conditions.wind_dir_deg   = -1;
    snap->conditions.wind_speed_mph = -1;
    snap->conditions.humidity_pct   = -1;

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

    /* Sun times: NWS doesn't expose them, so Open-Meteo is asked — but
     * only once per calendar day, not every poll.  This cuts one HTTPS
     * request from every cycle in "auto"/"nws" mode. */
    if (snap->conditions.valid && !snap->conditions.sunrise[0]) {
        static char   s_sunrise[8] = "", s_sunset[8] = "";
        static int    s_yday = -1;
        static double s_lat = 0.0, s_lon = 0.0;

        time_t now = time(NULL);
        struct tm tm;
        localtime_r(&now, &tm);

        if (!(s_yday == tm.tm_yday && s_lat == lat && s_lon == lon && s_sunrise[0])) {
            OMObservation om;
            openmeteo_get_observation(lat, lon, &om);
            if (om.valid) {
                snprintf(s_sunrise, sizeof(s_sunrise), "%s", om.sunrise);
                snprintf(s_sunset,  sizeof(s_sunset),  "%s", om.sunset);
                s_yday = tm.tm_yday; s_lat = lat; s_lon = lon;
            }
        }
        snprintf(snap->conditions.sunrise, sizeof(snap->conditions.sunrise), "%s", s_sunrise);
        snprintf(snap->conditions.sunset,  sizeof(snap->conditions.sunset),  "%s", s_sunset);
    }

    /* ── Alerts (NWS only — no open-meteo alerts) ────────────────────────── */
    if (use_nws) {
        nws_get_alerts(lat, lon, user_agent, &snap->alerts);
        if (!snap->alerts.fetch_ok)
            syslog(LOG_WARNING,
                   "weather: alerts fetch FAILED — alert port state will be held, not cleared");
    } else {
        /* Open-Meteo-only mode has no alert source: "no alerts" is a known
         * fact, so mapped ports may legitimately clear. */
        snap->alerts.fetch_ok = 1;
    }

    /* Success means "something actionable": current conditions, or a
     * trustworthy alert list (possibly empty).  Callers gate each consumer
     * on the specific flag it needs. */
    return snap->conditions.valid || snap->alerts.fetch_ok;
}
