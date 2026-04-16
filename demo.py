#!/usr/bin/env python3
"""
demo.py — End-to-end pipeline demonstration for Weather ACAP.

Runs the full application pipeline in mock mode and prints every step
verbosely to stdout.  No pytest required — just run it directly.

Usage:
  # Dry run — prints all steps, makes zero VAPIX calls
  python demo.py --dry-run

  # Against a real Axis device using mock weather data
  python demo.py --host 192.168.1.100 --user root --pass yourpassword

  # Against a real device with live NWS data
  python demo.py --host 192.168.1.100 --user root --pass yourpassword --live

  # Show full alert descriptions
  python demo.py --dry-run --verbose

Options:
  --host HOST       Device IP or hostname (default: localhost)
  --user USER       VAPIX username (default: root)
  --pass PASS       VAPIX password (default: empty)
  --zip  ZIP        ZIP code to use (default: reads from config_store.json)
  --live            Use live NWS API instead of mock_weather.json
  --dry-run         Skip all VAPIX calls (no device needed)
  --verbose         Print full alert descriptions
"""

import argparse
import json
import logging
import os
import sys
import time
from pathlib import Path

# Add app/ to the module search path so we can import the application modules.
_APP_DIR = Path(__file__).parent / "app"
sys.path.insert(0, str(_APP_DIR))


# ── ANSI colour helpers ────────────────────────────────────────────────────────

_USE_COLOUR = sys.stdout.isatty()

def _c(code: str, text: str) -> str:
    return f"\033[{code}m{text}\033[0m" if _USE_COLOUR else text

def red(t):    return _c("31;1", t)
def yellow(t): return _c("33;1", t)
def green(t):  return _c("32;1", t)
def cyan(t):   return _c("36;1", t)
def bold(t):   return _c("1", t)
def dim(t):    return _c("2", t)


# ── Separator helpers ──────────────────────────────────────────────────────────

def _sep(title: str = "", width: int = 64) -> None:
    if title:
        pad = (width - len(title) - 2) // 2
        print(f"\n{'─' * pad} {bold(title)} {'─' * pad}")
    else:
        print("─" * width)


# ── Dry-run VAPIX stub ────────────────────────────────────────────────────────

class DryRunVapixClient:
    """Prints what would be sent to VAPIX without making any HTTP calls."""

    def activate_virtual_port(self, port: int) -> bool:
        print(f"  {green('→ VAPIX')} GET /axis-cgi/io/virtualport.cgi"
              f"?schemaversion=1&action=11&port={port}  "
              f"{dim('[DRY RUN — no call made]')}")
        return True

    def deactivate_virtual_port(self, port: int) -> bool:
        print(f"  {yellow('← VAPIX')} GET /axis-cgi/io/virtualport.cgi"
              f"?schemaversion=1&action=10&port={port}  "
              f"{dim('[DRY RUN — no call made]')}")
        return True

    def create_overlay(self, text: str, position: str = "topLeft"):
        print(f"  {green('→ VAPIX')} POST /vapix/overlays/text  "
              f"position={position!r}  "
              f"{dim('[DRY RUN — no call made]')}")
        print(f"           text: {cyan(repr(text))}")
        return "dry-run-overlay-id"

    def update_overlay(self, overlay_id: str, text: str) -> bool:
        print(f"  {green('→ VAPIX')} PUT /vapix/overlays/text/{overlay_id}  "
              f"{dim('[DRY RUN — no call made]')}")
        print(f"           text: {cyan(repr(text))}")
        return True

    def delete_overlay(self, overlay_id: str) -> bool:
        print(f"  {yellow('← VAPIX')} DELETE /vapix/overlays/text/{overlay_id}  "
              f"{dim('[DRY RUN — no call made]')}")
        return True

    def list_overlays(self) -> list:
        return []

    def get_virtual_port_list(self) -> list:
        return list(range(1, 65))

    def get_basic_device_info(self) -> dict:
        return {"Model": "DRY-RUN", "Version": "0.0.0", "SerialNumber": "DRYRUN000"}

    def list_params(self, group: str) -> str:
        if "Properties.Image" in group:
            return "root.Properties.Image.Resolution=1920x1080"
        return ""

    def __enter__(self): return self
    def __exit__(self, *_): pass


# ── Minimal config shim ───────────────────────────────────────────────────────

