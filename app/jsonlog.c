#include "jsonlog.h"

#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <time.h>

static int  g_json_enabled = 0;
static char g_app_name[32] = "weather_acap";

void jsonlog_init(int enabled, const char *app_name) {
    g_json_enabled = enabled;
    if (app_name && *app_name)
        snprintf(g_app_name, sizeof(g_app_name), "%s", app_name);
}

/* Map syslog priority to a short level string. */
static const char *priority_to_level(int priority) {
    switch (priority & LOG_PRIMASK) {
        case LOG_EMERG:   return "emerg";
        case LOG_ALERT:   return "alert";
        case LOG_CRIT:    return "critical";
        case LOG_ERR:     return "error";
        case LOG_WARNING: return "warning";
        case LOG_NOTICE:  return "notice";
        case LOG_INFO:    return "info";
        case LOG_DEBUG:   return "debug";
        default:          return "info";
    }
}

/* JSON-escape src into dst (max dstlen bytes, NUL-terminated). */
static void json_esc_str(const char *src, char *dst, size_t dstlen) {
    size_t j = 0;
    if (!src) src = "";
    for (const char *p = src; *p && j + 2 < dstlen; p++) {
        unsigned char c = (unsigned char)*p;
        if (c == '"' || c == '\\') {
            if (j + 3 >= dstlen) break;
            dst[j++] = '\\';
            dst[j++] = c;
        } else if (c == '\n') {
            if (j + 3 >= dstlen) break;
            dst[j++] = '\\'; dst[j++] = 'n';
        } else if (c == '\r') {
            if (j + 3 >= dstlen) break;
            dst[j++] = '\\'; dst[j++] = 'r';
        } else if (c == '\t') {
            if (j + 3 >= dstlen) break;
            dst[j++] = '\\'; dst[j++] = 't';
        } else if (c < 0x20) {
            /* skip other control chars */
        } else {
            dst[j++] = c;
        }
    }
    dst[j] = '\0';
}

void jlog(int priority, const char *fmt, ...) {
    va_list ap;

    /* Always call vsyslog() — normal syslog pipeline is unaffected. */
    va_start(ap, fmt);
    vsyslog(priority, fmt, ap);
    va_end(ap);

    if (!g_json_enabled) return;

    /* Format the message string */
    char msg[1024];
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);

    /* ISO-8601 UTC timestamp */
    char ts[32];
    struct timespec tp;
    clock_gettime(CLOCK_REALTIME, &tp);
    struct tm tm;
    gmtime_r(&tp.tv_sec, &tm);
    strftime(ts, sizeof(ts), "%Y-%m-%dT%H:%M:%SZ", &tm);

    /* JSON-escape the message */
    char emsg[1024];
    json_esc_str(msg, emsg, sizeof(emsg));

    /* Emit JSON line to stderr (captured by the syslog daemon) */
    fprintf(stderr,
            "{\"ts\":\"%s\",\"level\":\"%s\",\"app\":\"%s\",\"msg\":\"%s\"}\n",
            ts,
            priority_to_level(priority),
            g_app_name,
            emsg);
    fflush(stderr);
}
