#include "vapix.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>
#include <unistd.h>

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

/* Binary-safe write-to-file callback for JPEG capture. */
static size_t write_to_file_cb(void *ptr, size_t sz, size_t nmemb, void *ud) {
    return fwrite(ptr, sz, nmemb, (FILE *)ud);
}

static void set_auth(CURL *curl, const char *user, const char *pass) {
    static char userpwd[256];
    snprintf(userpwd, sizeof(userpwd), "%s:%s",
             user ? user : "", pass ? pass : "");
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
    set_auth(curl, user, pass);
    curl_easy_setopt(curl, CURLOPT_URL,     url);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 5L);
    curl_easy_setopt(curl, CURLOPT_NOBODY,  1L);

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
    set_auth(curl, user, pass);
    curl_easy_setopt(curl, CURLOPT_URL,           url);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT,       10L);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA,     &buf);

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

int vapix_snapshot_to_file(const char *path,
                           const char *resolution,
                           const char *user,
                           const char *pass,
                           long *http_code_out) {
    char url[384];
    if (resolution && *resolution)
        snprintf(url, sizeof(url),
                 "http://localhost/axis-cgi/jpg/image.cgi?camera=1&resolution=%s",
                 resolution);
    else
        snprintf(url, sizeof(url),
                 "http://localhost/axis-cgi/jpg/image.cgi?camera=1");

    FILE *f = fopen(path, "wb");
    if (!f) {
        syslog(LOG_WARNING, "vapix: snapshot: fopen(%s): %s", path, strerror(errno));
        return -1;
    }

    CURL *curl = curl_easy_init();
    if (!curl) { fclose(f); unlink(path); return -1; }

    set_auth(curl, user, pass);
    curl_easy_setopt(curl, CURLOPT_URL,           url);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT,       15L);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_to_file_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA,     f);

    CURLcode rc = curl_easy_perform(curl);

    /* Read response metadata before freeing the curl handle */
    char *ct = NULL;
    curl_easy_getinfo(curl, CURLINFO_CONTENT_TYPE, &ct);
    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
    /* Log CT now — ct pointer is owned by curl and invalid after cleanup */
    syslog(LOG_INFO, "vapix: snapshot HTTP %ld content-type=%s",
           http_code, ct ? ct : "(none)");

    /* Reject explicitly if the response is an error document.
     * Do NOT reject on NULL or unknown content-type — some cameras omit
     * the Content-Type header on image responses and CURLINFO_CONTENT_TYPE
     * returns NULL even on a valid JPEG body.  Trust HTTP 200 for those. */
    int is_error_body = ct &&
        (strstr(ct, "text/html")       != NULL ||
         strstr(ct, "application/json") != NULL ||
         strstr(ct, "text/plain")       != NULL);

    curl_easy_cleanup(curl);
    fclose(f);

    if (http_code_out) *http_code_out = http_code;

    if (rc != CURLE_OK) {
        syslog(LOG_WARNING, "vapix: snapshot curl error: %s", curl_easy_strerror(rc));
        unlink(path);
        return -1;
    }
    if (http_code != 200) {
        syslog(LOG_WARNING, "vapix: snapshot HTTP %ld (expected 200)", http_code);
        unlink(path);
        return -1;
    }
    if (is_error_body) {
        syslog(LOG_WARNING,
               "vapix: snapshot rejected error body (content-type: %s)", ct);
        unlink(path);
        return -1;
    }
    return 0;
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

#endif /* CGI_NO_CURL */