class _DemoConfig:
    """Wraps the real Config but allows CLI overrides."""

    def __init__(self, real_cfg, args):
        self._cfg = real_cfg
        self._args = args

    @property
    def vapix_host(self):      return self._args.host
    @property
    def vapix_user(self):      return self._args.user
    @property
    def vapix_pass(self):      return getattr(self._args, 'pass') or ""
    @property
    def vapix_base_url(self):
        h = self._args.host
        return h if h.startswith("http") else f"http://{h}"
    @property
    def zip_code(self):        return self._args.zip or self._cfg.zip_code
    @property
    def lat_override(self):    return self._cfg.lat_override
    @property
    def lon_override(self):    return self._cfg.lon_override
    @property
    def mock_mode(self):       return not self._args.live
    @property
    def enabled_alert_types(self): return self._cfg.enabled_alert_types
    @property
    def all_alert_types(self): return self._cfg.all_alert_types
    @property
    def virtual_port_start(self): return self._cfg.virtual_port_start
    @property
    def overlay_enabled(self): return self._cfg.overlay_enabled
    @property
    def overlay_position(self): return self._cfg.overlay_position
    @property
    def overlay_max_chars(self): return self._cfg.overlay_max_chars
    @property
    def poll_interval(self):   return 60
    @property
    def nws_user_agent(self):  return self._cfg.nws_user_agent
    @property
    def log_level(self):       return "INFO"


# ── Main demo ─────────────────────────────────────────────────────────────────

