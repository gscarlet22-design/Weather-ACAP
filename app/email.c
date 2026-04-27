/*
 * email.c — SMTP email notifications via libcurl.
 *
 * libcurl handles SMTP (smtp://), SMTPS (smtps://), and STARTTLS
 * (smtp:// with CURLOPT_USE_SSL = CURLUSESSL_TRY).
 *
 * The RFC 2822 message (headers + blank line + body) is stored in a
 * heap buffer and fed to curl via a read callback.  This avoids any
 * mismatch between the envelope RCPT and the "To:" header.
 *
 * Attachment: none — plain text only.
 * Subject format:
 *   "Weather Alert Active: <event>" on activate
 *   "Weather Alert Cleared: <event>" on clear
 *   "Weather ACAP — Test Email" for the test action
 */
#include "email.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>
#include <time.h>

#ifndef CGI_NO_CURL
#include <curl/curl.h>

/* ── Read callback ────────────────────────────────────────────────────── */

typedef struct { const char *data; size_t pos; size_t len; } Reader;

static size_t read_cb(void *ptr, size_t sz, size_t nmemb, void *ud)
{
    Reader *r = (Reader *)ud;
    size_t remain = r->len - r->pos;
    size_t want   = sz * nmemb;
    size_t give   = (remain < want) ? remain : want;
    memcpy(ptr, r->data + r->pos, give);
    r->pos += give;
    return give;
}

/* Discard any SMTP response body (server banners, etc.). */
static size_t discard_cb(void *p, size_t sz, size_t n, void *ud)
{
    (void)p; (void)ud;
    return sz * n;
}

/* ── RFC 2822 date string ─────────────────────────────────────────────── */
/* "Mon, 27 Apr 2026 14:25:37 +0000" */
static void rfc2822_date(char *out, size_t outlen)
{
    static const char *days[]  = { "Sun","Mon","Tue","Wed","Thu","Fri","Sat" };
    static const char *months[]= { "Jan","Feb","Mar","Apr","May","Jun",
                                    "Jul","Aug","Sep","Oct","Nov","Dec" };
    time_t  now = time(NULL);
    struct tm *t = gmtime(&now);
    snprintf(out, outlen, "%s, %02d %s %04d %02d:%02d:%02d +0000",
             days[t->tm_wday], t->tm_mday, months[t->tm_mon],
             t->tm_year + 1900,
             t->tm_hour, t->tm_min, t->tm_sec);
}

/* ── Build RFC 2822 message ───────────────────────────────────────────── */
static char *build_message(const EmailConfig *cfg,
                            const char *event_type,
                            const char *alert_event,
                            const WeatherSnapshot *snap)
{
    /* Subject line */
    char subject[256];
    if (strcmp(event_type, "alert_activated") == 0)
        snprintf(subject, sizeof(subject),
                 "Weather Alert Active: %s",
                 alert_event ? alert_event : "Unknown");
    else if (strcmp(event_type, "alert_cleared") == 0)
        snprintf(subject, sizeof(subject),
                 "Weather Alert Cleared: %s",
                 alert_event ? alert_event : "Unknown");
    else
        snprintf(subject, sizeof(subject), "Weather ACAP \xe2\x80\x94 Test Email");

    /* Body text */
    char body[1024];
    if (snap && snap->conditions.valid) {
        snprintf(body, sizeof(body),
            "Event: %s\r\n"
            "Alert: %s\r\n"
            "\r\n"
            "Current conditions:\r\n"
            "  Temperature : %.1f \xc2\xb0""F\r\n"
            "  Description : %s\r\n"
            "  Wind        : %.0f mph\r\n"
            "  Humidity    : %d%%\r\n"
            "  Provider    : %s\r\n"
            "\r\n"
            "Active alerts: %d\r\n"
            "\r\n"
            "--\r\n"
            "Weather ACAP (Axis ACAP application)\r\n",
            event_type  ? event_type  : "",
            alert_event ? alert_event : "",
            snap->conditions.temp_f,
            snap->conditions.description,
            snap->conditions.wind_speed_mph,
            snap->conditions.humidity_pct,
            snap->conditions.provider,
            snap->alerts.count);
    } else {
        snprintf(body, sizeof(body),
            "Event: %s\r\n"
            "Alert: %s\r\n"
            "\r\n"
            "(No weather data available at send time)\r\n"
            "\r\n"
            "--\r\n"
            "Weather ACAP (Axis ACAP application)\r\n",
            event_type  ? event_type  : "",
            alert_event ? alert_event : "");
    }

    /* Date header */
    char date[64];
    rfc2822_date(date, sizeof(date));

    /* Assemble full RFC 2822 message.
     * Lines MUST be terminated with CRLF.
     * A blank line separates headers from body. */
    size_t msgsz = 512 + strlen(subject) + strlen(body);
    char  *msg   = (char *)malloc(msgsz);
    if (!msg) return NULL;

    snprintf(msg, msgsz,
        "Date: %s\r\n"
        "From: %s\r\n"
        "To: %s\r\n"
        "Subject: %s\r\n"
        "MIME-Version: 1.0\r\n"
        "Content-Type: text/plain; charset=UTF-8\r\n"
        "Content-Transfer-Encoding: 7bit\r\n"
        "\r\n"
        "%s",
        date,
        cfg->from ? cfg->from : "",
        cfg->to   ? cfg->to   : "",
        subject,
        body);

    return msg;
}

