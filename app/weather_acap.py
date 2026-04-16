"""
weather_acap.py — Main entry point for Weather ACAP.

Startup sequence:
  1. Configure logging from LOG_LEVEL env var.
  2. Load config (env vars → config_store.json → config_defaults.json).
  3. Build VAPIX client and probe device capabilities.
  4. Warn if virtual port range exceeds device maximum.
  5. Start Flask config UI on port 8080 in a daemon thread.
  6. Run initial weather poll immediately.
  7. Enter the poll loop: fetch → event engine → overlay → heartbeat → sleep.

Shutdown:
  SIGTERM / SIGINT → deactivate all active virtual ports → delete overlay → exit.
"""

import logging
import os
import signal
import sys
import threading
import time
from pathlib import Path

# ── Configure logging before any other imports ────────────────────────────────
_LOG_LEVEL = os.environ.get("LOG_LEVEL", "INFO").upper()
logging.basicConfig(
    level=getattr(logging, _LOG_LEVEL, logging.INFO),
    format="%(asctime)s [%(levelname)s] %(name)s: %(message)s",
    datefmt="%Y-%m-%dT%H:%M:%S",
    stream=sys.stdout,
)
logger = logging.getLogger("weather_acap")

# ── Application imports ───────────────────────────────────────────────────────
from config import Config
from device_probe import probe, warn_if_port_range_exceeds, UNKNOWN_CAPABILITIES
from event_engine import EngineState, run as run_event_engine
from nws_client import NWSClient
from overlay_manager import OverlayState, cleanup as cleanup_overlay, update as update_overlay
from vapix_client import VapixClient
from web_ui import run_web_ui, update_status

# ── Heartbeat file (monitored by Docker HEALTHCHECK) ─────────────────────────
_HEARTBEAT_PATH = Path("/tmp/heartbeat")


def _touch_heartbeat() -> None:
    try:
        _HEARTBEAT_PATH.touch(exist_ok=True)
    except OSError:
        pass


# ── Graceful shutdown ─────────────────────────────────────────────────────────

class _ShutdownFlag:
    def __init__(self):
        self._event = threading.Event()

    def set(self):
        self._event.set()

    @property
    def is_set(self) -> bool:
        return self._event.is_set()

    def wait(self, timeout: float) -> bool:
        """Returns True if shutdown was requested before timeout."""
        return self._event.wait(timeout=timeout)


_shutdown = _ShutdownFlag()


def _handle_signal(signum, _frame):
    logger.info("Received signal %d — initiating graceful shutdown.", signum)
    _shutdown.set()


# ── Poll cycle ────────────────────────────────────────────────────────────────

def _poll_cycle(
    cfg: Config,
    nws: NWSClient,
    vapix: VapixClient,
    engine_state: EngineState,
    overlay_state: OverlayState,
    has_video: bool,
) -> tuple[EngineState, OverlayState]:
    """
    Execute one complete poll cycle.
    Returns updated (engine_state, overlay_state).
    Catches all exceptions so the main loop never crashes on a transient error.
    """
    try:
        # ── 1. Fetch weather ──────────────────────────────────────────────────
        weather = nws.fetch(cfg)
        logger.info(
            "Weather fetched [%s]: %s | %d active alert(s)",
            weather.source,
            weather.conditions.summary(),
            len(weather.alerts),
        )

        # ── 2. Push status to web UI ──────────────────────────────────────────
        update_status({
            "source": weather.source,
            "conditions": {
                "temperature_f": weather.conditions.temperature_f,
                "sky_condition": weather.conditions.sky_condition,
                "wind_speed_mph": weather.conditions.wind_speed_mph,
                "wind_direction": weather.conditions.wind_direction,
                "relative_humidity_pct": weather.conditions.relative_humidity_pct,
                "observed_at": weather.conditions.observed_at,
            },
            "active_alerts": [
                {
                    "event": a.event,
                    "headline": a.headline,
                    "severity": a.severity,
                    "urgency": a.urgency,
                    "expires": a.expires,
                }
                for a in weather.alerts
            ],
            "last_poll_utc": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
        })

        # ── 3. Event engine (virtual port triggers) ───────────────────────────
        engine_state = run_event_engine(weather, cfg, vapix, engine_state)

        # ── 4. Overlay update (cameras only) ──────────────────────────────────
        overlay_state = update_overlay(weather, cfg, vapix, overlay_state, has_video)

    except ValueError as exc:
        # Typically: no ZIP configured, or ZIP not found.
        logger.warning("Configuration issue — skipping this poll cycle: %s", exc)
    except Exception as exc:
        logger.error(
            "Unexpected error in poll cycle: %s: %s",
            type(exc).__name__, exc, exc_info=True,
        )

    return engine_state, overlay_state


