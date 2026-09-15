# Weather ACAP — session handoff (2026-09-15)

Read this first when picking the project up again. It captures state that is
not derivable from the code or git history.

## Where things stand

| Item | State |
|---|---|
| Branch | `fix/audit-hardening` (8 commits on top of `main`), pushed |
| Pull request | https://github.com/gscarlet22-design/Weather-ACAP/pull/15 — open, all checks green |
| Version | 1.1.0 (`app/version.h`, `app/manifest.json`, `CHANGELOG.md` top entry) |
| Latest package | `dist/v1.1.0-rc3/weather-acap-1.1.0-rc3-aarch64.eap` (CI artifact from PR run 34991608159, commit `12a96ba`) |
| Audit report | https://claude.ai/code/artifact/08ec10ca-45b8-4138-bc56-2308a4d07e2b |
| Test camera | AXIS M3086-V (CV25 → aarch64), AXIS OS 12.9.x, ZIP 66103, poll interval 60 s |
| Camera state | **rc1 (commit 837bb53) is installed and running as 1.1.0.** The Apps page showed 1.0.1 only because the AXIS management proxy (`AxisDAX`) timed out during the 25-second install; the camera log confirms `Successfully installed` and `starting up … version 1.1.0`. rc2/rc3 not yet installed. |

CI is the only compiler available (no Docker/gcc on the workstation). `Build
Check` fails on any compiler warning. Packaging artifacts are named from the
PR merge-commit SHA, so download with `gh run download <run> --pattern "*aarch64*"`.

## What was done (summary — details in CHANGELOG.md and the audit report)

1. Full-codebase audit (4 parallel reviewers), ~60 defects fixed across daemon,
   CGI, UI, build. Overlay root cause: handle kept only in memory → one new
   overlay per restart → camera limit (error 300) → overlay silently gone.
   Handle now persisted to `/tmp/weather_acap_overlay.json`; Diagnostics gained
   **Remove all text overlays**.
2. On-device log from the first 1.1.0 start surfaced two more bugs, fixed in
   rc2: AXIS event declarations lacked the `tnsaxis` topic path (events never
   existed), and virtual-port writes returned non-200 for all 15 ports
   (`vapix_port_set` now probes three API variants and logs which works).
3. UI redesign from the separate design session integrated in rc3:
   `app/html/index.html`, `style.css`, new `ui.js`; `app.js` unchanged.
   Desktop preview lives in `tools/ui-preview/` — **never put dev files in
   `app/html/`**, `acap-build` ships that whole directory.

## Next steps, in order

1. Install rc3 on the M3086-V (Apps → Add app). Expect the management proxy to
   report a timeout again; wait 30 s and reload the Apps page. Header of the app
   UI should read **v1.1.0**.
2. In the camera System log after start, look for:
   - `axisevents: Alert event declared (id=…)` and `Conditions event declared`
     — if instead `declare … failed`, paste the message (topic fix unverified).
   - `vapix: virtual input API variant N works on this firmware` — if instead
     `failed on every virtual input API`, paste the three `vapix: port … via api`
     lines before it. **Until this is confirmed, virtual ports may never have
     actually worked on this firmware** (old code used HEAD requests).
   - `alerts: startup reset — N/15 mapped ports forced OFF` with N > 0.
3. Diagnostics → Overlay maintenance → **Remove all text overlays** (one time).
   Overlay should appear within seconds; Overlay tab shows "live … (handle N)".
   Stop/start the app: handle must stay the same.
4. Hardware Output: re-enter the D4200 password once (old build overwrote it).
   Test the Watch strobe; log line `alertoutput/color-probe:` shows the colour
   chosen. On clear, `alertoutput/strobe: stop id=…` (stop-id parsing unverified).
5. Sanity-check Dashboard wind speed vs the NWS station (old readings were
   3.6× high); review any `WindMph` threshold rules.
6. Walk the new UI on the camera's own browser engine: each tab, a save on each
   section, overlay builder round-trip, audio clip pills, snapshot gallery.
   `aspect-ratio` fallback may be needed on old firmware (design session note).
7. Merge PR #15 → tag `v1.1.0` → the release workflow publishes both `.eap`s
   (first release ever). Then delete the 20 fully-merged branches (6 local,
   14 remote) and the dev trees under `builds/`, `dist/`, `.ci-artifact/`.

## Open questions / unverified

- Which virtual-input API the firmware accepts (see step 2).
- `siren_and_light.cgi` `stop` id field and the overlay `list` response key
  (`textOverlays`) were implemented from docs; both log the raw body if the
  shape differs.
- One NWS `/points` request timed out at 10 s right after install (during the
  Apache reload). Treated as transient; watch for repeats of `nws/http_get FAILED`.
- Whether the AXIS session cookie's SameSite setting makes the CGI's POST
  endpoints CSRF-exposed (no token yet).

## Deferred by design (needs a decision)

Worker thread / curl_multi for remote notification channels (worst case
~60–90 s per transition today); threshold hysteresis; local alert expiry from
NWS `expires`; VAPIX service-account credentials instead of a stored password;
CSRF token + FastCGI socket 0777; dedup of escapers/curl blocks; route module
`syslog()` calls through `jlog`. Listed with rationale in the audit report §7
and in memory (`project_audit_followups.md`).

## Gotchas learned this session

- Working-tree files are CRLF (autocrlf); `.gitattributes` now pins LF for
  sources. Scripted edits with `\n` anchors need `sed -i 's/\r$//'` first.
- The Edit tool cannot match source lines containing literal `\uXXXX` escape
  text; use perl/sed splices there.
- `curl` in this codebase must always set `CURLOPT_NOSIGNAL` — without it
  timeouts never fire under GLib / FastCGI.
- Runtime text overlays are invisible in the camera's Overlays page and
  survive app restarts; only a reboot or the purge endpoint clears them.
- NWS renamed "Excessive Heat Warning" → "Extreme Heat Warning" (2025);
  defaults updated, existing `params.json` files keep whatever they have.
