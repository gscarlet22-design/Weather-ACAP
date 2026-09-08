#ifndef WEATHER_ACAP_VERSION_H
#define WEATHER_ACAP_VERSION_H

/*
 * Single source of truth for the application version string.
 *
 * Keep in sync with app/manifest.json "version" and the top entry of
 * CHANGELOG.md.  Used for the HTTP User-Agent sent to NWS / Open-Meteo /
 * SPC / webhooks, and exposed to the web UI via the CGI `config` endpoint.
 */
#define WEATHER_ACAP_VERSION "1.1.0"

#endif /* WEATHER_ACAP_VERSION_H */
