# Weather ACAP — Troubleshooting Guide

This document covers how to diagnose and fix the most common issues when deploying
Weather ACAP on an Axis device.

---

## Table of Contents

1. [Viewing container logs](#1-viewing-container-logs)
2. [Verifying virtual input port state via cURL](#2-verifying-virtual-input-port-state)
3. [Verifying the video overlay via cURL](#3-verifying-the-video-overlay)
4. [Common VAPIX authentication errors](#4-common-vapix-authentication-errors)
5. [Virtual port out-of-range error (2104)](#5-virtual-port-out-of-range-error-2104)
6. [NWS API errors](#6-nws-api-errors)
7. [Running demo.py against a live device](#7-running-demopy-against-a-live-device)
8. [No overlay visible on camera stream](#8-no-overlay-visible-on-camera-stream)
9. [App starts but ZIP lookup fails](#9-app-starts-but-zip-lookup-fails)
10. [Port mapping reference](#10-port-mapping-reference)

---

## 1. Viewing Container Logs

### Via SSH (most detail)

```bash
# SSH into the Axis device
ssh root@<device-ip>

# Tail logs live (Ctrl+C to stop)
docker logs weather-acap --follow

# Last 100 lines
docker logs weather-acap --tail 100

# Filter for errors only
docker logs weather-acap 2>&1 | grep -E "ERROR|WARNING"

# Filter for VAPIX HTTP calls
docker logs weather-acap 2>&1 | grep "VAPIX"

# Filter for alert activity
docker logs weather-acap 2>&1 | grep -E "ALERT|virtual port"
```

### Via the Axis Web Interface

1. Open `http://<device-ip>` in a browser.
2. Go to **Apps** in the left menu.
3. Click **Weather Monitor ACAP**.
4. Click the **Logs** tab.

> **Note:** The web interface log viewer shows the last ~500 lines.
> For longer history, use SSH.

### Log level

Change the log level in the config UI (`http://<device>:8080`) or set the
`LOG_LEVEL` environment variable in the ACAP manifest before installing:

| Level   | What you see |
|---------|---|
| `ERROR` | Only failures |
| `WARNING` | Failures + degraded states |
| `INFO` | Normal operation (default) |
| `DEBUG` | Every HTTP call, every poll cycle detail |

---

## 2. Verifying Virtual Input Port State

### List all virtual input port states

```bash
# Replace root:password with your credentials
curl -s -u "root:password" --digest \
  "http://<device-ip>/axis-cgi/io/virtualport.cgi?schemaversion=1&action=4" \
  | tr '&' '\n'
```

Expected output when port 20 is active (HIGH):
```
port=20&value=1
port=21&value=0
...
```

### Manually activate port 20 (test without the app running)

```bash
curl -s -u "root:password" --digest \
  "http://<device-ip>/axis-cgi/io/virtualport.cgi?schemaversion=1&action=11&port=20"
```

Expected response: `OK` or an empty `200 OK`.

### Manually deactivate port 20

```bash
curl -s -u "root:password" --digest \
  "http://<device-ip>/axis-cgi/io/virtualport.cgi?schemaversion=1&action=10&port=20"
```

### Confirm a port appears as an Action Rule trigger

1. In the Axis web interface, go to **System → Events → Action rules → Add rule**.
2. In the **Condition** dropdown, look for **Input signal** or **Virtual input**.
3. Select the port number (e.g. Port 20 = Tornado Warning).
4. If Port 20 does not appear, check whether the port number exceeds the
   device's maximum (see [section 5](#5-virtual-port-out-of-range-error-2104)).

---

## 3. Verifying the Video Overlay

### List all registered text overlays

```bash
curl -s -u "root:password" --digest \
  "http://<device-ip>/vapix/overlays/text" \
  | python3 -m json.tool
```

Expected output (abbreviated):
```json
[
  {
    "id": "weather_acap_1",
    "text": "[TORNADO WARNING] Temp: 91°F | Mostly Cloudy | Wind: 22mph SSW",
    "position": "topLeft"
  }
]
```

### Delete a stale overlay manually

If the app was killed uncleanly, the overlay may persist with stale text:

```bash
# Get the overlay ID from the list command above, then:
curl -s -X DELETE -u "root:password" --digest \
  "http://<device-ip>/vapix/overlays/text/<overlay-id>"
```

The app will recreate it on the next poll cycle.

### Overlay not visible in the stream?

- Check the log for `"Overlay created successfully"` or `"Overlay updated"`.
- If logs show `"device has no video capability"` → the device probe detected
  no video. This is correct behaviour on speakers/intercoms.
- Confirm the overlay is enabled in the config UI (port 8080).
- Some cameras require the overlay to be enabled at the channel level in
  **Video → Video streams → Overlay settings** in the Axis web interface.

---

## 4. Common VAPIX Authentication Errors

### HTTP 401 Unauthorized

**Cause:** Incorrect username or password, or the account doesn't have sufficient
permissions.

**Fix:**
1. Verify `VAPIX_USER` and `VAPIX_PASS` environment variables are correct.
2. The Axis account must have **Operator** or **Administrator** role.
3. The `root` account works on most devices, but some hardened deployments
   disable root API access — create a dedicated Operator account if needed.

```bash
# Test credentials directly
curl -v -u "root:yourpassword" --digest \
  "http://<device-ip>/axis-cgi/basicdeviceinfo.cgi?action=list"
```

### HTTP 403 Forbidden

**Cause:** The account exists and authenticated, but lacks permission for the
specific VAPIX resource.

**Fix:** Ensure the account has **Operator** or **Administrator** role in
**System → Users** on the Axis web interface.

### HTTP 401 loops / `digestmod failure`

**Cause:** The device uses an older Digest algorithm (MD5) but the requests
library is not correctly negotiating it.

**Fix:** Ensure `requests>=2.31.0` is installed (already pinned in
`app/requirements.txt`). Rebuild the container image.

### `requests.exceptions.ConnectionError`

**Cause:** The container cannot reach `localhost` (or the configured
`VAPIX_HOST`).

**Checklist:**
- Confirm the container is running with `--network host` (check `manifest.json`
  `runOptions`).
- On macOS/Windows Docker Desktop, `--network host` is not supported. Set
  `VAPIX_HOST` to the device's actual IP address instead of `localhost`.
- Test connectivity from inside the container:
  ```bash
  docker exec -it weather-acap curl -v http://localhost/axis-cgi/ping
  ```

---

## 5. Virtual Port Out-of-Range Error (2104)

**Log message:**
```
ERROR weather_acap.vapix_client: VAPIX error 2104 (Invalid parameter value):
virtual port 20 is out of range for this device.
```

**Cause:** The device has fewer virtual inputs than the configured starting port.
Some older Axis devices support only 4–8 virtual inputs.

**Fix options (choose one):**

1. **Lower `virtual_port_start`** in the config UI to a value within the
   device's range (e.g. 1 instead of 20).

2. **Reduce the number of enabled alert types** so the last port number
   does not exceed the device's maximum.

3. **Check the device maximum** at startup in the logs:
   ```
   INFO device_probe: Device reports 32 virtual input(s) (max port: 32)
   ```
   If this shows a low number, adjust accordingly.

4. **Query the maximum directly:**
   ```bash
   curl -s -u "root:pass" --digest \
     "http://<device-ip>/axis-cgi/param.cgi?action=list&group=Properties.VirtualInput"
   ```
   Look for `root.Properties.VirtualInput.NbrOfVirtualInputs=N`.

---

## 6. NWS API Errors

### `ValueError: ZIP code '...' not found in Census Geocoder`

**Cause:** The ZIP code you entered is not in the US Census database (non-US ZIP,
PO Box-only ZIP, or typo).

**Fix:** Verify the ZIP at `https://geocoding.geo.census.gov`. For locations
outside the Census database, use the **Latitude/Longitude override** fields in
the config UI instead.

### `HTTPError: 404 Client Error` from `api.weather.gov/points`

**Cause:** The coordinates resolved from your ZIP are outside the NWS coverage
area (US territories, offshore, or near a border).

**Fix:** Use the lat/lon override fields and enter coordinates well within the
continental US, Alaska, Hawaii, or a covered US territory.

### `HTTPError: 503 Service Unavailable` from NWS

**Cause:** NWS has periodic maintenance windows, typically lasting a few minutes.

**Fix:** The client has automatic retry with exponential back-off (up to 3
retries). The poll loop will also retry on the next cycle. No action needed
for brief outages.

### NWS returns stale data (observations are hours old)

**Cause:** The nearest observation station may be offline or reporting
infrequently. NWS picks the closest *available* station, which may not be the
closest geographically.

**Fix:** Use the lat/lon override to shift the coordinates closer to a more
active station. You can browse NWS stations at
`https://www.weather.gov/about/mesonet`.

---

## 7. Running demo.py Against a Live Device

`demo.py` exercises the full pipeline using `mock_weather.json` and optionally
fires real VAPIX calls at a device.

```bash
# Install dependencies (run once)
cd "C:\Weather ACAP"
pip install -r app/requirements.txt

# Dry run — no device needed, prints every VAPIX call that WOULD be made
python demo.py --dry-run

# Against a real device with mock weather (safest test)
python demo.py --host 192.168.1.100 --user root --pass yourpassword

# Against a real device with live NWS data
python demo.py --host 192.168.1.100 --user root --pass yourpassword --live --zip 73301

# Show full alert text
python demo.py --dry-run --verbose
```

**What to look for:**
- `Step 3` should report your device model and confirm `has_video=True` (camera)
  or `False` (speaker).
- `Step 5` should show ports being activated with `→ VAPIX GET ...action=11`.
- `Step 6` should show `POST /vapix/overlays/text` with the formatted text.
- All VAPIX calls should return HTTP `200`.

---

## 8. No Overlay Visible on Camera Stream

Work through this checklist in order:

1. **Log check:** `docker logs weather-acap 2>&1 | grep -i overlay`
   - `"Overlay skipped — device has no video capability"` → device probe found
     no video. Is this actually a camera? Check firmware version.
   - `"Overlay disabled in config"` → enable it in the web UI.
   - `"Overlay created successfully"` but nothing visible → see step 4.

2. **VAPIX check:** Confirm the overlay exists on the device:
   ```bash
   curl -s -u "root:pass" --digest http://<device>/vapix/overlays/text
   ```

3. **Stream check:** Some cameras require overlays to be enabled per-stream.
   In the Axis web interface, go to **Video → Video streams**, open stream
   settings, and verify **Text overlay** is enabled.

4. **Restart:** If you previously deleted the overlay manually, the next poll
   cycle (within `poll_interval` seconds) will re-create it.

---

## 9. App Starts But ZIP Lookup Fails

**Log:**
```
WARNING: No ZIP code or coordinate override configured.
Open http://<device>:8080 and set a ZIP code before the first poll.
```

**Fix:** Open the config UI at `http://<device-ip>:8080` and enter a ZIP code.
The next poll cycle will use it automatically — no restart needed.

If the UI is not reachable:
- Confirm port 8080 is not blocked by a firewall between your browser and the device.
- On the device: `docker exec -it weather-acap curl http://localhost:8080/health`
  should return `{"ok": true}`.

---

## 10. Port Mapping Reference

Default port assignments (virtual_port_start = 20):

| Virtual Port | Alert Type (default config) |
|---|---|
| 20 | Tornado Warning |
| 21 | Severe Thunderstorm Warning |
| 22 | Flash Flood Warning |
| 23 | *(4th enabled alert type)* |
| 24 | *(5th enabled alert type)* |
| 25 | *(6th enabled alert type)* |
| 26 | *(7th enabled alert type)* |
| 27 | *(8th enabled alert type)* |

The exact mapping is determined by the order of items checked in the config UI.
The mapping is logged at startup:

```
INFO event_engine: Alert 'Tornado Warning' matched → port 20
INFO event_engine: Alert 'Flash Flood Warning' matched → port 22
```

To verify the current mapping at any time without restarting:

```bash
curl http://<device-ip>:8080/status
```

This returns a JSON snapshot of the last weather poll including which alerts
were active.
