/*
 * mqtt.h — Publish weather/alert data to an MQTT broker.
 *
 * Uses libcurl's built-in mqtt:// URL scheme (curl ≥ 7.71.0).
 * Only QoS 0 is supported by the libcurl MQTT client.
 *
 * Broker URL: mqtt://host:1883   (scheme + host:port only, no trailing slash)
 * Topic is a separate field — concatenated as the URL path at publish time.
 */
#ifndef MQTT_H
#define MQTT_H

#include "weather_api.h"

typedef struct {
    int         enabled;
    const char *broker_url;    /* mqtt://host:port (no topic)     */
    const char *topic;         /* e.g. weather/camera/alerts      */
    const char *username;      /* broker username (may be NULL)   */
    const char *password;      /* broker password (may be NULL)   */
    int         on_alert_only; /* 1 = skip routine poll publishes */
    int         retain;        /* MQTT retain flag                */
} MqttConfig;

/*
 * Publish a JSON weather/alert payload to the configured MQTT broker.
 *
 * event_type:  e.g. "alert_activated", "alert_cleared", "poll"
 * alert_event: alert name, or NULL / "" for non-alert events
 *
 * Returns 1 on success, 0 on error.
 */
int mqtt_publish(const MqttConfig *cfg,
                 const WeatherSnapshot *snap,
                 const char *event_type,
                 const char *alert_event);

#endif /* MQTT_H */
