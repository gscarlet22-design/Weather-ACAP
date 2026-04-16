# Weather Monitor ACAP

A native **ACAP** application for Axis cameras that polls US weather data in real time, then drives **VAPIX virtual input ports** and an **on-video text overlay** so your camera can act as a self-contained weather-alert radio — no server required.

---

## What it does

1. **Fetches weather** from the [National Weather Service API](https://www.weather.gov/documentation/services-web-api) (US) or [Open-Meteo](https://open-meteo.com/) (worldwide fallback).
2. **Activates virtual input ports** when configured alert types (Tornado Warning, Severe Thunderstorm Warning, etc.) are active for your location.
3. **Renders a text overlay** on the live video stream with current conditions and active alerts.
4. **Optionally POSTs a webhook** on alert transitions — ideal for MQTT bridges, Slack, or Axis Camera Station Pro event endpoints.

Your VMS or the camera's own Action Rules can react to the virtual port changes to trigger recordings, audio clips, relay outputs, email, or any other action — exactly like a weather radio built into the camera.

---

## Supported devices

| Device | Events | Overlay | Notes |
|---|---|---|---|
| Axis cameras (ARTPEC-7/8, CV25, etc.) | Yes | Yes | Full feature set |
| Axis cameras without video (thermal, radar) | Yes | No | Overlay auto-disabled |
| Axis speakers / intercoms | Yes | No | Overlay auto-disabled |

**Architectures:**

| `.eap` variant | SoC families | Example devices |
|---|---|---|
| **aarch64** | CV25, ARTPEC-8, ARTPEC-9 | M3086-V, P3265-V, Q6135-LE |
| **armv7hf** | ARTPEC-7 | M3075-V, P3245-V (AXIS OS 11.10+) |

**Minimum firmware:** AXIS OS with Native ACAP SDK support (embedded SDK 3.0).

---

## Quick start

### 1. Install the ACAP

Download the latest `.eap` for your architecture from the [Releases page](https://github.com/gscarlet22-design/Weather-ACAP/releases), then:

1. Open the camera web interface
2. Go to **Apps** (or **System > ACAP**)
3. Click **Add app** and upload the `.eap` file
4. Toggle the app **On**

### 2. Open the configuration UI

Navigate to the app's built-in web page through the camera's app list — click the app name or its **Open** link. The storm-themed interface has six tabs:

| Tab | Purpose |
|---|---|
| **Dashboard** | Live conditions, active alerts, port status, recent history |
| **Location** | ZIP code, lat/lon override, weather provider, poll interval |
| **Alerts & Triggers** | Map NWS alert types to virtual input ports (add, remove, enable/disable, test) |
| **Overlay** | Toggle overlay, set position, customize template with `{temp}`, `{cond}`, `{wind}`, etc. |
| **Diagnostics** | Self-tests (VAPIX, weather, webhook), manual port control, fire drill, device info |
| **Advanced** | System on/off, VAPIX credentials, webhook config, mock mode, config export/import |

### 3. Configure your location

On the **Location** tab, enter your US ZIP code. The app geocodes it to coordinates via the US Census service (no API key needed).

For rural areas where the ZIP centroid is far from the camera, enter exact latitude/longitude overrides — they take precedence over ZIP.

### 4. Set up alert-to-port mappings

On the **Alerts & Triggers** tab, each row maps an NWS alert type to a virtual input port number. Defaults:

| Alert type | Port |
|---|---|
| Tornado Warning | 20 |
| Severe Thunderstorm Warning | 21 |
| Flash Flood Warning | 22 |
| Winter Storm Warning | 23 |
| Ice Storm Warning | 24 |
| Hurricane Warning | 25 |
| Storm Surge Warning | 26 |
| Extreme Wind Warning | 27 |
| Blizzard Warning | 28 |
| Dust Storm Warning | 29 |
| Tsunami Warning | 30 |
| Tropical Storm Warning | 31 |
| Excessive Heat Warning | 32 |
| Fire Weather Watch | 33 |
| Flood Warning | 34 |

You can add, remove, or reorder these. Use **Auto-assign ports** to renumber sequentially. The **Test** buttons on each row let you fire/clear individual ports to verify your Action Rules.

### 5. Wire up Action Rules (camera) or VMS events

#### Option A: Camera Action Rules

In the camera's web interface:

1. **System > Events > Rules > Add a rule**
2. Condition: **I/O > Virtual input** > select the port number (e.g., port 20)
3. Action: whatever you want — play audio, send email, activate relay, record video, etc.

#### Option B: Axis Camera Station Pro

1. **Configuration > Recording and events > Action rules > New**
2. Add trigger: **Device event** > your camera > **Virtual Input** > port number
3. Add action: Record, raise alarm, send email, trigger output, etc.

#### Option C: Any VMS with ONVIF or VAPIX event support

Virtual input port changes are standard VAPIX/ONVIF events. Any VMS that subscribes to Axis device events can trigger on them.

---

## Overlay

When the camera has video, a text overlay is rendered at the configured corner. The template is fully customizable:

**Default template:**
```
{temp}F {cond} | Wind {wind}mph {dir} | Hum {hum}%
```

**Available variables:** `{temp}`, `{temp_f}`, `{cond}`, `{wind}`, `{dir}`, `{hum}`, `{humidity}`, `{provider}`, `{lat}`, `{lon}`, `{time}`, `{alert_type}`

**Alert prefix** (prepended when alerts are active):
```
ALERT: {alert_type} |
```

The overlay tab includes a live preview that updates as you type.

---

## Webhook

When enabled, the app POSTs a JSON payload to a URL of your choice whenever an alert activates or clears:

```json
{
  "timestamp": "2025-06-15T18:30:00Z",
  "event_type": "alert_fire",
  "alert_event": "Tornado Warning",
  "conditions": {
    "temp_f": 91.0,
    "description": "Partly Cloudy",
    "wind_speed_mph": 22.0,
    "humidity_pct": 68
  },
  "location": { "lat": 41.878, "lon": -87.630 },
  "alert_count": 2
}
```

Use this to bridge to Slack, Teams, MQTT, Node-RED, Home Assistant, or any HTTP endpoint.

---

## Diagnostics & troubleshooting

The **Diagnostics** tab provides built-in self-tests:

| Button | What it tests |
|---|---|
| **Test VAPIX** | Connects to localhost VAPIX with saved credentials, reports video capability and port count |
| **Test weather** | Runs a live weather fetch and shows the parsed result |
| **Test webhook** | Sends a sample POST to the configured webhook URL |
| **Fire port / Clear port** | Manually toggle any virtual input port |
| **Fire Drill** | Activates ALL enabled ports at once (for testing Action Rules end-to-end) |
| **Clear all** | Deactivates every mapped port |

Device info, alert history, and system logs are also available on this tab.

For deeper troubleshooting, see **[TROUBLESHOOTING.md](TROUBLESHOOTING.md)**.

---

## Mock mode

Enable **Mock mode** in the Advanced tab to skip real weather fetches and use a fake snapshot with a Tornado Warning active. All VAPIX calls still fire normally. This is ideal for:

- Bench-testing Action Rules before deployment
- Verifying VMS integration without waiting for real weather
- Demonstrating the system to stakeholders

---

## Configuration backup

Use **Download config JSON** / **Upload config JSON** on the Advanced tab to export settings from one camera and import them into another — useful for deploying identical configurations across a fleet.

---

## Building from source

This is a native C ACAP built with the [ACAP Native SDK 1.14](https://github.com/AxisCommunications/acap-native-sdk). No Python, no container runtime on the camera.

### Prerequisites

- Docker (the SDK cross-toolchains run inside Docker — no ARM hardware needed)

### Build

```bash
# Build the .eap for aarch64 (CV25, ARTPEC-8/9)
docker build --build-arg ARCH=aarch64 -t weather-acap-build:aarch64 .
container=$(docker create weather-acap-build:aarch64)
docker cp "$container:/opt/app/." dist/ && docker rm "$container"
# .eap is now in dist/

# Build the .eap for armv7hf (ARTPEC-7)
docker build --build-arg ARCH=armv7hf -t weather-acap-build:armv7hf .
container=$(docker create weather-acap-build:armv7hf)
docker cp "$container:/opt/app/." dist/ && docker rm "$container"
```

### CI/CD

GitHub Actions builds both architectures on every push to `main` and on pull requests. Tagged pushes (`v*.*.*`) also publish a GitHub Release with both `.eap` files attached.

See [`.github/workflows/build.yml`](.github/workflows/build.yml) for the full pipeline.

### Project structure

```
app/
  weather_acap.c        Main daemon (GLib event loop + poll timer)
  config_cgi.c          CGI backend → builds as weather_acap.cgi
  params.c              axparameter wrapper (all config fields)
  weather_api.c         Weather provider abstraction
  nws.c                 NWS API client
  openmeteo.c           Open-Meteo API client
  alerts.c              Alert-to-port mapping + state machine
  overlay.c             Template renderer + VAPIX overlay API
  vapix.c               Shared VAPIX helpers (ports, video detect, device info)
  history.c             Alert history ring buffer (JSONL)
  webhook.c             Outbound webhook POST
  cJSON.c / cJSON.h     JSON parser (MIT, Dave Gamble)
  html/
    index.html          Single-page app shell (6 tabs)
    style.css           Storm-theme dark UI
    app.js              Tab routing, config CRUD, live polling, diagnostics
    weather_acap.cgi    CGI binary (installed here by build)
  manifest.json         ACAP package manifest
  Makefile              Cross-compile targets (used by acap-build)
Dockerfile              Native SDK build container
TROUBLESHOOTING.md      Common issues and VAPIX debug commands
```

---

## License

[MIT](LICENSE)
