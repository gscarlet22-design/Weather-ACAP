# Weather Monitor ACAP

A native **ACAP** application for Axis cameras that turns any Axis device into a self-contained weather-alert radio — no server required.

The app polls US weather data in real time, then drives **VAPIX virtual input ports**, an **on-video text overlay**, **JPEG snapshots** (including from additional networked cameras), **MQTT publishes**, **email notifications**, **webhooks with Slack/Teams/Discord/Home Assistant templates**, **threshold-based condition alerts**, **SPC lightning-risk alerts**, **native AXIS camera events**, and **C1710/C1720/D4200 hardware outputs** (display, strobe, siren profiles, audio clips) — so your camera can react to the weather just like it reacts to motion.

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
| 8 | **Webhook** | POSTs a JSON payload to any HTTP endpoint on alert transitions (or every poll); custom `{token}` templates with Slack / Discord / Teams / Home Assistant presets |
| 9 | **History & diagnostics** | Alert history log, VAPIX/weather/webhook self-tests, manual port control, fire drill, overlay maintenance, condition-trend sparklines |
| 10 | **Notification cool-down** | Per-type minimum interval between repeat email/MQTT/webhook/snapshot notifications (VAPIX ports always fire immediately) |
| 11 | **Multi-camera snapshots** | Capture JPEG snapshots from up to 8 additional networked Axis cameras simultaneously on alert |
| 12 | **Native AXIS events** | Publishes `Alert` (stateful) and `Conditions` (stateless) events into the camera's native AXIS event system via `axevent` — usable in Action Rules, ACS, and ONVIF subscriptions |
| 13 | **Lightning / thunderstorm risk** | Checks the NOAA SPC Day-1 Convective Outlook and fires a virtual input port (default 35) when the camera's location is inside a risk polygon at or above a chosen category |
| 14 | **Hardware alert output** | Axis C1710/C1720: scrolling display message and strobe (red Warning / amber Watch); Axis D4200: named siren profile; any device with a media-clip library: play an audio clip per tier |
| 15 | **Structured JSON logging** | Optional JSON log lines alongside syslog for Loki / Splunk / Datadog |
| 16 | **Mock mode** | Synthetic Tornado Warning + fixed conditions for bench-testing every integration without waiting for weather (sticky banner while active) |

Your VMS or the camera's own Action Rules can react to port changes to trigger recordings, audio clips, relay outputs, or any other action — exactly like a weather radio built into the camera.

---

## Supported devices

| Device | Events | Overlay | Hardware output | Notes |
|---|---|---|---|---|
| Axis cameras (ARTPEC-7/8/9, CV25, etc.) | ✅ | ✅ | audio clips if a media-clip library exists | Full feature set |
| Axis cameras without video (thermal, radar) | ✅ | — | — | Overlay auto-disabled |
| Axis C1710 / C1720 network speaker-strobe | ✅ | — | display, strobe, audio clips | Overlay auto-disabled |
| Axis D4200 network horn | — | — | remote siren profile, driven from any of the above | Configured as a target, not a host |
| Other Axis speakers / intercoms | ✅ | — | audio clips | Overlay auto-disabled |

**Architectures:**

| `.eap` variant | SoC families | Example devices |
|---|---|---|
| **aarch64** | CV25, ARTPEC-8, ARTPEC-9 | M3086-V, P3265-V, Q6135-LE |
| **armv7hf** | ARTPEC-7 (AXIS OS 11.10+) | M3075-V, P3245-V |

**Minimum firmware:** AXIS OS with Native ACAP SDK support (embedded SDK 3.0+). Overlay requires AXIS OS 11+ (JSON-RPC dynamic overlay API); the app tracks the `identity`/`identifier` key drift between OS minors automatically.

---

## Quick start

### 1. Install the ACAP

