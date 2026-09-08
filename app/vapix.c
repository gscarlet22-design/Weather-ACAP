#include "vapix.h"

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>
#include <unistd.h>

/* ── Input validation ────────────────────────────────────────────────── */

int vapix_valid_host(const char *host) {
    if (!host || !*host) return 0;
    if (strlen(host) > 253) return 0;
    for (const char *p = host; *p; p++) {
        unsigned char c = (unsigned char)*p;
        if (!(isalnum(c) || c == '.' || c == '-' || c == ':' || c == '[' || c == ']'))
            return 0;
    }
    return 1;
}

int vapix_valid_resolution(const char *res) {
    if (!res || !*res || strlen(res) > 11) return 0;
    const char *x = strchr(res, 'x');
    if (!x || x == res || !x[1]) return 0;
    for (const char *p = res; p < x; p++) if (!isdigit((unsigned char)*p)) return 0;
    for (const char *p = x + 1; *p; p++)  if (!isdigit((unsigned char)*p)) return 0;
    return 1;
}

#ifndef CGI_NO_CURL
#include <curl/curl.h>

typedef struct { char *data; size_t size; } Buf;

static size_t write_cb(void *ptr, size_t sz, size_t nmemb, void *ud) {
    Buf *b   = (Buf *)ud;
    size_t n = sz * nmemb;
    char  *p = realloc(b->data, b->size + n + 1);
    if (!p) return 0;
    b->data = p;
    memcpy(b->data + b->size, ptr, n);
    b->size += n;
    b->data[b->size] = '\0';
    return n;
}

static size_t discard_cb(void *ptr, size_t sz, size_t nmemb, void *ud) {
    (void)ptr; (void)ud;
    return sz * nmemb;
}

/* Binary-safe write-to-file callback for JPEG capture. */
static size_t write_to_file_cb(void *ptr, size_t sz, size_t nmemb, void *ud) {
    return fwrite(ptr, sz, nmemb, (FILE *)ud);
}

/* Options every localhost/LAN call shares.
 * NOSIGNAL: the daemon's GLib loop and the FastCGI runtime own signal
 * handling; without it curl's SIGALRM-based timeout never fires and a
 * stuck VAPIX endpoint blocks the caller indefinitely. */
static void set_common(CURL *curl, long timeout_s) {
    curl_easy_setopt(curl, CURLOPT_TIMEOUT,        timeout_s);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 3L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL,       1L);
}

/* userpwd must outlive the setopt call only — curl copies string options. */
static void set_auth(CURL *curl, char *userpwd, size_t len,
                     const char *user, const char *pass) {
    snprintf(userpwd, len, "%s:%s", user ? user : "", pass ? pass : "");
    curl_easy_setopt(curl, CURLOPT_HTTPAUTH, CURLAUTH_DIGEST);
    curl_easy_setopt(curl, CURLOPT_USERPWD,  userpwd);
}

long vapix_port_set(int port, int activate, const char *user, const char *pass) {
    char url[256];
    snprintf(url, sizeof(url),
        "http://localhost/axis-cgi/io/virtualport.cgi"
        "?schemaversion=1&action=%d&port=%d",
        activate ? 11 : 10, port);

    CURL *curl = curl_easy_init();
    if (!curl) return 0;
    char userpwd[256];
    set_auth(curl, userpwd, sizeof(userpwd), user, pass);
    curl_easy_setopt(curl, CURLOPT_URL,           url);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, discard_cb);   /* plain GET, not HEAD */
    set_common(curl, 5L);

    CURLcode rc = curl_easy_perform(curl);
    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
    curl_easy_cleanup(curl);

    if (rc != CURLE_OK) {
        syslog(LOG_WARNING, "vapix: port %d %s curl err: %s",
               port, activate ? "set" : "clear", curl_easy_strerror(rc));
        return 0;
    }
    return http_code;
}

