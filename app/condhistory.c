/*
 * condhistory.c — Periodic conditions history log.
 *
 * See condhistory.h for format and design notes.
 */
#include "condhistory.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>
#include <time.h>

#define COND_FILE     "/tmp/weather_acap_cond.jsonl"
#define MAX_FILE_BYTES (512 * 1024)   /* 512 KB safety cap */

/* ── JSON string escaper (scope-local, no link conflict) ────────────────── */
static void cond_esc(const char *in, char *out, size_t outlen) {
    size_t j = 0;
    if (!in) in = "";
    for (size_t i = 0; in[i] && j + 2 < outlen; i++) {
        unsigned char c = (unsigned char)in[i];
        if (c == '"' || c == '\\') {
            if (j + 3 >= outlen) break;
            out[j++] = '\\'; out[j++] = c;
        } else if (c < 0x20) {
            continue;
        } else {
            out[j++] = c;
        }
    }
    out[j] = '\0';
}

/* ── Prune: rewrite file keeping only the last max_lines lines ─────────── */
static void prune_file(const char *path, int max_lines) {
    FILE *f = fopen(path, "r");
    if (!f) return;

    /* Read entire file into a heap buffer (capped at MAX_FILE_BYTES). */
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    rewind(f);

    if (sz <= 0 || sz > MAX_FILE_BYTES) { fclose(f); return; }

    char *buf = (char *)malloc(sz + 1);
    if (!buf) { fclose(f); return; }

    size_t nr = fread(buf, 1, sz, f);
    fclose(f);
    buf[nr] = '\0';

    /* Count newlines to determine line count. */
    int count = 0;
    for (size_t i = 0; i < nr; i++)
        if (buf[i] == '\n') count++;

    if (count <= max_lines) { free(buf); return; }

    /* Skip the oldest (count - max_lines) lines. */
    int skip = count - max_lines;
    char *p = buf;
    for (int i = 0; i < skip && *p; i++) {
        char *nl = strchr(p, '\n');
        if (!nl) break;
        p = nl + 1;
    }

    /* Rewrite from p onwards. */
    FILE *out = fopen(path, "w");
    if (out) {
        fputs(p, out);
        fclose(out);
    }
    free(buf);
}

/* ── Public API ─────────────────────────────────────────────────────────── */

void condhistory_append(const WeatherSnapshot *snap) {
    if (!snap || !snap->conditions.valid) return;

    /* Build timestamp */
    time_t now = time(NULL);
    char   ts[32];
    strftime(ts, sizeof(ts), "%Y-%m-%dT%H:%M:%SZ", gmtime(&now));

    /* Escape description string */
    char e_desc[192];
    cond_esc(snap->conditions.description, e_desc, sizeof(e_desc));

    /* Append JSON line */
    FILE *f = fopen(COND_FILE, "a");
    if (!f) {
        syslog(LOG_WARNING, "condhistory: cannot open %s for append", COND_FILE);
        return;
    }

    fprintf(f,
        "{\"ts\":\"%s\","
        "\"temp_f\":%.1f,"
        "\"wind_mph\":%.1f,"
        "\"humidity_pct\":%d,"
        "\"description\":\"%s\"}\n",
        ts,
        snap->conditions.temp_f,
        snap->conditions.wind_speed_mph,
        snap->conditions.humidity_pct,
        e_desc);
    fclose(f);

    /* Prune to CONDHISTORY_MAX lines */
    prune_file(COND_FILE, CONDHISTORY_MAX);
}
