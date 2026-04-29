# Weather Monitor ACAP

A native **ACAP** application for Axis cameras that turns any Axis device into a self-contained weather-alert radio — no server required.

The app polls US weather data in real time, then drives **VAPIX virtual input ports**, an **on-video text overlay**, **JPEG snapshots** (including from additional networked cameras), **MQTT publishes**, **email notifications**, **threshold-based condition alerts**, and **native AXIS camera events** — so your camera can react to the weather just like it reacts to motion.

---

## What it does

| # | Feature | Description |
|---|---|---|
| 1 | **Weather polling** | Fetches from [NWS API](https://www.weather.gov/documentation/services-web-api) (US, free, no key) or [Open-Meteo](https://open-meteo.com/) (worldwide fallback) on a configurable interval |
| 2 | **NWS alert ports** | Activates VAPIX virtual input ports when configured alert types (Tornado Warning, Flash Flood Warning, etc.) are active for your location; clears them when the alert ends |
| 3 | **Threshold alert ports** | Fires virtual input ports when numeric conditions (temperature, wind speed, humidity, wind direction) cross configurable thresholds — catches what the NWS alert system doesn't cover |
| 4 | **On-video overlay** | Renders a fully-templated text overlay via VAPIX on the live video stream; updates every poll cycle |
| 5 | **JPEG snapshots** | Captures a JPEG from the camera via VAPIX whenever an alert activates or clears; auto-deletes oldest files when a count limit is set |
| 6 | **MQTT publishing** | Publishes JSON weather and alert payloads to any MQTT broker on every transition (or every poll) |
| 7 | **Email notifications** | Sends plain-text email via SMTP/SMTPS (STARTTLS auto-negotiated); supports multiple recipients; optional on-clear emails |
| 8 | **Webhook** | POSTs a JSON payload to any HTTP endpoint on alert transitions |
| 9 | **History & diagnostics** | Alert history log, VAPIX/weather/webhook self-tests, manual port control, fire drill |
| 10 | **Notification cool-down** | Per-type minimum interval between repeat email/MQTT/webhook/snapshot notifications (VAPIX ports always fire immediately) |
| 11 | **Multi-camera snapshots** | Capture JPEG snapshots from up to 8 additional networked Axis cameras simultaneously on alert |
| 12 | **Native AXIS events** | Publishes `Alert` (stateful) and `Conditions` (stateless) events into the camera's native AXIS event system via `axevent` — usable in Action Rules, ACS, and ONVIF subscriptions |

Your VMS or the camera's own Action Rules can react to port changes to trigger recordings, audio clips, relay outputs, or any other action — exactly like a weather radio built into the camera.

---

## Supported devices

| Device | Events | Overlay | Notes |
|---|---|---|---|
| Axis cameras (ARTPEC-7/8, CV25, etc.) | ✅ | ✅ | Full feature set |
| Axis cameras without video (thermal, radar) | ✅ | — | Overlay auto-disabled |
| Axis speakers / intercoms | ✅ | — | Overlay auto-disabled |

**Architectures:**

| `.eap` variant | SoC families | Example devices |
|---|---|---|
| **aarch64** | CV25, ARTPEC-8, ARTPEC-9 | M3086-V, P3265-V, Q6135-LE |
| **armv7hf** | ARTPEC-7 | M3075-V, P3245-V |

**Minimum firmware:** AXIS OS with Native ACAP SDK support (embedded SDK 3.0+). Overlay requires AXIS OS 11+ (JSON-RPC dynamic overlay API).

---

## Quick start

### 1. Install the ACAP

Download the latest `.eap` for your architecture from the [Releases page](https://github.com/gscarlet22-design/Weather-ACAP/releases), then:

1. Open the camera web interface
2. Go to **Apps** (or **System > ACAP**)
3. Click **Add app** and upload the `.eap` file
4. Toggle the app **On**

### 2. Open the configuration UI

Navigate to the app's built-in web page through the camera's app list. The storm-themed interface has seven tabs:

| Tab | Purpose |
|---|---|
| **Dashboard** | Live conditions, active alerts, virtual port status, recent history |
| **Location** | ZIP code, lat/lon override, weather provider, poll interval |
| **Alerts & Triggers** | Map NWS alert types AND numeric thresholds to virtual input ports; notification cool-down settings |
| **Overlay** | Toggle overlay, set position, customize the template |
| **Snapshots** | Configure auto-capture on alert, gallery, on-demand capture, auto-delete limit, additional cameras |
| **Diagnostics** | Self-tests, manual port control, fire drill, device info |
| **Advanced** | System on/off, VAPIX credentials, webhook, MQTT, email, native AXIS events, mock mode, backup/restore |

### 3. Configure your location

On the **Location** tab, enter your US ZIP code. Geocoding uses the US Census service — no API key required.

For rural areas where the ZIP centroid is far from the camera, enter exact latitude/longitude overrides (they take precedence over ZIP).

### 4. Set up NWS alert-to-port mappings

On the **Alerts & Triggers** tab, map NWS alert types to virtual input ports. Default mappings:

| Alert type | Port |
|---|---|
| Tornado Warning | 20 |
| Severe Thunderstorm Warning | 21 |
| Flash Flood Warning | 22 |
| Tornado Watch | 23 |
| Severe Thunderstorm Watch | 24 |
| Flash Flood Watch | 25 |
| Flood Warning | 26 |
| Winter Storm Warning | 27 |
| Blizzard Warning | 28 |
| Ice Storm Warning | 29 |
| High Wind Warning | 30 |
| Hurricane Warning | 31 |
| Tropical Storm Warning | 32 |
| Excessive Heat Warning | 33 |
| Red Flag Warning | 34 |

Use **Auto-assign ports** to renumber sequentially. The **Fire / Clear** buttons on each row let you test individual ports immediately.

### 5. Set up threshold-to-port mappings

Below the NWS alert table is the **Threshold → Port Mapping** table. Each row fires a virtual input port when a numeric weather condition crosses a value:

| Condition | Description | Operators |
|---|---|---|
| `TempF` | Air temperature in °F | `>` `<` `>=` `<=` |
| `WindMph` | Wind speed in mph | `>` `<` `>=` `<=` |
| `HumidityPct` | Relative humidity % | `>` `<` `>=` `<=` |
| `WindDirDeg` | Wind direction 0–360° | `>` `<` `>=` `<=` |

Example rules:
- `TempF > 95 → Port 10` — heat alert when temperature exceeds 95 °F
- `TempF < 32 → Port 11` — freeze alert when temperature drops below freezing
- `WindMph > 40 → Port 12` — high-wind alert above 40 mph
- `HumidityPct > 90 → Port 13` — high-humidity alert

Ports activate when the condition is met and clear automatically when it returns to normal.

### 6. Wire up Action Rules or VMS events

#### Camera Action Rules (VAPIX)

1. **System > Events > Rules > Add a rule**
2. Condition: **I/O > Virtual input** → select the port number
3. Action: play audio, record video, activate relay, send email, etc.

#### Axis Camera Station Pro

1. **Configuration > Recording and events > Action rules > New**
2. Trigger: **Device event** → your camera → **Virtual Input** → port number
3. Action: record, raise alarm, send email, trigger output, etc.

#### Any ONVIF/VAPIX-capable VMS

Virtual input port changes are standard VAPIX events. Any VMS that subscribes to Axis device events can trigger on them.

---

## Snapshots

When snapshot capture is enabled, the app saves a JPEG image via VAPIX each time an alert activates (and optionally clears). Images are stored on the SD card when present, otherwise in the app's persistent `localdata` directory.

**Settings (Snapshots tab):**
- **Resolution:** 1080p / 720p / VGA / QVGA
- **Save directory:** auto-detect SD card, or enter a custom path
- **Capture when:** alert activates, alert clears, or both
- **Max snapshots to keep:** automatically deletes the oldest `.jpg` files after each new capture (default 50; set to 0 for unlimited)

**Gallery:** the Snapshots tab shows a thumbnail gallery of all saved images with timestamps. Use **📷 Capture now** for an immediate on-demand capture without waiting for an alert. Use **Test & diagnose** for step-by-step troubleshooting output (auth failure vs. directory error vs. connectivity).

---

## MQTT

When enabled, the app publishes a JSON payload to your MQTT broker on every alert transition (or on every poll if "on alert only" is disabled). All threshold-triggered events publish identically to NWS alert events.

**Configuration (Advanced tab):**
- **Broker URL:** `mqtt://host:1883` — scheme + host + port, no trailing slash
- **Topic:** e.g. `weather/camera/alerts`
- **Username / Password:** optional broker authentication
- **Publish only on alert transitions:** skip routine poll publishes
- **Retain flag:** set the MQTT retain bit on published messages

**Payload:**
```json
{
  "timestamp": "2026-04-28T14:25:37Z",
  "event_type": "alert_activated",
  "alert": "Tornado Warning",
  "conditions": {
    "temp_f": 82.0,
    "description": "Mostly Cloudy",
    "wind_mph": 22.0,
    "humidity_pct": 68,
    "provider": "nws"
  },
  "active_alert_count": 1
}
```

> **Note:** libcurl MQTT support is experimental and must be compiled into the device's libcurl. AXIS OS 12+ typically includes it. If unsupported, the app logs a clear warning and falls back gracefully — no crash, no hang. Use the **Send test publish** button to verify.

---

## Email

When enabled, the app sends a plain-text email on alert activation (and optionally on clear). All threshold-triggered events send email identically to NWS alert events.

**Configuration (Advanced tab):**
- **SMTP URL:** `smtp://mailhost:587` (STARTTLS auto-negotiated) or `smtps://mailhost:465` (implicit TLS)
- **From address / To address(es):** multiple recipients supported — separate with commas
- **SMTP username / password:** optional auth (required for Gmail, Outlook 365, etc.)
- **Also send on alert clear:** optional

**Gmail setup:** use `smtps://smtp.gmail.com:465` with your Gmail address as the username and a [Google App Password](https://support.google.com/accounts/answer/185833) as the password (not your Google account password). Use **Send test email** to verify before a real alert.

---

## Webhook

When enabled, POSTs a JSON payload to any URL whenever an alert activates or clears. Use to bridge to Slack, Teams, Node-RED, Home Assistant, or any HTTP endpoint.

```json
{
  "timestamp": "2026-04-28T14:25:37Z",
  "event_type": "alert_activated",
  "alert_event": "Tornado Warning",
  "conditions": {
    "temp_f": 91.0,
    "description": "Partly Cloudy",
    "wind_speed_mph": 22.0,
    "humidity_pct": 68,
    "provider": "nws"
  },
  "active_alert_count": 1
}
```

---

## On-video overlay

A text overlay is rendered at the configured corner of the live video stream and updated every poll cycle. The template is fully customizable; a live preview updates in the UI as you type.

**Default template:**
```
Temp: {temp}F | {cond} | Wind: {wind}mph {dir} | Hum: {hum}%
```

**Available variables:** `{temp}` `{temp_f}` `{cond}` `{wind}` `{dir}` `{hum}` `{humidity}` `{provider}` `{lat}` `{lon}` `{time}` `{alert_type}`

**Alert prefix** (prepended when alerts are active):
```
[ALERT: {alert_type}]
```

Position options: top-left, top-right, bottom-left, bottom-right.

---

## Notification cool-down

By default, every alert transition (activate or clear) immediately fires all enabled notification channels — email, MQTT, webhook, and snapshots. For slowly-evolving alerts that stay active for hours, this can generate a flood of repeat notifications if the alert briefly expires and re-issues.

**Cool-down settings (Alerts & Triggers tab → Notification Cool-down card):**

| Setting | Default | Effect |
|---------|---------|--------|
| **NWS/threshold alert cool-down** | 10 min | Minimum minutes between repeat notifications for the same NWS alert event |
| **Threshold cool-down** | 10 min | Same for threshold-rule transitions |

Set either value to **0** to disable cool-down for that category.

> **Note:** VAPIX virtual output ports are **always** fired immediately regardless of cool-down — the suppression only applies to email, MQTT, webhook, and snapshot channels. This preserves the integrity of VMS/Action Rule integrations.

---

## Multi-camera snapshots

When an alert activates (or clears), the app can capture JPEG snapshots from up to **8 additional networked Axis cameras** simultaneously — beyond the local camera already captured by the Snapshots feature.

**Configuration (Snapshots tab → Additional Cameras card):**

1. Enable **Capture from additional cameras**
2. Set the shared **Resolution** for all remote captures (independent from the local camera resolution)
3. Add camera entries: **Host** (IP or hostname), **Username**, **Password**, and an optional **Label** shown in the filename

Snapshot filenames include the label for easy identification:
```
alert_Tornado_Warning_20260428T142537Z_cam_192_168_1_101.jpg
```

All remote snapshots are saved to the same directory as local snapshots and count toward the **Max snapshots to keep** limit. The **Test** button on each row verifies connectivity and saves a test image before a real alert.

> Remote cameras are accessed via VAPIX (`/axis-cgi/jpg/image.cgi`) over HTTP. Ensure network connectivity and that the camera credentials have viewer access.

---

## Native AXIS events

In addition to VAPIX virtual input ports, the app publishes two native event types into the camera's AXIS event system via the `axevent` library. These events appear alongside built-in camera events and are usable in:
- Camera **Action Rules** (System > Events)
- **Axis Camera Station** / ACS Pro action rules
- Any **ONVIF** event subscription client

### Event types

| Event | Type | Keys | When fired |
|-------|------|------|------------|
| **WeatherAlert** | Stateful (property) | `active` (bool), `alert_type` (string), `action` (string) | Every NWS or threshold alert activation and clearance |
| **WeatherConditions** | Stateless (notification) | `temp_f` (double), `wind_mph` (double), `humidity_pct` (int), `description` (string) | Every poll cycle |

The **WeatherAlert** event is stateful — its `active` property is set to `true` on activation and `false` on clearance, so Action Rules can trigger on both transitions independently. **WeatherConditions** is stateless and fires repeatedly; use it with numeric condition filters in Action Rules.

> **Note:** Native events fire on every state change and are **not** subject to the notification cool-down. Toggle them off in **Advanced > Native AXIS Events** if needed.

**Build requirement:** the `axevent` library (`axsdk/axevent.h`) must be present at compile time to enable this feature. When the library is absent, the daemon compiles cleanly with no-op stubs. Both pre-built `.eap` releases are compiled with axevent support.

---

## Diagnostics & troubleshooting

**Self-tests (Diagnostics tab):**

| Button | What it checks |
|---|---|
| **Test VAPIX** | Connects to localhost VAPIX; reports video capability and port count |
| **Test weather** | Runs a live weather fetch and shows the parsed result |
| **Test webhook** | Sends a sample POST to the configured webhook URL |
| **Fire port / Clear port** | Manually toggle any virtual input port by number |
| **Fire Drill** | Activates ALL enabled ports at once (for end-to-end Action Rule testing) |
| **Clear all** | Deactivates every mapped port |

Per-row **Fire / Clear** buttons also appear on every alert and threshold row in the Alerts & Triggers tab.

For deeper troubleshooting see [TROUBLESHOOTING.md](TROUBLESHOOTING.md).

---

## Mock mode

Enable **Mock mode** in the Advanced tab to skip real weather fetches and inject a fake snapshot: 72 °F, Mostly Cloudy, Tornado Warning active. All VAPIX calls, MQTT publishes, email sends, snapshots, and threshold evaluations still execute normally. Use this for:

- Bench-testing Action Rules before deployment
- Verifying VMS integration without waiting for real weather
- Demonstrating the system in controlled conditions

---

## Configuration backup & fleet deployment

Use **Download config JSON** / **Upload config JSON** on the Advanced tab to export settings from one camera and import them into another — for deploying identical configurations across a fleet. All settings are exported except secret fields (VAPIX password, SMTP password, MQTT password).

---

## Building from source

This is a native C ACAP built with the [ACAP Native SDK 1.14](https://github.com/AxisCommunications/acap-native-sdk). No runtime dependencies on the camera beyond OS-bundled libcurl and GLib.

### Prerequisites

- Docker (the SDK cross-toolchains run inside Docker — no ARM hardware needed on your build machine)

### Build

```bash
# aarch64 (CV25, ARTPEC-8/9)
docker build --build-arg ARCH=aarch64 -t weather-acap-build:aarch64 .
id=$(docker create weather-acap-build:aarch64)
docker cp "$id:/opt/app/." dist/ && docker rm "$id"

# armv7hf (ARTPEC-7)
docker build --build-arg ARCH=armv7hf -t weather-acap-build:armv7hf .
id=$(docker create weather-acap-build:armv7hf)
docker cp "$id:/opt/app/." dist/ && docker rm "$id"
```

The `.eap` files appear in `dist/`.

### CI/CD

GitHub Actions builds both architectures on every push and pull request. Tagged pushes (`v*.*.*`) publish a GitHub Release with both `.eap` files attached.

See [`.github/workflows/build.yml`](.github/workflows/build.yml) for the full pipeline.

---

## Project structure

```
app/
  weather_acap.c     Main daemon — GLib event loop, poll timer, alert transitions
  config_cgi.c       FastCGI web-UI backend (all CGI endpoints)
  params.c           axparameter wrapper — all config fields + defaults
  weather_api.c      Weather provider abstraction (NWS / Open-Meteo)
  nws.c              NWS API client + alert parser
  openmeteo.c        Open-Meteo API client
  alerts.c           NWS alert-to-port mapping + transition state machine
  threshold.c/h      Numeric threshold-to-port mapping + transition state machine
  overlay.c          Template renderer + VAPIX dynamic overlay (JSON-RPC)
  vapix.c            VAPIX helpers: virtual port control, snapshot, device info
  history.c          Alert history ring buffer (JSONL file)
  webhook.c          Outbound webhook HTTP POST via libcurl
  snapshot.c/h       JPEG capture via VAPIX + auto-delete (snapshot_prune)
  mqtt.c/h           MQTT publish via libcurl experimental MQTT support
  email.c/h          SMTP email via libcurl (RFC 2822, STARTTLS / SMTPS)
  multicam.c/h       Multi-camera snapshot capture (up to 8 remote Axis cameras)
  axisevents.c/h     Native AXIS event publishing via axevent (Alert + Conditions)
  condhistory.c/h    Condition history ring buffer for dashboard sparklines
  cJSON.c/h          Bundled JSON parser (MIT — Dave Gamble)
  html/
    index.html       Single-page app shell (7 tabs)
    style.css        Storm-theme dark UI
    app.js           Tab routing, config CRUD, live polling, diagnostics
  manifest.json      ACAP package manifest
  Makefile           Cross-compile targets (used inside acap-build container)
  CMakeLists.txt     CMake build definition
Dockerfile           Native SDK build container (acap-native-sdk:1.14)
```

---

## License

[MIT](LICENSE)