char *vapix_get(const char *path, const char *user, const char *pass,
                long *http_code_out) {
    char url[384];
    snprintf(url, sizeof(url), "http://localhost%s", path);

    CURL *curl = curl_easy_init();
    if (!curl) return NULL;

    Buf buf = { NULL, 0 };
    char userpwd[256];
    set_auth(curl, userpwd, sizeof(userpwd), user, pass);
    curl_easy_setopt(curl, CURLOPT_URL,           url);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA,     &buf);
    set_common(curl, 10L);

    CURLcode rc = curl_easy_perform(curl);
    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
    curl_easy_cleanup(curl);

    if (http_code_out) *http_code_out = http_code;
    if (rc != CURLE_OK) {
        free(buf.data);
        return NULL;
    }
    return buf.data;  /* caller frees; may be NULL */
}

int vapix_probe_virtual_ports(const char *user, const char *pass, int *max_ports) {
    /* Ask for the Input group of properties — the number of available
     * virtual input ports is exposed there on modern AXIS OS.
     * We fall back to a conservative default (32) on older firmware. */
    long code = 0;
    char *body = vapix_get(
        "/axis-cgi/param.cgi?action=list&group=Properties.VirtualInput",
        user, pass, &code);

    int n = 0;

    if (body && code == 200) {
        /* Look for "Properties.VirtualInput.NumberOfPorts=N" */
        const char *k = strstr(body, "NumberOfPorts=");
        if (k) {
            k += strlen("NumberOfPorts=");
            n = atoi(k);
        }
    }
    free(body);

    if (n <= 0) n = 32;                 /* conservative default */
    if (n > 64) n = 64;                 /* platform hard cap */
    if (max_ports) *max_ports = n;
    return 1;
}

int vapix_has_video(const char *user, const char *pass) {
    long code = 0;
    char *body = vapix_get(
        "/axis-cgi/param.cgi?action=list&group=Properties.Image",
        user, pass, &code);
    int yes = (code == 200 && body && strstr(body, "Properties.Image")) ? 1 : 0;
    free(body);
    return yes;
}

char *vapix_device_info(const char *user, const char *pass) {
    long code = 0;
    char *body = vapix_get(
        "/axis-cgi/param.cgi?action=list&group=Brand,Properties.System",
        user, pass, &code);
    if (code == 200) return body;
    free(body);
    return NULL;
}

/* Shared implementation for the local and remote snapshot entry points.
 * `tag` is the syslog prefix ("vapix" / "multicam"). */
