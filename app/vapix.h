#ifndef VAPIX_H
#define VAPIX_H

#include <stddef.h>

/*
 * Localhost VAPIX helpers shared by alerts.c, overlay.c, and config_cgi.c.
 * All requests are made to http://localhost using Digest auth.
 */

/* Set a virtual input port state. Returns HTTP status, or 0 on error. */
long vapix_port_set(int port, int activate, const char *user, const char *pass);

/* GET and return body (caller frees). Returns NULL on failure.
 * If http_code_out is non-NULL, fills in the response code. */
char *vapix_get(const char *path, const char *user, const char *pass,
                long *http_code_out);

/* Probe the device's virtual port capability.
 * Populates *max_ports with the highest port number available.
 * Returns 1 on success, 0 on failure. */
int vapix_probe_virtual_ports(const char *user, const char *pass,
                              int *max_ports);

/* Probe video capability. Returns 1 if device has video, 0 otherwise. */
int vapix_has_video(const char *user, const char *pass);

/* Get device model / firmware (heap-allocated; caller frees).
 * Returns NULL on failure. */
char *vapix_device_info(const char *user, const char *pass);

#endif /* VAPIX_H */
