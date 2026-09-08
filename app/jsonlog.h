#ifndef JSONLOG_H
#define JSONLOG_H

#include <syslog.h>

/*
 * jsonlog.h — optional structured JSON syslog output
 *
 * When enabled (JsonLogging param = "yes"), jlog() emits a JSON-structured
 * line to stderr IN ADDITION to the normal syslog() call.  The JSON format
 * is ingestible by Loki, Splunk, Datadog, and other JSON log aggregators:
 *
 *   {"ts":"2026-04-30T06:00:00Z","level":"warning","app":"weather_acap","msg":"..."}
 *
 * When disabled, jlog() behaves exactly like syslog() with no overhead.
 */

/*
 * Call once at startup before the first jlog().
 * enabled  : 1 to activate JSON output, 0 for syslog-only mode
 * app_name : value for the "app" JSON key (e.g. "weather_acap")
 */
void jsonlog_init(int enabled, const char *app_name);

/*
 * Drop-in replacement for syslog().
 * Always calls vsyslog(); also writes a JSON line to stderr when enabled.
 */
void jlog(int priority, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));

#endif /* JSONLOG_H */