Download the latest `.eap` for your architecture from the [Releases page](https://github.com/gscarlet22-design/Weather-ACAP/releases), then:

1. Open the camera web interface
2. Go to **Apps** (or **System > ACAP**)
3. Click **Add app** and upload the `.eap` file
4. Toggle the app **On**

### 2. Open the configuration UI

Navigate to the app's built-in web page through the camera's app list. The storm-themed interface has eight tabs:

| Tab | Purpose |
|---|---|
| **Dashboard** | Live conditions, active alerts, SPC risk, virtual port status, recent history, condition trend sparklines, **Poll now** |
| **Location** | ZIP code, lat/lon override, weather provider, poll interval, NWS User-Agent |
| **Alerts & Triggers** | Map NWS alert types AND numeric thresholds to virtual input ports; notification cool-down; lightning / thunderstorm risk |
| **Overlay** | Toggle overlay, set position, customize the template, live preview, overlay health |
| **Snapshots** | Configure auto-capture on alert, gallery, on-demand capture, auto-delete limit, additional cameras |
| **Hardware Output** | C1710/C1720 display and strobe, D4200 profiles, audio clips (device-populated dropdown) — each with a test button |
| **Diagnostics** | Self-tests, manual port control, fire drill, overlay maintenance, device info, alert history |
| **Advanced** | System on/off, VAPIX credentials, webhook + templates, MQTT, email, native AXIS events, mock mode, JSON logging, backup/restore |

The header shows the running version. Saves take effect within a second (including the poll interval); the status pill turns **NWS unreachable** when the alert feed cannot be fetched — port state is held, not cleared, until it recovers.

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
| Extreme Heat Warning | 33 |
| Red Flag Warning | 34 |
| *SPC Lightning Risk* (see below) | 35 |

Use **Auto-assign ports** to renumber sequentially. The **Fire / Clear** buttons on each row let you test individual ports immediately. Alert names are matched exactly (case-insensitive) against the NWS `event` field — NWS renamed several products in 2025 (`Excessive Heat` → `Extreme Heat`, `Wind Chill` → `Extreme Cold` / `Cold Weather Advisory`); check the [current product list](https://www.weather.gov/documentation/services-web-api) if a rule never fires.

Rules can be edited while alerts are active: a rule that is disabled or deleted mid-alert clears its port through the normal transition path. Every mapped port is forced OFF when the app starts and cleared when it stops or the master switch is turned off.

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

Ports activate when the condition is met and clear automatically when it returns to normal. Rules hold their state on a poll where the provider did not report that quantity (or conditions were unavailable), and there is no hysteresis — a value hovering on the threshold toggles the port each poll, so pick values with margin.

### 5a. Lightning / thunderstorm risk

The **⚡ Lightning & Thunderstorm Risk** card polls the [NOAA SPC Day-1 Convective Outlook](https://www.spc.noaa.gov/products/outlook/) (free GeoJSON, no key) and activates a virtual input port when the camera's coordinates fall inside a risk polygon at or above the chosen category (TSTM · MRGL · SLGT · ENH · MDT · HIGH). SPC updates roughly hourly, so the check runs every N poll cycles (default 6). Transitions appear in history, native events, notifications and hardware output (Watch tier) like any other alert; the current category is shown on the Dashboard and available as the `{lightning}` overlay token.

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

Auto-delete only touches files this app wrote (`YYYYMMDD_HHMMSS_*.jpg`), so a shared SD-card folder is safe.

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
- **SMTP URL:** `smtp://mailhost:587` (STARTTLS) or `smtps://mailhost:465` (implicit TLS)
- **From address / To address(es):** multiple recipients supported — separate with commas
- **SMTP username / password:** optional auth (required for Gmail, Outlook 365, etc.)
- **Also send on alert clear:** optional

**TLS behaviour:** with a username configured, STARTTLS is *required* so credentials never travel in the clear; without one it is attempted opportunistically (plain internal relays keep working). Server certificates are **not** verified, so self-signed internal relays work — do not point it at an untrusted network path.

**Gmail setup:** use `smtps://smtp.gmail.com:465` with your Gmail address as the username and a [Google App Password](https://support.google.com/accounts/answer/185833) as the password (not your Google account password). Use **Send test email** to verify before a real alert.

---

## Webhook

When enabled, POSTs a payload to any URL whenever an alert activates or clears (untick **Only POST for alert transitions** to also post every poll with `event_type: "poll"`). Use to bridge to Slack, Teams, Discord, Node-RED, Home Assistant, or any HTTP endpoint.

**Default payload** (template left blank):

```json
{
  "timestamp": "2026-04-28T14:25:37Z",
  "event_type": "alert_activated",
  "alert": "Tornado Warning",
  "headline": "Tornado Warning issued April 28 at 2:20PM CDT until 3:00PM CDT by NWS",
  "conditions": {
    "temp_f": 91.0,
    "description": "Partly Cloudy",
    "wind_mph": 22.0,
    "wind_dir_deg": 225,
    "humidity_pct": 68,
    "provider": "nws"
  },
  "location": { "lat": 39.0577, "lon": -94.6406 },
  "active_alert_count": 1
}
```

**Custom templates:** the **Payload template** box accepts any text with `{token}` placeholders — `{timestamp}` `{event_type}` `{alert_type}` `{headline}` `{description}` (strings) and `{temp_f}` `{wind_mph}` `{humidity_pct}` `{active_count}` (numbers). Preset buttons fill in ready-made Slack, Discord, Teams and Home Assistant bodies. When the template is JSON (starts with `{` or `[`), string tokens are JSON-escaped so a headline containing quotes cannot break the payload; otherwise it is sent as `text/plain`. Redirects are followed with the POST intact, and 4xx/5xx responses are logged as warnings.

---

## On-video overlay

A text overlay is rendered at the configured corner of the live video stream and updated every poll cycle. The template is fully customizable; a live preview updates in the UI as you type.

**Default template:**
```
Temp: {temp}F | {cond} | Wind: {wind}mph {dir} | Hum: {hum}%
```

**Available variables:** `{temp}` `{temp_f}` `{cond}` `{wind}` `{dir}` `{arrow}` `{hum}` `{humidity}` `{provider}` `{sunrise}` `{sunset}` `{lightning}` `{lat}` `{lon}` `{time}` (camera-local `HH:MM`) `{utc}` (`HH:MM UTC`) — and `{alert_type}` inside the alert prefix.

**Alert prefix** (prepended when alerts are active):
```
[ALERT: {alert_type}]
```

Position options: top-left, top-right, bottom-left, bottom-right. Changing the position re-creates the overlay on the next poll.

**How it stays healthy:** the app creates one runtime text overlay and updates it in place every poll. Its handle is persisted to `/tmp` — the same lifetime as the camera's runtime overlays — so app restarts, crashes and upgrades reuse the existing overlay instead of adding another (versions before 1.1.0 stacked one per restart until the camera's limit was hit). Disabling the overlay, or the master switch, removes it from the video. The Overlay tab shows the live handle, or why the overlay is being skipped (no video channel / VAPIX auth failure / limit reached); **Diagnostics → Remove all text overlays** clears orphans left by older versions.

---

## Notification cool-down

By default, every alert transition (activate or clear) immediately fires all enabled notification channels — email, MQTT, webhook, and snapshots. For slowly-evolving alerts that stay active for hours, this can generate a flood of repeat notifications if the alert briefly expires and re-issues.

**Cool-down settings (Alerts & Triggers tab → Notification Cool-down card):**

| Setting | Default | Effect |
|---------|---------|--------|
| **NWS/threshold alert cool-down** | 10 min | Minimum minutes between repeat notifications for the same NWS alert event |
| **Threshold cool-down** | 10 min | Same for threshold-rule transitions |

Set either value to **0** to disable cool-down for that category.

> **Note:** VAPIX virtual input ports, native AXIS events and hardware outputs are **always** driven immediately regardless of cool-down — the suppression only applies to email, MQTT, webhook, and snapshot channels. A "cleared" notification is always sent when its "activated" was sent, so downstream consumers never get stuck in the active state.

---

## Multi-camera snapshots

When an alert activates (or clears), the app can capture JPEG snapshots from up to **8 additional networked Axis cameras** simultaneously — beyond the local camera already captured by the Snapshots feature.

**Configuration (Snapshots tab → Additional Cameras card):**

1. Enable **Capture from additional cameras**
2. Set the shared **Resolution** for all remote captures (independent from the local camera resolution)
3. Add camera entries: **Host** (IP or hostname, optionally `host:PORT`), **Username**, **Password** (may not contain `:` or `|`), and an optional **Label** shown in the filename

Snapshot filenames include the label for easy identification:
```
20260428_142537_Parking_Lot_Tornado_Warning.jpg
```

Passwords are never echoed back to the browser or included in the config export; a saved password shows as "(unchanged)" and is kept unless you type a new one.

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

## Hardware alert output

The **Hardware Output** tab drives physical signalling devices on every alert activation, classified into two tiers: **Warning** (event name contains "Warning", "Emergency" or "Extreme") and **Watch** (contains "Watch", "Advisory", "Statement" or "Outlook"). Threshold rules and SPC lightning risk always use the Watch tier.

| Channel | Device | What happens |
|---|---|---|
| **Speaker display** | Axis C1710 / C1720 | Scrolls the alert headline across the front-panel display for N seconds; text colour and per-tier background colour are configurable |
| **Strobe light** | Axis C1710 / C1720 | `siren_and_light.cgi` start — Warning: fast red pulse; Watch: slow pulse in the device's nearest colour to amber (the palette is probed from `getCapabilities`, usually `yellow`). Stopped explicitly when the alert clears |
| **D4200 Network Horn** | Axis D4200 (remote) | Starts a named siren/strobe **profile** pre-configured on the D4200 — one profile name per tier |
| **Audio clips** | Any device with a media-clip library | Plays a clip per tier via `mediaclip.cgi`; clips are chosen from a dropdown populated from the device |

Every channel fails silently on devices that lack the API (404/405 is logged and ignored). Each card has a test button that fires that channel at the chosen tier. Hardware outputs are not subject to notification cool-down.

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
| **Remove all text overlays** | Deletes every runtime text overlay on the camera — one-shot cleanup for cameras upgraded from versions that stacked overlays |
| **Poll now** (Dashboard) | Fetches weather immediately instead of waiting for the next interval |

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

Use **Download config JSON** / **Upload config JSON** on the Advanced tab to export settings from one camera and import them into another — for deploying identical configurations across a fleet. All settings are exported except secret fields (VAPIX, SMTP, MQTT, D4200 and multi-camera passwords), which stay as stored on the importing device. Numeric fields outside their allowed range are clamped on import and reported.

Configuration is stored in `/usr/local/packages/weather_acap/localdata/params.json` (file mode 0600) and survives app restarts and camera reboots; it is removed on uninstall.

---

## Structured JSON logging

Enable **Structured JSON Logging** on the Advanced tab to emit one JSON object per daemon log line (`ts`, `level`, `app`, `msg`) on stderr alongside normal syslog, for Loki / Splunk / Datadog ingestion. Module-level lines written directly with `syslog()` (curl transport details) are not duplicated in JSON form.

---

## Building from source

This is a native C ACAP built with the [ACAP Native SDK 1.14](https://github.com/AxisCommunications/acap-native-sdk). Runtime dependencies on the camera are the OS-bundled libcurl, GLib and libfcgi (and axevent for native events). The version string lives in one place, `app/version.h`, and must match `app/manifest.json` and the top of `CHANGELOG.md`.

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

The `.eap` files appear in `dist/`. The Dockerfile stamps the target architecture into the manifest and passes `-a weather_acap.cgi` to `acap-build` — without that flag the FastCGI backend is left out of the package and the web UI returns HTTP 500.

### CI/CD

GitHub Actions compiles both architectures on every push and pull request (`Build Check`, which also fails on compiler warnings) and packages `.eap` artifacts for pushes to `main` (`Build & Package ACAP`). Tagged pushes (`v*.*.*`) publish a GitHub Release with both `.eap` files attached.

See [`.github/workflows/build.yml`](.github/workflows/build.yml) for the full pipeline.

---

## Project structure

```
app/
  weather_acap.c     Main daemon — GLib event loop, poll timer, alert transitions, signals
  config_cgi.c       FastCGI web-UI backend (all CGI endpoints, validation, secret masking)
  params.c           File-backed parameter store (localdata/params.json) — fields + defaults
  version.h          Single source of the version string
  weather_api.c      Weather provider abstraction (NWS / Open-Meteo), station + sun-time caching
  nws.c              NWS API client + alert parser (unit-aware, fetch_ok flag)
  openmeteo.c        Open-Meteo API client
  alerts.c           NWS alert-to-port mapping + identity-keyed transition state machine
  threshold.c/h      Numeric threshold-to-port mapping + transition state machine
  lightning.c/h      SPC Day-1 convective outlook fetch + point-in-polygon
  overlay.c          Template renderer + VAPIX dynamic overlay (JSON-RPC, persisted handle)
  vapix.c            VAPIX helpers: virtual ports, snapshot, device info, input validation
  alertoutput.c/h    Hardware output: C1710/C1720 display + strobe, D4200 profiles, audio clips
  history.c          Alert history ring buffer (JSONL file)
  condhistory.c/h    Condition history ring buffer for dashboard sparklines
  webhook.c          Outbound webhook HTTP POST via libcurl (templates, escaping)
  snapshot.c/h       JPEG capture via VAPIX + auto-delete of app-owned files
  multicam.c/h       Multi-camera snapshot capture (up to 8 remote Axis cameras)
  mqtt.c/h           MQTT publish via libcurl experimental MQTT support
  email.c/h          SMTP email via libcurl (RFC 2822, STARTTLS / SMTPS)
  axisevents.c/h     Native AXIS event publishing via axevent (Alert + Conditions)
  jsonlog.c/h        Optional structured JSON log output
  cJSON.c/h          Bundled minimal JSON parser (parse-only; strict; UTF-8 escapes)
  html/
    index.html       Single-page app shell (8 tabs)
    style.css        Storm-theme dark UI
    app.js           Tab routing, config CRUD, live polling, diagnostics
  manifest.json      ACAP package manifest
  Makefile           Cross-compile targets (used inside acap-build container)
Dockerfile           Native SDK build container (acap-native-sdk:1.14)
```

---

## License

[MIT](LICENSE)
