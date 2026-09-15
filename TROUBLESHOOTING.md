# Troubleshooting — Weather Monitor ACAP

This guide covers the most common issues when installing, configuring, or running the Weather Monitor ACAP. The version you are running is shown in the web UI header.

---

## 1. App won't start or immediately stops

**Check the system log:** in the camera web interface, go to **System > Logs > System log** and look for entries containing `weather_acap`.

| Log message | Cause | Fix |
|---|---|---|
| `params init failed` | The app's `localdata/` directory is not writable | Uninstall and reinstall the ACAP |
| `no FCGI_SOCKET_NAME in env` | The runtime did not wire the FastCGI socket | Check that `manifest.json` declares `httpConfig` type `fastCgi`; reinstall |
| `FastCGI backend died 10 times — giving up` | The web backend is crash-looping | Restart the app; if it repeats, capture the lines just before it and open an issue |
| No log entries at all | App binary didn't launch | Check architecture — the `.eap` must match your device (aarch64 for CV25/ARTPEC-8/9, armv7hf for ARTPEC-7) |

**Via SSH (if available):**

```bash
ssh root@<camera-ip>
journalctl -u weather_acap --no-pager -n 100
```

---

## 2. Web UI returns HTTP 500 or 503

- **503** — the FastCGI backend is not running. The daemon spawns it at startup and respawns it if it dies (up to 10 times). Restart the app; check the log for `spawned FastCGI child`.
- **500** — the package was built without the CGI binary. Only affects self-built packages: build with `acap-build . -a weather_acap.cgi` (the Dockerfile and CI do this). The release `.eap` files are correct.
- **"Daemon config not available yet"** toast on load, or saves refused — the daemon has not yet written `/tmp/weather_acap_config.json`. Wait a few seconds and reload. If it persists, see §1.

---

## 3. VAPIX connection failures

The app calls VAPIX on `localhost` to control virtual ports and overlays. If the Diagnostics tab's **Test VAPIX** button fails:

### "HTTP 0" or "Connection refused"

The app can't reach the camera's internal web server. Rare on native ACAPs. Verify the camera's own web interface loads.

### "HTTP 401" — Authentication failed

VAPIX uses Digest authentication. On the **Advanced** tab, re-enter the VAPIX password (the field shows "(unchanged)" and does not re-send the stored value; type it again to change it). The user must have **Operator** or **Administrator** role.

The overlay logs `VAPIX auth failed (HTTP 401)` every poll while credentials are wrong and recovers automatically once they are fixed.

### "HTTP 404" — Endpoint not found

```bash
# Virtual port support:
curl --digest -u root:pass \
  "http://<camera-ip>/axis-cgi/param.cgi?action=list&group=Properties.VirtualInput"
```

If this returns 404 your device may not support virtual inputs.

---

## 4. Virtual input ports not working

### Ports don't appear in Action Rules

1. On the **Diagnostics** tab use **Fire port** with the port number
2. "Port N activated" means the VAPIX call succeeded
3. Refresh the camera's Events page — the virtual input should now appear

### A port is stuck ON

- Every mapped port is forced OFF when the app starts, and cleared when the app stops, when the master switch is turned off, and when a rule is deleted or disabled. If a port is still ON, the daemon believes the alert is still active — check the Dashboard.
- If the NWS alert feed is unreachable the status pill shows **NWS unreachable** and port state is deliberately *held* (an unreachable feed is not evidence the alert ended). Ports clear on the next successful fetch.
- **Clear all ports** on the Diagnostics tab forces every mapped port OFF; the next poll re-fires anything genuinely active.

### Port number out of range

The Dashboard shows "N virtual ports available". Port numbers above that fail. Use **Auto-assign ports** on the Alerts tab or lower the numbers.

---

## 5. Weather data not loading

### "weather fetch failed" in status

1. **Diagnostics > Test weather fetch** shows the detailed error
2. Common causes: no ZIP code set; NWS API down (auto mode falls back to Open-Meteo for conditions — alerts are NWS-only); DNS/internet not reachable from the camera

### "conditions unavailable" but alerts work

The nearest NWS observation station returned no temperature (common for automated stations). In **auto** mode the app falls back to Open-Meteo; in **NWS only** mode the threshold rules hold their state until the station reports again.

### Wind speed looks wrong

Versions before 1.1.0 over-reported NWS wind speed by 3.6× (unit mix-up). Threshold rules on `WindMph` written against the old values will need adjusting.

### NWS rate limiting

Set the NWS User-Agent on the **Location** tab to include an email address, e.g. `WeatherACAP/1.1.0 (yourname@company.com)`. NWS recommends polling no more often than every 5 minutes.

---

## 6. Overlay not showing on video

### Device has no video

Speakers, intercoms, and radar devices won't get overlays. The Overlay tab hint says "device reported no video".

### Overlay is enabled but nothing appears

1. **Diagnostics > Test VAPIX** — check that "Video: yes" appears
2. **Overlay** tab — the toggle is on, the template isn't empty, the **Live preview** shows text
3. Look in the configured corner of the live view
4. Check the Overlay tab hint: it shows the live overlay handle when the overlay exists on the camera

### "Overlay limit reached" on the Overlay tab

The camera allows a fixed number of runtime text overlays per channel. Versions before 1.1.0 created a new one on every app restart until the limit was hit, after which the overlay silently stopped appearing. These orphaned overlays are **not** visible in the camera's Settings > Overlays page and survive app restarts.

