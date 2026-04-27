/*
 * email.h — Send alert notification emails via SMTP (libcurl).
 *
 * Supports plain SMTP (smtp://host:587) and SMTPS/STARTTLS
 * (smtps://host:465).  Authentication is optional: leave username
 * blank for unauthenticated relays (internal mail servers).
 */
#ifndef EMAIL_H
#define EMAIL_H

#include "weather_api.h"

typedef struct {
    int         enabled;
    const char *smtp_url;   /* smtp://host:587 or smtps://host:465 */
    const char *from;       /* envelope/header From address         */
    const char *to;         /* envelope/header To address           */
    const char *username;   /* SMTP auth user (NULL = no auth)      */
    const char *password;   /* SMTP auth password                   */
    int         on_clear;   /* also send when an alert clears       */
} EmailConfig;

/*
 * Send an alert notification email.
 *
 * event_type:  "alert_activated", "alert_cleared", "email_test", …
 * alert_event: alert name, or NULL / "" for test messages
 *
 * Returns 0 on success, -1 on failure.
 */
int email_send(const EmailConfig *cfg,
               const char *event_type,
               const char *alert_event,
               const WeatherSnapshot *snap);

#endif /* EMAIL_H */
