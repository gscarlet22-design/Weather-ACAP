/*
 * snapshot.c — JPEG capture on alert transition
 *
 * Calls VAPIX /axis-cgi/jpg/image.cgi via vapix_snapshot_to_file() and
 * saves the result to the best available local directory.
 *
 * Directory priority (auto-detect, no SnapshotSaveDir configured):
 *   1. /var/spool/storage/SD_DISK/weather_acap/  (SD card — only if writable)
 *   2. /usr/local/packages/weather_acap/localdata/snapshots/  (always writable)
 *
 * The SD card check actually tries to create the subdirectory — stat alone is
 * not enough because the ACAP sandbox user can see the SD card mount point but
 * may not have write access without a manifest storage declaration.
 *
 * Filename format: YYYYMMDD_HHMMSS_<sanitized_alert_type>.jpg
 * e.g.: 20260427_142537_Tornado_Warning.jpg
 */

#include "snapshot.h"
#include "vapix.h"

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

/* ── Save-directory helpers ─────────────────────────────────────────────── */

/* Returns 0 if path exists as a directory or was successfully created. */
static int ensure_dir(const char *path) {
    struct stat st;
    if (stat(path, &st) == 0)
        return S_ISDIR(st.st_mode) ? 0 : -1;
    if (mkdir(path, 0755) != 0 && errno != EEXIST) {
        syslog(LOG_WARNING, "snapshot: mkdir(%s): %s", path, strerror(errno));
        return -1;
    }
    return 0;
}

const char *snapshot_find_save_dir(void) {
    static char dir[256];

    /* Try SD card — only use it if we can actually create our subdirectory.
     * stat() alone isn't sufficient: the ACAP sandbox can see the SD card
     * mount point but write access requires a manifest storage entry.
     * Attempting mkdir() is the only reliable test. */
    struct stat st;
    if (stat("/var/spool/storage/SD_DISK", &st) == 0 && S_ISDIR(st.st_mode)) {
        snprintf(dir, sizeof(dir), "/var/spool/storage/SD_DISK/weather_acap");
        if (ensure_dir(dir) == 0)
            return dir;
        syslog(LOG_INFO,
               "snapshot: SD card not writable — falling back to localdata");
    }

    /* App-owned localdata directory: always writable without extra manifest
     * declarations.  Persistent across reboots (unlike /tmp). */
    snprintf(dir, sizeof(dir),
             "/usr/local/packages/weather_acap/localdata/snapshots");
    return dir;
}

/* Replace every non-alphanumeric character with '_', max 48 output chars. */
static void sanitize_type(const char *in, char *out, size_t outlen) {
    size_t i = 0;
    for (const char *p = in; *p && i + 1 < outlen && i < 48; p++)
        out[i++] = isalnum((unsigned char)*p) ? *p : '_';
    out[i] = '\0';
}

/* ── Capture ────────────────────────────────────────────────────────────── */

int snapshot_capture(const char *event_type,
                     const char *action,
                     const char *user,
                     const char *pass,
                     const SnapshotConfig *cfg,
                     char *saved_path,
                     size_t saved_path_len) {
    if (!cfg || !cfg->enabled)
        return -1;

    /* Check whether this action type should trigger a capture */
    if (action) {
        if (strcmp(action, "activated") == 0 && !cfg->on_activate) return -1;
        if (strcmp(action, "cleared")   == 0 && !cfg->on_clear)    return -1;
    }

    /* Resolve save directory */
    const char *dir = (cfg->save_dir && *cfg->save_dir)
                      ? cfg->save_dir
                      : snapshot_find_save_dir();

    if (ensure_dir(dir) != 0)
        return -1;

    /* Build filename: YYYYMMDD_HHMMSS_<type>.jpg */
    time_t now = time(NULL);
    struct tm *utc = gmtime(&now);
    char ts[20];
    strftime(ts, sizeof(ts), "%Y%m%d_%H%M%S", utc);

    char safe_type[64];
    sanitize_type(event_type ? event_type : "unknown", safe_type, sizeof(safe_type));

    char path[512];
    snprintf(path, sizeof(path), "%s/%s_%s.jpg", dir, ts, safe_type);

    /* Resolution */
    const char *res = (cfg->resolution && *cfg->resolution)
                      ? cfg->resolution : "1280x720";

    /* Fetch and save */
    long http_code = 0;
    int  rc = vapix_snapshot_to_file(path, res, user, pass, &http_code);

    if (rc != 0) {
        syslog(LOG_WARNING,
               "snapshot: capture failed for '%s' action=%s (HTTP %ld)",
               event_type ? event_type : "?",
               action     ? action     : "?",
               http_code);
        return -1;
    }

    syslog(LOG_INFO, "snapshot: saved %s (event: %s, action: %s)",
           path,
           event_type ? event_type : "?",
           action     ? action     : "?");

    if (saved_path && saved_path_len > 0)
        snprintf(saved_path, saved_path_len, "%s", path);

    return 0;
}