static int snapshot_impl(const char *tag,
                         const char *host,
                         const char *path,
                         const char *resolution,
                         const char *user,
                         const char *pass,
                         long *http_code_out) {
    if (http_code_out) *http_code_out = 0;

    if (!vapix_valid_host(host)) {
        syslog(LOG_WARNING, "%s: snapshot: invalid host \"%s\"", tag, host ? host : "");
        return -1;
    }
    if (resolution && *resolution && !vapix_valid_resolution(resolution)) {
        syslog(LOG_WARNING, "%s: snapshot: invalid resolution \"%s\" — using camera default",
               tag, resolution);
        resolution = NULL;
    }

    char url[512];
    if (resolution && *resolution)
        snprintf(url, sizeof(url),
                 "http://%s/axis-cgi/jpg/image.cgi?camera=1&resolution=%s",
                 host, resolution);
    else
        snprintf(url, sizeof(url),
                 "http://%s/axis-cgi/jpg/image.cgi?camera=1", host);

    FILE *f = fopen(path, "wb");
    if (!f) {
        syslog(LOG_WARNING, "%s: snapshot: fopen(%s): %s", tag, path, strerror(errno));
        return -1;
    }

    CURL *curl = curl_easy_init();
    if (!curl) { fclose(f); unlink(path); return -1; }

    int is_local = strcmp(host, "localhost") == 0 || strncmp(host, "127.", 4) == 0;
    char userpwd[256];
    set_auth(curl, userpwd, sizeof(userpwd), user, pass);
    curl_easy_setopt(curl, CURLOPT_URL,           url);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_to_file_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA,     f);
    /* Remote cameras get a tighter budget: a transition can fan out to 8
     * of them sequentially on the single daemon thread. */
    set_common(curl, is_local ? 10L : 8L);

    CURLcode rc = curl_easy_perform(curl);

    /* Read response metadata before freeing the curl handle — the
     * CONTENT_TYPE pointer is owned by the handle and dies with it. */
    char ct[128] = "";
    char *ct_ptr = NULL;
    curl_easy_getinfo(curl, CURLINFO_CONTENT_TYPE, &ct_ptr);
    if (ct_ptr) snprintf(ct, sizeof(ct), "%s", ct_ptr);
    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
    curl_easy_cleanup(curl);

    int close_err = (fclose(f) != 0);

    syslog(LOG_INFO, "%s: snapshot %s HTTP %ld content-type=%s",
           tag, host, http_code, ct[0] ? ct : "(none)");

    /* Reject explicitly if the response is an error document.
     * Do NOT reject on an unknown content-type — some cameras omit the
     * Content-Type header on image responses.  Trust HTTP 200 for those. */
    int is_error_body = ct[0] &&
        (strstr(ct, "text/html")        != NULL ||
         strstr(ct, "application/json") != NULL ||
         strstr(ct, "text/plain")       != NULL);

    if (http_code_out) *http_code_out = http_code;

    if (rc != CURLE_OK) {
        syslog(LOG_WARNING, "%s: snapshot curl error: %s", tag, curl_easy_strerror(rc));
        unlink(path);
        return -1;
    }
    if (http_code != 200) {
        syslog(LOG_WARNING, "%s: snapshot HTTP %ld (expected 200)", tag, http_code);
        unlink(path);
        return -1;
    }
    if (is_error_body) {
        syslog(LOG_WARNING, "%s: snapshot rejected error body (content-type: %s)", tag, ct);
        unlink(path);
        return -1;
    }
    if (close_err) {
        /* Storage full / SD removed mid-write: the file is truncated. */
        syslog(LOG_WARNING, "%s: snapshot fclose(%s): %s", tag, path, strerror(errno));
        unlink(path);
        return -1;
    }
    return 0;
}

int vapix_snapshot_to_file(const char *path,
                           const char *resolution,
                           const char *user,
                           const char *pass,
                           long *http_code_out) {
    return snapshot_impl("vapix", "localhost", path, resolution, user, pass, http_code_out);
}

int vapix_snapshot_to_file_remote(const char *host,
                                  const char *path,
                                  const char *resolution,
                                  const char *user,
                                  const char *pass,
                                  long *http_code_out) {
    return snapshot_impl("multicam", host, path, resolution, user, pass, http_code_out);
}

#else /* CGI_NO_CURL — stub implementations: VAPIX not available without libcurl */

long vapix_port_set(int port, int activate, const char *user, const char *pass) {
    (void)port; (void)activate; (void)user; (void)pass;
    return 0;
}

char *vapix_get(const char *path, const char *user, const char *pass,
                long *http_code_out) {
    (void)path; (void)user; (void)pass;
    if (http_code_out) *http_code_out = 0;
    return NULL;
}

int vapix_probe_virtual_ports(const char *user, const char *pass, int *max_ports) {
    (void)user; (void)pass;
    if (max_ports) *max_ports = 32;
    return 0;
}

int vapix_has_video(const char *user, const char *pass) {
    (void)user; (void)pass;
    return 0;
}

char *vapix_device_info(const char *user, const char *pass) {
    (void)user; (void)pass;
    return NULL;
}

int vapix_snapshot_to_file(const char *path, const char *resolution,
                           const char *user, const char *pass,
                           long *http_code_out) {
    (void)path; (void)resolution; (void)user; (void)pass;
    if (http_code_out) *http_code_out = 0;
    return -1;
}

int vapix_snapshot_to_file_remote(const char *host, const char *path,
                                   const char *resolution,
                                   const char *user, const char *pass,
                                   long *http_code_out) {
    (void)host; (void)path; (void)resolution; (void)user; (void)pass;
    if (http_code_out) *http_code_out = 0;
    return -1;
}

#endif /* CGI_NO_CURL */
