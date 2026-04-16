# Troubleshooting — Weather Monitor ACAP

This guide covers the most common issues when installing, configuring, or running the Weather Monitor ACAP.

---

## 1. App won't start or immediately stops

**Check the system log:**

In the camera web interface, go to **System > Logs > System log** and look for entries containing `weather_acap`.

Common causes:

| Log message | Cause | Fix |
|---|---|---|
| `axparameter init failed` | Parameter database error | Uninstall and reinstall the ACAP |
| `curl_global_init` errors | libcurl not available | Verify AXIS OS version supports native ACAP |
| No log entries at all | App binary didn't launch | Check architecture — the `.eap` must match your device (aarch64) |

**Via SSH (if available):**

```bash
ssh root@<camera-ip>
journalctl -u weather_acap --no-pager -n 50
# or
cat /var/log/messages | grep weather_acap | tail -50
```

---

## 2. VAPIX connection failures

The app calls VAPIX on `localhost` to control virtual ports and overlays. If the Diagnostics tab's **Test VAPIX** button fails:

### "HTTP 0" or "Connection refused"

The app can't reach the camera's internal web server. This is rare on native ACAPs since they run directly on the device. Try:

- Verify the camera's web interface is accessible from a browser
- Check that no firewall rules block localhost connections

### "HTTP 401" — Authentication failed

VAPIX uses Digest authentication. Verify credentials:

```bash
# From the camera via SSH, or from your workstation:
curl -v --digest -u root:yourpassword \
  "http://<camera-ip>/axis-cgi/param.cgi?action=list&group=root.Brand"
```

- The username must have **Operator** or **Administrator** role
- Go to the **Advanced** tab in the Weather ACAP UI and re-enter the VAPIX password (it shows "(unchanged)" but won't re-send the old password — you must type it again if you need to change it)

### "HTTP 404" — Endpoint not found

The VAPIX endpoint may not exist on your firmware version:

```bash
# Test virtual port support:
curl --digest -u root:pass \
  "http://localhost/axis-cgi/param.cgi?action=list&group=Properties.VirtualInput"

# Test overlay API:
curl --digest -u root:pass \
  "http://localhost/vapix/overlays/text"
```

If virtual ports return 404, your device may not support them. Check the [VAPIX library documentation](https://developer.axis.com/vapix/).

---

## 3. Virtual input ports not working

### Ports don't appear in Action Rules

After the ACAP activates a virtual port, it should appear under **System > Events > Device events** as "Virtual input N". If it doesn't:

1. Go to the **Diagnostics** tab and use **Fire port** with the port number
2. Check the result — "Fired 20" means the VAPIX call succeeded
3. Refresh the camera's Events page — the virtual input should now appear

### VMS doesn't see the event

- In Axis Camera Station Pro: **Configuration > Devices** > right-click camera > **Events** > verify virtual input events are listed
- Some VMS platforms cache the event list — remove and re-add the camera to refresh

### Port number out of range

The camera reports how many virtual ports it supports (shown on the Dashboard as "N virtual ports available"). If your mapped port numbers exceed this, the VAPIX call will fail with HTTP error code in the test result.

Fix: On the **Alerts & Triggers** tab, click **Auto-assign ports** to renumber starting at 20, or manually lower port numbers.

---

## 4. Weather data not loading

### "weather fetch failed" in status

1. Go to **Diagnostics** > **Test weather fetch** to see the detailed error
2. Common causes:
   - **No ZIP code set** — go to Location tab and enter a US ZIP
   - **NWS API down** — the app auto-falls back to Open-Meteo if provider is set to "auto"
   - **DNS resolution failure** — the camera needs internet access to reach `api.weather.gov`

### NWS returns no data for my location

NWS coverage is US-only. For international locations:
- Set provider to **Open-Meteo** (worldwide, but no alerts)
- Or set provider to **Auto** — it will use Open-Meteo as fallback

### NWS rate limiting

NWS asks for a User-Agent header with contact info. On the **Location** tab, set the NWS User-Agent field to something like:

```
WeatherACAP/2.0 (yourname@company.com)
```

NWS recommends polling no more often than every 5 minutes (300 seconds). The minimum is enforced at 60 seconds.

---

## 5. Overlay not showing on video

### Device has no video

The app auto-detects video capability. Speakers, intercoms, and radar devices won't get overlays — this is normal and logged as informational.

### Overlay is enabled but nothing appears

1. On the **Diagnostics** tab, click **Test VAPIX** — check that "Video: yes" appears
2. On the **Overlay** tab, verify the toggle is on and the template isn't empty
3. Check **Live preview** — if it shows text, the template is working
4. Look at the camera's live view — the overlay may be in a corner you're not looking at (try changing position)

### Overlay text is stale

The overlay updates every poll cycle (default 300 seconds). To force an immediate update, reduce the poll interval temporarily or restart the app.

---

## 6. Webhook not firing

1. Go to **Advanced** tab — verify webhook is enabled and the URL is correct
2. Go to **Diagnostics** > **Test webhook** — this sends a sample payload and shows the HTTP response code
3. Common issues:
   - **HTTP 0** — DNS or connection failure (camera can't reach the URL)
   - **HTTP 403/401** — authentication required on the target endpoint
   - **"webhook URL not set"** — save the URL on the Advanced tab first

### Webhook fires for every poll, not just alerts

Enable **"Only POST for alert transitions"** on the Advanced tab. This limits webhooks to only fire when an alert activates or clears.

---

## 7. Fire drill

The **Fire Drill** button on the Diagnostics tab activates all enabled virtual ports simultaneously. This is the quickest way to verify your entire chain end-to-end:

1. Click **Fire Drill**
2. Check that all expected Action Rules / VMS responses trigger
3. Click **Clear all ports** when done

Ports remain active until cleared manually or until the next weather poll cycle clears them naturally.

---

## 8. Config export/import

### Exporting

Click **Download config JSON** on the Advanced tab. The file contains all settings except the VAPIX password (security — passwords are not exported).

### Importing

Click **Upload config JSON** and select a previously exported file. All fields are overwritten. You'll need to re-enter the VAPIX password after import.

### Fleet deployment

Export from a fully configured camera, then import to each additional camera. Only the ZIP code / coordinates need to be changed per-site.

---

## 9. Verifying from the command line

If you have SSH access to the camera, these curl commands can help debug:

```bash
# Check current status (written by the daemon every poll cycle):
cat /tmp/weather_acap_status.json | python3 -m json.tool

# Check heartbeat (Unix timestamp of last successful poll):
cat /tmp/weather_acap_heartbeat

# Check alert history:
cat /tmp/weather_acap_history.jsonl

# Manually test a virtual port:
curl --digest -u root:pass \
  "http://localhost/axis-cgi/io/virtualport.cgi?schemaversion=1&action=11&port=20"

# Check overlay:
curl --digest -u root:pass \
  "http://localhost/vapix/overlays/text"

# List all app parameters:
curl --digest -u root:pass \
  "http://localhost/axis-cgi/param.cgi?action=list&group=root.weather_acap"
```

---

## 10. Reinstalling

If the app is in a bad state:

1. **Stop** the app in the Apps page
2. **Remove** / uninstall it
3. Reboot the camera (clears `/tmp` files)
4. Reinstall the `.eap`

All configuration is stored in axparameter and will be lost on uninstall. Use config export beforehand if you want to preserve settings.
