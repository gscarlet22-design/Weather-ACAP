# Changelog

All notable changes to this project will be documented in this file.

## [1.0.1] — 2026-04-16

### Fixed
- CGI binary now uses `.cgi` extension — required by the camera's lighttpd for proper CGI routing
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