#endif /* CGI_NO_CURL */

int email_send(const EmailConfig *cfg,
               const char *event_type,
               const char *alert_event,
               const WeatherSnapshot *snap)
{
    if (!cfg || !cfg->enabled) return -1;
    if (!cfg->smtp_url || !*cfg->smtp_url) {
        syslog(LOG_WARNING, "email: smtp_url not configured");
        return -1;
    }
    if (!cfg->from || !*cfg->from) {
        syslog(LOG_WARNING, "email: from address not configured");
        return -1;
    }
    if (!cfg->to || !*cfg->to) {
        syslog(LOG_WARNING, "email: to address not configured");
        return -1;
    }

#ifdef CGI_NO_CURL
    (void)event_type; (void)alert_event; (void)snap;
    return -1;
#else
    char *msg = build_message(cfg, event_type, alert_event, snap);
    if (!msg) {
        syslog(LOG_WARNING, "email: failed to allocate message buffer");
        return -1;
    }

    CURL *curl = curl_easy_init();
    if (!curl) { free(msg); return -1; }

    /* Recipient list — single address only for now */
    struct curl_slist *rcpts = curl_slist_append(NULL, cfg->to);

    /* SMTP auth */
    if (cfg->username && *cfg->username) {
        char userpwd[256];
        snprintf(userpwd, sizeof(userpwd), "%s:%s",
                 cfg->username,
                 cfg->password ? cfg->password : "");
        curl_easy_setopt(curl, CURLOPT_USERPWD, userpwd);
    }

    Reader reader = { msg, 0, strlen(msg) };

    curl_easy_setopt(curl, CURLOPT_URL,           cfg->smtp_url);
    curl_easy_setopt(curl, CURLOPT_MAIL_FROM,     cfg->from);
    curl_easy_setopt(curl, CURLOPT_MAIL_RCPT,     rcpts);
    curl_easy_setopt(curl, CURLOPT_READFUNCTION,  read_cb);
    curl_easy_setopt(curl, CURLOPT_READDATA,      &reader);
    curl_easy_setopt(curl, CURLOPT_UPLOAD,        1L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT,       20L);
    /* Opportunistic STARTTLS on plain smtp:// connections */
    curl_easy_setopt(curl, CURLOPT_USE_SSL,       (long)CURLUSESSL_TRY);
    /* Don't verify server cert for internal/private SMTP relays */
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION,  discard_cb);

    CURLcode rc = curl_easy_perform(curl);

    curl_slist_free_all(rcpts);
    curl_easy_cleanup(curl);
    free(msg);

    if (rc != CURLE_OK) {
        syslog(LOG_WARNING, "email: send to %s via %s failed: %s",
               cfg->to, cfg->smtp_url, curl_easy_strerror(rc));
        return -1;
    }

    syslog(LOG_INFO, "email: sent to %s via %s (event=%s alert=%s)",
           cfg->to, cfg->smtp_url,
           event_type  ? event_type  : "?",
           alert_event ? alert_event : "");
    return 0;
#endif /* CGI_NO_CURL */
}