# ── Main ──────────────────────────────────────────────────────────────────────

def main() -> None:
    logger.info("=" * 60)
    logger.info("Weather ACAP starting up")
    logger.info("=" * 60)

    # ── 1. Load initial config ────────────────────────────────────────────────
    cfg = Config.load()
    # Re-apply log level from config (may differ from env var default)
    logging.getLogger().setLevel(getattr(logging, cfg.log_level, logging.INFO))
    logger.info("Configuration: %r", cfg)

    if cfg.mock_mode:
        logger.warning(
            "[MOCK MODE ENABLED] Weather data will be read from mock_weather.json. "
            "VAPIX calls will still fire against the real device."
        )

    if not cfg.zip_code and (cfg.lat_override is None or cfg.lon_override is None):
        logger.warning(
            "No ZIP code or coordinate override configured. "
            "Open http://<device>:8080 and set a ZIP code before the first poll."
        )

    # ── 2. Build VAPIX client ─────────────────────────────────────────────────
    vapix = VapixClient(
        base_url=cfg.vapix_base_url,
        user=cfg.vapix_user,
        password=cfg.vapix_pass,
    )

    # ── 3. Probe device capabilities ─────────────────────────────────────────
    logger.info("Probing device capabilities via VAPIX...")
    caps = probe(vapix)
    if not caps.probe_successful:
        logger.warning(
            "Device probe failed — VAPIX may not be reachable yet. "
            "Using conservative defaults. Will retry on next startup."
        )
        caps = UNKNOWN_CAPABILITIES

    warn_if_port_range_exceeds(caps, cfg)
    logger.info("Device: %s", caps)

    # ── 4. Signal handlers ────────────────────────────────────────────────────
    signal.signal(signal.SIGTERM, _handle_signal)
    signal.signal(signal.SIGINT, _handle_signal)

    # ── 5. Start Flask config UI (daemon thread) ──────────────────────────────
    ui_thread = threading.Thread(
        target=run_web_ui,
        kwargs={"host": "0.0.0.0", "port": 8080},
        daemon=True,
        name="web-ui",
    )
    ui_thread.start()
    logger.info("Config UI available at http://0.0.0.0:8080")

    # ── 6. Build NWS client ───────────────────────────────────────────────────
    nws = NWSClient(user_agent=cfg.nws_user_agent)

    # ── 7. Poll loop ──────────────────────────────────────────────────────────
    engine_state = EngineState()
    overlay_state = OverlayState()

    logger.info("Starting poll loop (interval: %ds)", cfg.poll_interval)

    while not _shutdown.is_set:
        # Reload config each cycle so UI changes take effect without restart.
        cfg = Config.load()
        logging.getLogger().setLevel(getattr(logging, cfg.log_level, logging.INFO))

        engine_state, overlay_state = _poll_cycle(
            cfg, nws, vapix, engine_state, overlay_state, caps.has_video
        )
        _touch_heartbeat()

        # Sleep in short intervals so SIGTERM is handled promptly.
        logger.debug("Next poll in %ds.", cfg.poll_interval)
        _shutdown.wait(timeout=float(cfg.poll_interval))

    # ── 8. Graceful shutdown ──────────────────────────────────────────────────
    logger.info("Shutdown requested — cleaning up...")

    # Deactivate all active virtual ports
    for port in sorted(engine_state.active_ports):
        try:
            vapix.deactivate_virtual_port(port)
        except Exception as exc:
            logger.warning("Could not deactivate port %d on shutdown: %s", port, exc)

    # Delete the overlay
    if caps.has_video:
        cleanup_overlay(vapix, overlay_state)

    vapix.__exit__(None, None, None)
    logger.info("Weather ACAP shut down cleanly.")


if __name__ == "__main__":
    main()
