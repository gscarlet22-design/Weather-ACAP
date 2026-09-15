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
#include <unistd.h>

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
/*
 * Runs only once the file has grown PRUNE_SLACK lines past the limit, so
 * the full rewrite happens every ~30 polls rather than every poll at
 * steady state.  The rewrite goes through a temp file + rename() so the
 * CGI (which reads this file for the dashboard sparklines) never sees a
 * half-written file.
 */
#define PRUNE_SLACK 32

static void prune_file(const char *path, int max_lines) {
    FILE *f = fopen(path, "r");
    if (!f) return;

    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    rewind(f);

    if (sz <= 0) { fclose(f); return; }
    if (sz > MAX_FILE_BYTES) {
        /* Runaway file (only reachable after repeated failed prunes) —
         * previously this early-returned forever, so the cap disabled the
         * very mechanism meant to enforce it.  Start over. */
        fclose(f);
        syslog(LOG_WARNING, "condhistory: %s exceeded %d bytes; resetting",
               path, MAX_FILE_BYTES);
        unlink(path);
        return;
    }

    char *buf = (char *)malloc(sz + 1);
    if (!buf) { fclose(f); return; }

    size_t nr = fread(buf, 1, sz, f);
    fclose(f);
    buf[nr] = '\0';

    int count = 0;
    for (size_t i = 0; i < nr; i++)
        if (buf[i] == '\n') count++;

    if (count <= max_lines + PRUNE_SLACK) { free(buf); return; }

    int skip = count - max_lines;
    char *p = buf;
    for (int i = 0; i < skip && *p; i++) {
        char *nl = strchr(p, '\n');
        if (!nl) break;
        p = nl + 1;
    }

    char tmp[256];
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    FILE *out = fopen(tmp, "w");
    if (!out) { free(buf); return; }
    int ok = (fputs(p, out) >= 0);
    ok = (fclose(out) == 0) && ok;
    if (ok && rename(tmp, path) == 0) {
        /* pruned */
    } else {
        syslog(LOG_WARNING, "condhistory: prune rewrite failed; keeping old file");
        unlink(tmp);
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