Fix, once: **Diagnostics > Overlay maintenance > Remove all text overlays**. The daemon re-creates its own overlay immediately. (A camera reboot also clears them.)

### Overlay text is stale

The overlay updates every poll cycle. Use **Poll now** on the Dashboard to refresh immediately. The `{time}` token is camera-local time at the last poll.

### Changing the position has no effect

Fixed in 1.1.0 — the overlay is re-created at the new position on the next poll.

### Technical notes (AXIS OS 11+/12)

- The overlay API is JSON-RPC `POST /axis-cgi/dynamicoverlay/dynamicoverlay.cgi`. The legacy `?action=addtext` GET form returns HTTP 200 with `{"error":{"code":200,...}}` and draws nothing.
- OS 12.9+ names the handle `identity` (older docs: `identifier`). Sending the wrong key returns error 103 "Unknown parameter supplied". The app follows whichever key `addText` echoed back.
- Error 300 = per-channel overlay limit reached (see above).

```bash
# List runtime overlays on camera 1:
curl --digest -u root:pass -H 'Content-Type: application/json' \
  -d '{"apiVersion":"1.0","method":"list","params":{"camera":1}}' \
  "http://<camera-ip>/axis-cgi/dynamicoverlay/dynamicoverlay.cgi"
```

---

## 7. Webhook not firing

1. **Advanced** tab — webhook enabled and URL correct
2. **Diagnostics > Test webhook** sends a sample payload and shows the HTTP response
3. Common issues: HTTP 0 — DNS or connection failure; 401/403 — the endpoint needs auth; 4xx/5xx are logged as warnings in the system log with the URL

### Custom template returns 400 from Slack/Teams/Discord

Tokens inside JSON templates are JSON-escaped since 1.1.0. If you hand-edit the template, keep the string tokens (`{headline}`, `{description}`, `{alert_type}`, `{event_type}`) inside quotes and the numeric ones (`{temp_f}`, `{wind_mph}`, `{humidity_pct}`, `{active_count}`) outside.

### Webhook fires for every poll, not just alerts

Enable **Only POST for alert transitions** on the Advanced tab. (Before 1.1.0 this setting was ignored and the webhook only ever fired on transitions.)

---

## 8. Email, MQTT

- **Email "send failed"** — check the SMTP URL scheme (`smtp://host:587` for STARTTLS, `smtps://host:465` for implicit TLS). With a username configured STARTTLS is *required*; a relay that offers no TLS will fail — remove the username for an internal unauthenticated relay. Server certificates are not verified.
- **MQTT "compiled without MQTT support"** — the device's libcurl lacks MQTT. Use the webhook instead.
- **MQTT retain has no effect** — the SDK's curl headers lacked `CURLOPT_MQTT_RETAIN` at build time; the log says so.

---

## 9. Hardware output (C1710 / C1720 / D4200)

- **Strobe fires red for a Watch** — versions before 1.1.0 hard-coded the colour name `amber`, which most devices don't have. The app now asks the device for its palette and picks the nearest colour to amber (usually `yellow`); the log line `alertoutput/color-probe:` shows what was chosen.
- **"Loading clips…" never finishes** — the device's `mediaclip.cgi` blocked and the timeout never fired (missing `CURLOPT_NOSIGNAL`; fixed in 1.1.0). The status now reports the HTTP code; models without a media clip library return 404.
- **D4200 password keeps resetting** — before 1.1.0 the UI's placeholder was stored as the password on every Hardware-tab save. Re-enter it once after upgrading.
- Threshold and lightning alerts drive hardware output at the **Watch** tier.

---

## 10. Snapshots

- **Nothing saved** — **Snapshots > Test & diagnose** distinguishes auth failure / directory error / connectivity.
- **Files disappearing from a shared SD-card folder** — only files named `YYYYMMDD_HHMMSS_*.jpg` are ever pruned (1.1.0+). Earlier versions deleted every `.jpg` in the directory.
- **Multi-camera with a non-default port** — use `host:PORT` in the Host field. Passwords may not contain `:` or `|`.

---

## 11. Verifying from the command line

```bash
# Status written by the daemon every poll (atomic; includes alerts_fetch_ok,
# overlay_id, overlay_limit_reached, version):
cat /tmp/weather_acap_status.json

# Heartbeat (Unix timestamp of last poll):
cat /tmp/weather_acap_heartbeat

# Alert history:
cat /tmp/weather_acap_history.jsonl

# Persisted overlay handle (absent = no overlay created yet):
cat /tmp/weather_acap_overlay.json

# Stored configuration (0600; contains credentials):
cat /usr/local/packages/weather_acap/localdata/params.json

# Manually toggle a virtual port:
curl --digest -u root:pass \
  "http://localhost/axis-cgi/io/virtualport.cgi?schemaversion=1&action=11&port=20"
```

---

## 12. Reinstalling

1. **Stop** the app in the Apps page
2. **Remove** / uninstall it
3. Reboot the camera (clears `/tmp` and any runtime overlays)
4. Reinstall the `.eap`

Configuration lives in `/usr/local/packages/weather_acap/localdata/params.json` and is removed on uninstall — use **Download config JSON** first if you want to keep it. Passwords are never exported; re-enter them after import.
