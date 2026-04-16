#include "history.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/stat.h>

#define HISTORY_FILE    "/tmp/weather_acap_history.jsonl"
#define HISTORY_ROTATE  "/tmp/weather_acap_history.jsonl.1"
#define HISTORY_MAX_BYTES 32768   /* ~250 entries before rotation */

/* JSON-string escape — enough for NWS text (no control chars expected). */
static void escape_json(const char *in, char *out, size_t outlen) {
    size_t j = 0;
    if (!in) in = "";
    for (size_t i = 0; in[i] && j + 2 < outlen; i++) {
        unsigned char c = (unsigned char)in[i];
        if (c == '"' || c == '\\') {
            if (j + 3 >= outlen) break;
            out[j++] = '\\';
            out[j++] = c;
        } else if (c < 0x20) {
            /* skip control chars */
            continue;
        } else {
            out[j++] = c;
        }
    }
    out[j] = '\0';
}

void history_append(const char *event, const char *headline, const char *action) {
    /* Rotate if file is too big */
    struct stat st;
    if (stat(HISTORY_FILE, &st) == 0 && st.st_size > HISTORY_MAX_BYTES) {
        rename(HISTORY_FILE, HISTORY_ROTATE);
    }

    FILE *f = fopen(HISTORY_FILE, "a");
    if (!f) return;

    time_t now = time(NULL);
    char   ts[32];
    strftime(ts, sizeof(ts), "%Y-%m-%dT%H:%M:%SZ", gmtime(&now));

    char e_evt[256], e_hdl[512], e_act[32];
    escape_json(event,    e_evt, sizeof(e_evt));
    escape_json(headline, e_hdl, sizeof(e_hdl));
    escape_json(action,   e_act, sizeof(e_act));

    fprintf(f, "{\"ts\":\"%s\",\"event\":\"%s\",\"headline\":\"%s\",\"action\":\"%s\"}\n",
            ts, e_evt, e_hdl, e_act);
    fclose(f);
}