def main():
    parser = argparse.ArgumentParser(
        description="Weather ACAP end-to-end demo",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=__doc__,
    )
    parser.add_argument("--host",    default="localhost", help="Device IP or hostname")
    parser.add_argument("--user",    default="root",      help="VAPIX username")
    parser.add_argument("--pass",    default="",          dest="vapix_pass",
                        help="VAPIX password")
    parser.add_argument("--zip",     default=None,        help="ZIP code override")
    parser.add_argument("--live",    action="store_true", help="Use live NWS API")
    parser.add_argument("--dry-run", action="store_true", dest="dry_run",
                        help="Skip VAPIX calls")
    parser.add_argument("--verbose", action="store_true", help="Print full alert text")
    args = parser.parse_args()
    # Make password accessible as args.vapix_pass
    args.__dict__["pass"] = args.vapix_pass

    logging.basicConfig(
        level=logging.WARNING,  # suppress library noise in demo output
        format="%(levelname)s %(name)s: %(message)s",
    )

    print()
    print(bold("╔══════════════════════════════════════════════════╗"))
    print(bold("║       Weather ACAP — End-to-End Demo             ║"))
    print(bold("╚══════════════════════════════════════════════════╝"))
    print()

    # ── Step 1: Load config ───────────────────────────────────────────────────
    _sep("Step 1: Load Configuration")
    from config import Config
    real_cfg = Config.load()
    cfg = _DemoConfig(real_cfg, args)

    print(f"  Mode:             {cyan('MOCK (mock_weather.json)') if cfg.mock_mode else green('LIVE (NWS API)')}")
    print(f"  VAPIX target:     {cfg.vapix_base_url}")
    print(f"  ZIP code:         {cfg.zip_code or dim('(not set)')}")
    if cfg.lat_override:
        print(f"  Coord override:   lat={cfg.lat_override}  lon={cfg.lon_override}")
    print(f"  Enabled alerts:   {cfg.enabled_alert_types}")
    print(f"  Virtual ports:    {cfg.virtual_port_start}–{cfg.virtual_port_start + len(cfg.enabled_alert_types) - 1}")
    print(f"  Overlay:          {'enabled at ' + cfg.overlay_position if cfg.overlay_enabled else 'disabled'}")
    if args.dry_run:
        print(f"  {yellow('DRY RUN:')} no VAPIX HTTP calls will be made.")

    # ── Step 2: Build VAPIX client ────────────────────────────────────────────
    _sep("Step 2: VAPIX Client")
    if args.dry_run:
        from vapix_client import VapixClient
        vapix = DryRunVapixClient()
        print(f"  Using {yellow('DryRunVapixClient')} — all VAPIX calls are printed but not sent.")
    else:
        from vapix_client import VapixClient
        vapix = VapixClient(
            base_url=cfg.vapix_base_url,
            user=cfg.vapix_user,
            password=cfg.vapix_pass,
        )
        print(f"  Live VapixClient → {cfg.vapix_base_url}")

    # ── Step 3: Device probe ──────────────────────────────────────────────────
    _sep("Step 3: Device Probe")
    from device_probe import probe, warn_if_port_range_exceeds, UNKNOWN_CAPABILITIES
    caps = probe(vapix)
    print(f"  Model:            {caps.model}")
    print(f"  Firmware:         {caps.firmware_version}")
    print(f"  Serial:           {caps.serial_number}")
    print(f"  Has video:        {green('Yes') if caps.has_video else yellow('No (speaker/intercom)')}")
    print(f"  Max virtual in.:  {caps.max_virtual_inputs}")
    warn_if_port_range_exceeds(caps, cfg)

    # ── Step 4: Fetch weather ─────────────────────────────────────────────────
    _sep("Step 4: Fetch Weather Data")
    from nws_client import NWSClient
    nws = NWSClient(user_agent=cfg.nws_user_agent)

    print(f"  Fetching from {'mock_weather.json' if cfg.mock_mode else 'NWS API'}...")
    t0 = time.time()
    weather = nws.fetch(cfg)
    elapsed = time.time() - t0
    print(f"  Fetched in {elapsed:.2f}s  [source={weather.source}]")

    c = weather.conditions
    print()
    print(f"  {bold('Current Conditions:')}")
    print(f"    Temperature:   {c.temperature_f}°F / {c.temperature_c}°C")
    print(f"    Sky:           {c.sky_condition}")
    print(f"    Wind:          {c.wind_speed_mph} mph {c.wind_direction}")
    print(f"    Humidity:      {c.relative_humidity_pct}%")
    print(f"    Observed at:   {c.observed_at}")

    print()
    print(f"  {bold('Active Alerts')} ({len(weather.alerts)} total):")
    if not weather.alerts:
        print(f"    {dim('None')}")
    for a in weather.alerts:
        colour = red if a.severity == "Extreme" else yellow
        print(f"    {colour('■')} {bold(a.event)}")
        print(f"      Severity: {a.severity}  Urgency: {a.urgency}")
        print(f"      Expires:  {a.expires}")
        print(f"      Areas:    {a.areas_affected}")
        if args.verbose:
            print(f"      Headline: {a.headline}")
            print(f"      Instruction: {a.instruction[:200]}...")
        print()

    # ── Step 5: Event engine ──────────────────────────────────────────────────
    _sep("Step 5: Event Engine (Virtual Port Triggers)")
    from event_engine import EngineState, run as run_engine

    print(f"  Enabled alert types → port mapping:")
    for i, alert_type in enumerate(cfg.enabled_alert_types):
        port = cfg.virtual_port_start + i
        matched = any(a.event.lower() == alert_type.lower() for a in weather.alerts)
        indicator = green("● ACTIVE") if matched else dim("○ inactive")
        print(f"    Port {port:>2}: {alert_type:<35} {indicator}")

    print()
    print("  Firing VAPIX virtual port changes:")
    engine_state = run_engine(weather, cfg, vapix, EngineState())
    print(f"\n  Active ports after engine run: {sorted(engine_state.active_ports) or '(none)'}")

    # ── Step 6: Overlay ───────────────────────────────────────────────────────
    _sep("Step 6: Video Overlay")
    from overlay_manager import OverlayState, update as update_overlay, cleanup as cleanup_overlay

    if not caps.has_video:
        print(f"  {yellow('Skipped')} — device has no video capability.")
    elif not cfg.overlay_enabled:
        print(f"  {yellow('Skipped')} — overlay disabled in config.")
    else:
        print("  Sending overlay update to device:")
        overlay_state = update_overlay(weather, cfg, vapix, OverlayState(), caps.has_video)
        print(f"  Overlay ID: {overlay_state.overlay_id or dim('(not created)')}")

        print()
        print(f"  {bold('Cleanup:')} removing overlay from device...")
        cleanup_overlay(vapix, overlay_state)

    # ── Summary ───────────────────────────────────────────────────────────────
    _sep()
    print()
    print(f"  {green('✓')} Demo complete.")
    print()
    if args.dry_run:
        print(f"  No VAPIX calls were made. To test against a real device:")
        print(f"  {dim('python demo.py --host <device-ip> --user root --pass <password>')}")
    print()


if __name__ == "__main__":
    main()
