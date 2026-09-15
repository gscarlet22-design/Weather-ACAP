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
        /* Split on ':' into at most 5 fields.  Two layouts are accepted:
         *   host:user:pass:label
         *   host:PORT:user:pass:label   (PORT = all digits)
         * The old parser split "host:8080:root:pw:Lot" as user=8080,
         * pass=root and then wrote the real password into the filename
         * and syslog as the label. */
        char *fields[5] = { NULL, NULL, NULL, NULL, NULL };
        int   nf = 0;
        char *p  = entry;
        for (int f = 0; f < 5 && p; f++) {
            fields[f] = p;
            nf++;
            if (f < 4) {
                char *colon = strchr(p, ':');
                if (colon) { *colon = '\0'; p = colon + 1; }
                else         p = NULL;
            }
        }

        if (!fields[0] || !*fields[0]) {
            entry = strtok_r(NULL, "|", &save1);
            continue;
        }

        int has_port = 0;
        if (nf >= 5 && fields[1] && *fields[1]) {
            has_port = 1;
            for (const char *q = fields[1]; *q; q++)
                if (*q < '0' || *q > '9') { has_port = 0; break; }
        }

        const char *f_host  = fields[0];
        const char *f_port  = has_port ? fields[1] : NULL;
        const char *f_user  = has_port ? fields[2] : fields[1];
        const char *f_pass  = has_port ? fields[3] : fields[2];
        const char *f_label = has_port ? fields[4] : fields[3];
        /* Without a port, a 5th field is the tail of a label that
         * contained ':' — glue it back on. */
        if (!has_port && fields[4]) fields[4][-1] = ':';

        MultiCamEntry *e = &out->cams[out->count];
        if (f_port)
            snprintf(e->host, sizeof(e->host), "%s:%s", f_host, f_port);
        else
            snprintf(e->host, sizeof(e->host), "%s", f_host);
        snprintf(e->user, sizeof(e->user), "%s", f_user ? f_user : "root");
        snprintf(e->pass, sizeof(e->pass), "%s", f_pass ? f_pass : "");

        if (f_label && *f_label) {
            snprintf(e->label, sizeof(e->label), "%s", f_label);
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
