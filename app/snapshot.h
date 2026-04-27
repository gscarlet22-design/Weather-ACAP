#ifndef SNAPSHOT_H
#define SNAPSHOT_H

#include <stddef.h>

/*
 * snapshot — capture JPEG images from the camera on alert transitions.
 *
 * Uses VAPIX /axis-cgi/jpg/image.cgi via localhost Digest auth.
 * Images are saved to SD card when present, else /tmp/weather_acap_snaps/.
 */

typedef struct {
    int         enabled;
    const char *resolution;    /* e.g. "1280x720"; NULL → camera default */
    const char *save_dir;      /* absolute path; "" or NULL → auto-detect */
    int         on_activate;   /* capture when alert transitions → active */
    int         on_clear;      /* capture when alert transitions → cleared */
} SnapshotConfig;

/*
 * Return the best available save directory for snapshots.
 * Checks for a mounted SD card at /var/spool/storage/SD_DISK; if absent
 * falls back to /tmp/weather_acap_snaps (RAM-backed, non-persistent).
 * Returns a pointer to a static buffer — do NOT free.
 */
const char *snapshot_find_save_dir(void);

/*
 * Capture one JPEG via VAPIX and write it to disk.
 *
 *   event_type  — NWS alert type string, e.g. "Tornado Warning"
 *   action      — "activated" or "cleared"
 *   user/pass   — VAPIX Digest credentials
 *   cfg         — snapshot configuration; must not be NULL
 *   saved_path  — if non-NULL, filled with the saved file path
 *   saved_path_len — size of saved_path buffer
 *
 * Returns 0 on success, -1 on error or when cfg says not to capture.
 */
int snapshot_capture(const char *event_type,
                     const char *action,
                     const char *user,
                     const char *pass,
                     const SnapshotConfig *cfg,
                     char *saved_path,
                     size_t saved_path_len);

#endif /* SNAPSHOT_H */
