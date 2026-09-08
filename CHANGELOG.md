# Changelog

All notable changes to this project will be documented in this file.

## [1.1.0] — 2026-09-08

Hardening release after a full-codebase audit. No configuration migration
is needed; existing `params.json` files load unchanged. Two defaults changed
for **fresh installs only**: the `Excessive Heat Warning` rule is now
`Extreme Heat Warning` (NWS renamed the product in 2025), and the
User-Agent string reports the real version.

### Fixed — overlay
- **Overlays no longer stack across restarts.** The overlay handle is
  persisted to `/tmp` (same lifetime as the camera's runtime overlays), so a
  crash, respawn, upgrade or stop/start reuses the existing overlay instead
  of creating another. Previously every restart added one until the camera
  hit its limit and the overlay silently stopped appearing.
- **Diagnostics → "Remove all text overlays"** clears orphaned overlays left
  by earlier versions; the Overlay tab now says when the limit is reached.
- Wrong VAPIX credentials at the first poll no longer disable the overlay
  for the life of the process (negative video probe is re-tried; 401 is
  logged distinctly from "no video").
- Changing the overlay position takes effect (the overlay is re-created;
  `setText` cannot move one). Disabling the overlay, or the master switch,
  removes it from the video instead of leaving stale text.
- A stale handle (`setText` error) is re-added in the same poll instead of a
  full interval later.
- `{time}` is the camera's local time; `{utc}` keeps the old UTC form.

### Fixed — alert state and data quality
- **A failed NWS alert fetch is no longer treated as "no alerts".** It used
  to clear every mapped port, fire "cleared" on every channel, then re-fire
  everything on the next good poll. Port state is now held and the status
  pill shows "NWS unreachable". HTTP ≥ 400 error bodies are rejected instead
  of being parsed as data.
- **NWS wind speed was 3.6× too high** — the API reports km/h, the code
  assumed m/s. `unitCode` is honoured for wind and temperature.
- Null observation fields (very common) no longer produce `0 °F / 0 mph` with
  `valid=true`; a null temperature invalidates the observation and falls back
  to Open-Meteo, null wind/humidity are reported as unknown and skipped by
  threshold rules.
- Alert and threshold port state is keyed by rule identity, not table row.
  Editing, reordering or deleting a rule while its alert was active used to
  strand the port ON (or clear the wrong one). Removed/disabled rules now
  clear through the normal transition path (history, events, notifications).
- A failed VAPIX port write is retried every poll until the camera agrees.
- Every mapped port is forced OFF at startup: a crash or power loss skipped
  the clean-shutdown clear and the next process had no memory of them.
- Clean shutdown and the master switch now clear threshold and lightning
  ports too (previously NWS ports only).
- Threshold rules hold state when conditions are invalid this poll.
- NWS `Test`/`Exercise` products and `Cancel`/`Expire` messages are skipped;
  the per-poll alert cap is 32 (was 16).

### Fixed — lightning (SPC)
- `LightningPollMult = 0` crashed the daemon (SIGFPE) in a respawn loop;
  `= 1` checked SPC exactly once. Values are clamped and the cadence fixed.
- The `{lightning}` overlay token and status field were populated *after*
  the overlay/status were written, so they were always blank. The risk is
  now held between checks. A failed SPC fetch keeps the previous state.
- Lightning transitions go through the same path as other alerts (history,
  native events, notifications, hardware output at Watch tier).
- Point-in-polygon walk was O(V²) per ring on the single daemon thread;
  now linear.

### Fixed — notifications and hardware output
- **Webhook templates JSON-escape their tokens** — a headline containing a
  quote or newline broke every Slack/Teams/Discord/HA template. 4xx/5xx
  responses are logged as warnings; redirects keep the POST body.
- `WebhookOnAlertsOnly = no` (post on every poll) was read and ignored.
- Cool-down no longer suppresses a "cleared" whose "activated" was sent, so
  downstream consumers never latch active; uses the monotonic clock so an
  NTP step at boot can't mute notifications; table 64 → 256 entries.
- Native AXIS `Alert` event's `active` property means "any alert active",
  not "this transition was an activation".
- Threshold and lightning alerts drive hardware output at **Watch** tier; a
  humidity crossing no longer fires the red strobe / emergency profile.
- Strobe colour is probed once from `getCapabilities` (both tiers from one
  call) and only cached on success; the strobe is stopped explicitly on
  clear. "Extreme Cold/Heat Watch" now classify as Watch.
- Email: CR/LF stripped from header fields (threshold labels are
  user-typed), `8bit` transfer encoding for the UTF-8 body, message buffer
  sized for long recipient lists, STARTTLS **required** when a username is
  configured.
- MQTT topic segments are percent-encoded (`#`, `?`, `%`, space).
- Snapshot pruning only deletes files this app created
  (`YYYYMMDD_HHMMSS_*.jpg`) — a shared SnapshotSaveDir lost unrelated images.
- Multi-camera `host:PORT` records parse correctly (the port used to become
  the username and the real password landed in filenames and syslog).
- Every libcurl call sets `CURLOPT_NOSIGNAL` and a connect timeout; the
  overlay, port, snapshot, email, MQTT and webhook calls could previously
  hang past their timeouts under FastCGI/GLib. Remote snapshots 15 s → 8 s.
- Use-after-free of the content-type string in both snapshot functions.
- `SIGPIPE` ignored; signal handlers moved into the GLib loop (the old
  SIGCHLD handler called `syslog` — not async-signal-safe).

### Fixed — configuration and web UI
- **D4200 password** was returned in plaintext by the config/export
  endpoints and the UI's `__SET__` placeholder was *stored* as the password
  on every Hardware-tab save.
- Saving while the daemon's config file was missing/unreadable reset every
  other setting to compiled defaults (and the CGI's fallback alert map had 3
  rules instead of 15). Saves are refused until the daemon has written it.
- The daemon and the CGI shared one temp filename for the config export and
  could truncate each other's writes.
- Server-side range clamping for every numeric field (the HTML limits were
  never enforced). Import no longer silently drops fields past the 64th.
- Multi-camera passwords are masked in the UI/export and substituted back on
  save. Config/save files containing credentials are created `0600`.
- Status/history reads tolerate a torn line instead of failing the whole
  response; the status file is written atomically; history includes the
  rotated file after rotation.
- `PollInterval` takes effect on save (previously required a restart).
  A Save click does one flash sync instead of ~70.
- FastCGI backend is respawned (rate-limited) if it dies, instead of the
  web UI returning 503 until the app is restarted.
- History rows highlight activations (the class check used a value the
  daemon never writes). Clip-list failures keep the saved clip id.
  Lightning port max 64 (default is 35). Select fields fall back to
  defaults. New Dashboard "Poll now" button; version shown in the header.
- Bundled cJSON: heap overflow on a short `\u` escape fixed; `\uXXXX` is
  decoded to UTF-8 (NWS headlines with typographic dashes were `?`);
  malformed input returns NULL instead of a partial tree.

### Build / packaging
- `Dockerfile` now builds a working package: `-a weather_acap.cgi` (the CGI
  was omitted → web UI HTTP 500) and the target architecture is stamped.
- `$(LDFLAGS)` is passed at link time (RELRO / BIND_NOW were being dropped);
  header dependency tracking added; dead `CMakeLists.txt` removed.
- Build Check workflow verifies the real binary names and fails on compiler
  warnings. `.gitattributes` pins LF for the Makefile.
- Single version source (`app/version.h`); manifest 1.1.0; vendor URL points
  at the repository.

## [1.0.1] — 2026-04-16

### Fixed
- CGI binary now uses `.cgi` extension — required by the camera's web server for proper CGI routing
- Web UI fetch error handling improved: shows HTTP status and content-type mismatches instead of generic parse errors

### Changed
- Removed all legacy Python code, mock data fixtures, and container-era build artifacts
- Cleaned `.gitignore` to only cover native ACAP build outputs, secrets, and IDE files
- Removed `python3` reference from TROUBLESHOOTING.md

## [1.0.0] — 2026-04-15

### Added
- Initial native C ACAP release for Axis cameras (aarch64 + armv7hf)
- NWS and Open-Meteo weather providers with automatic fallback
- 15 default alert-to-virtual-port mappings (Tornado Warning, Severe Thunderstorm, etc.)
- Storm-themed dark web UI with 6 tabs: Dashboard, Location, Alerts & Triggers, Overlay, Diagnostics, Advanced
- Dynamic text overlay on live video with customizable template variables
- Built-in diagnostics: VAPIX self-test, weather fetch test, webhook test, fire drill
- Outbound webhook on alert transitions (JSON payload)
- Alert history with JSONL ring buffer
- Config export/import for fleet deployment
- Mock mode for bench-testing without live weather data
- GitHub Actions CI building both aarch64 and armv7hf .eap artifacts

### Between 1.0.1 and 1.1.0 (unreleased sprints, April 2026)
Sprint 2 snapshot-on-alert · Sprint 3 MQTT + email · Sprint 4 capture /
multi-recipient · Sprint 5 threshold rules + snapshot auto-delete · Sprint 6
condition history sparklines · Sprint 7 notification cool-down · Sprint 8
multi-camera snapshots · Sprint 9 native AXIS events · Sprint 10 UI polish
(mock banner, severity badges, wind arrow) · Sprint 11 webhook templates
with Slack/Discord/Teams/Home Assistant presets · Sprint 12 SPC lightning
risk · Sprint 13 structured JSON logging · Sprint 14 hardware alert output
(C1710/C1720 display + strobe, D4200 profiles, audio clips). Also: file-backed
parameter store replacing axparameter, FastCGI web backend replacing
transferCgi, JSON-RPC dynamic overlay for AXIS OS 11+/12.
