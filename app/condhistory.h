/*
 * condhistory.h — Periodic conditions history log.
 *
 * Appends one JSON line per poll to /tmp/weather_acap_cond.jsonl.
 * Line format:
 *   {"ts":"2026-04-28T14:25:37Z","temp_f":72.0,"wind_mph":8.0,
 *    "humidity_pct":65,"description":"Mostly Cloudy"}
 *
 * Keeps only the last CONDHISTORY_MAX lines (default 288 = 24 h at 5 min
 * polls).  Pruning is done in-place after each append so the file never
 * grows unbounded.
 *
 * The file is read directly by the CGI — no locking needed because the
 * daemon is the sole writer and the CGI is a read-only consumer.
 */
#ifndef CONDHISTORY_H
#define CONDHISTORY_H

#include "weather_api.h"

/* Maximum number of data-points kept in the file. */
#define CONDHISTORY_MAX 288   /* 24 h at 5-min poll interval */

/*
 * Append current conditions to the log and prune to CONDHISTORY_MAX lines.
 * Does nothing if snap is NULL or snap->conditions.valid is false.
 */
void condhistory_append(const WeatherSnapshot *snap);

#endif /* CONDHISTORY_H */
