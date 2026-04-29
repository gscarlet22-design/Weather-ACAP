/*
 * multicam.h — Capture snapshots from additional network cameras on alert.
 *
 * Sprint 8 feature.  On every alert transition (activate or clear) that
 * passes the notification cool-down gate, multicam_capture() fetches a JPEG
 * from each configured remote Axis camera via its VAPIX jpg/image.cgi endpoint
 * and saves the images alongside the primary camera snapshots.
 *
 * Camera list format (MultiCamList parameter):
 *   "host:user:pass:label|host:user:pass:label|..."
 *   host  — IP address or hostname (optionally :port, e.g. 192.168.1.10:8080)
 *   user  — VAPIX username
 *   pass  — VAPIX password
 *   label — human-readable name used in the filename; defaults to the host
 *
 * Filename format: YYYYMMDD_HHMMSS_<label>_<event_type>.jpg
 * e.g.: 20260428_143022_Parking_Lot_Tornado_Warning.jpg
 */

#ifndef MULTICAM_H
#define MULTICAM_H

#define MULTICAM_MAX_CAMS 8

typedef struct {
    char host[128];    /* IP / hostname [: port] */
    char user[64];
    char pass[64];
    char label[64];    /* human-readable label for filenames */
} MultiCamEntry;

typedef struct {
    MultiCamEntry cams[MULTICAM_MAX_CAMS];
    int count;
    int enabled;
} MultiCamConfig;

/*
 * Parse "host:user:pass:label|..." into out.
 * Missing label defaults to the host with dots replaced by underscores.
 * out->enabled is NOT set here — callers must set it from the parameter.
 */
void multicam_parse(const char *camlist, MultiCamConfig *out);

/*
 * Capture one JPEG per configured camera and save to save_dir.
 * Only fires when action is "activated" (on_activate=1) or "cleared"
 * (on_clear=1) according to the passed flags.
 *
 * event_type — NWS alert type / threshold label used in the filename
 * action     — "activated" or "cleared"
 * cfg        — parsed camera list; must not be NULL
 * resolution — VAPIX resolution string, e.g. "1280x720"; NULL → camera default
 * save_dir   — directory to write images (auto-detect if NULL/empty)
 * max_count  — prune limit passed to snapshot_prune(); 0 = unlimited
 * on_activate— capture on "activated" transitions
 * on_clear   — capture on "cleared" transitions
 *
 * Returns the number of images successfully captured.
 */
int multicam_capture(const char *event_type,
                     const char *action,
                     const MultiCamConfig *cfg,
                     const char *resolution,
                     const char *save_dir,
                     int max_count,
                     int on_activate,
                     int on_clear);

#endif /* MULTICAM_H */
