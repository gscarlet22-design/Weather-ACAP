/*
 * multicam.c — Capture snapshots from additional network cameras on alert.
 *
 * Sprint 8.  Iterates a user-supplied list of remote Axis cameras and
 * calls vapix_snapshot_to_file_remote() for each one whenever an alert
 * transition passes the notification cool-down gate.
 *
 * See multicam.h for the MultiCamList parameter format.
 */

#include "multicam.h"
#include "vapix.h"
#include "snapshot.h"      /* snapshot_find_save_dir, snapshot_prune */

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

/* ── Helpers ─────────────────────────────────────────────────────────────── */

/* Replace every non-alphanumeric character with '_', max outlen-1 chars. */
static void sanitize(const char *in, char *out, size_t outlen) {
    size_t i = 0;
    for (const char *p = in; *p && i + 1 < outlen; p++)
        out[i++] = isalnum((unsigned char)*p) ? *p : '_';
    out[i] = '\0';
}

static int ensure_dir(const char *path) {
    struct stat st;
    if (stat(path, &st) == 0)
        return S_ISDIR(st.st_mode) ? 0 : -1;
    if (mkdir(path, 0755) != 0 && errno != EEXIST) {
        syslog(LOG_WARNING, "multicam: mkdir(%s): %s", path, strerror(errno));
        return -1;
    }
    return 0;
}

/* ── Parse ──────────────────────────────────────────────────────────────── */

void multicam_parse(const char *camlist, MultiCamConfig *out) {
    if (!out) return;
    out->count   = 0;
    out->enabled = 0;
    if (!camlist || !*camlist) return;

    char *copy = strdup(camlist);
    if (!copy) return;

    char *save1 = NULL;
    char *entry = strtok_r(copy, "|", &save1);
    while (entry && out->count < MULTICAM_MAX_CAMS) {
        /* Split into host:user:pass:label — at most 4 fields on ':' */
        char *fields[4] = { NULL, NULL, NULL, NULL };
        int   nf = 0;
        char *p  = entry;
        for (int f = 0; f < 4 && p; f++) {
            fields[f] = p;
            nf++;
            if (f < 3) {
                char *colon = strchr(p, ':');
                if (colon) { *colon = '\0'; p = colon + 1; }
                else         p = NULL;
            }
        }

        if (!fields[0] || !*fields[0]) {
            entry = strtok_r(NULL, "|", &save1);
            continue;
        }

        MultiCamEntry *e = &out->cams[out->count];
        snprintf(e->host, sizeof(e->host), "%s", fields[0]);
        snprintf(e->user, sizeof(e->user), "%s", fields[1] ? fields[1] : "root");
        snprintf(e->pass, sizeof(e->pass), "%s", fields[2] ? fields[2] : "");

        if (fields[3] && *fields[3]) {
            snprintf(e->label, sizeof(e->label), "%s", fields[3]);
        } else {
            /* Default label: host with dots/colons → underscores */
            sanitize(e->host, e->label, sizeof(e->label));
        }

        out->count++;
        entry = strtok_r(NULL, "|", &save1);
    }

    free(copy);
}

/* ── Capture ────────────────────────────────────────────────────────────── */

int multicam_capture(const char *event_type,
                     const char *action,
                     const MultiCamConfig *cfg,
                     const char *resolution,
                     const char *save_dir,
                     int max_count,
                     int on_activate,
                     int on_clear) {
    if (!cfg || !cfg->enabled || cfg->count == 0) return 0;

    /* Check whether this action type should trigger a capture */
    if (action) {
        if (strcmp(action, "activated") == 0 && !on_activate) return 0;
        if (strcmp(action, "cleared")   == 0 && !on_clear)    return 0;
    }

    /* Resolve save directory */
    const char *dir = (save_dir && *save_dir) ? save_dir : snapshot_find_save_dir();
    if (ensure_dir(dir) != 0) return 0;

    /* Build timestamp prefix */
    time_t now = time(NULL);
    struct tm *utc = gmtime(&now);
    char ts[20];
    strftime(ts, sizeof(ts), "%Y%m%d_%H%M%S", utc);

    char safe_type[64];
    sanitize(event_type ? event_type : "unknown", safe_type, sizeof(safe_type));

    int captured = 0;

    for (int i = 0; i < cfg->count; i++) {
        const MultiCamEntry *cam = &cfg->cams[i];
        if (!cam->host[0]) continue;

        char safe_label[72];
        sanitize(cam->label, safe_label, sizeof(safe_label));

        char path[512];
        snprintf(path, sizeof(path), "%s/%s_%s_%s.jpg",
                 dir, ts, safe_label, safe_type);

        long http_code = 0;
        int rc = vapix_snapshot_to_file_remote(
                    cam->host, path, resolution,
                    cam->user, cam->pass, &http_code);

        if (rc == 0) {
            syslog(LOG_INFO,
                   "multicam: captured %s from %s (event: %s, action: %s)",
                   path, cam->host,
                   event_type ? event_type : "?",
                   action     ? action     : "?");
            captured++;
        } else {
            syslog(LOG_WARNING,
                   "multicam: capture failed from %s for '%s' action=%s (HTTP %ld)",
                   cam->host,
                   event_type ? event_type : "?",
                   action     ? action     : "?",
                   http_code);
        }
    }

    /* Prune shared directory after all captures */
    if (max_count > 0 && captured > 0)
        snapshot_prune(dir, max_count);

    return captured;
}
