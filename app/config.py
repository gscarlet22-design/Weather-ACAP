"""
config.py — Single source of truth for all Weather ACAP settings.

Priority order (highest → lowest):
  1. Environment variables  (set in manifest runOptions or docker run -e)
  2. config_store.json      (written by the Flask web UI)
  3. config_defaults.json   (factory defaults shipped with the app)

This module never raises on missing values — it always falls back gracefully.
"""

import json
import logging
import os
from pathlib import Path
from typing import Any

logger = logging.getLogger(__name__)

# ── File paths ────────────────────────────────────────────────────────────────
_APP_DIR = Path(__file__).parent
_DEFAULTS_FILE = _APP_DIR / "config_defaults.json"
_STORE_FILE = _APP_DIR / "config_store.json"


# ── Internal helpers ──────────────────────────────────────────────────────────

def _load_json(path: Path) -> dict:
    """Load a JSON file; return {} on any error."""
    try:
        with open(path, "r", encoding="utf-8") as fh:
            return json.load(fh)
    except FileNotFoundError:
        return {}
    except json.JSONDecodeError as exc:
        logger.warning("JSON parse error in %s: %s", path, exc)
        return {}


def _env_str(key: str, fallback: Any = None) -> Any:
    """Return stripped env var string, or fallback if unset/empty."""
    val = os.environ.get(key, "").strip()
    return val if val else fallback


def _env_bool(key: str, fallback: bool) -> bool:
    val = os.environ.get(key, "").strip().lower()
    if val in ("1", "true", "yes"):
        return True
    if val in ("0", "false", "no"):
        return False
    return fallback


def _env_int(key: str, fallback: int) -> int:
    val = os.environ.get(key, "").strip()
    if val:
        try:
            return int(val)
        except ValueError:
            logger.warning("Invalid integer for env var %s=%r, using %d", key, val, fallback)
    return fallback


def _env_float(key: str, fallback: Any) -> Any:
    val = os.environ.get(key, "").strip()
    if val:
        try:
            return float(val)
        except ValueError:
            logger.warning("Invalid float for env var %s=%r", key, val)
    return fallback


# ── Public API ────────────────────────────────────────────────────────────────

class Config:
    """
    Immutable-ish config snapshot.  Call Config.load() to get a fresh instance.
    Re-load after the web UI saves to pick up changes without restarting.
    """

    def __init__(self, data: dict):
        self._data = data

    # ── VAPIX / device ────────────────────────────────────────────────────────
    @property
    def vapix_host(self) -> str:
        return self._data["vapix_host"]

    @property
    def vapix_user(self) -> str:
        return self._data["vapix_user"]

    @property
    def vapix_pass(self) -> str:
        return self._data["vapix_pass"]

    @property
    def vapix_base_url(self) -> str:
        host = self.vapix_host
        if host.startswith("http"):
            return host.rstrip("/")
        return f"http://{host}"

    # ── Location ──────────────────────────────────────────────────────────────
    @property
    def zip_code(self) -> str:
        return self._data.get("zip_code", "")

    @property
    def lat_override(self):
        """float | None"""
        return self._data.get("lat_override")

    @property
    def lon_override(self):
        """float | None"""
        return self._data.get("lon_override")

    # ── Alerts ────────────────────────────────────────────────────────────────
    @property
    def enabled_alert_types(self) -> list[str]:
        return self._data.get("enabled_alert_types", [])

    @property
    def all_alert_types(self) -> list[str]:
        """Full list of selectable alert types shown in the UI."""
        return self._data.get("alert_types", [])

    @property
    def virtual_port_start(self) -> int:
        return self._data.get("virtual_port_start", 20)

    # ── Overlay ───────────────────────────────────────────────────────────────
    @property
    def overlay_enabled(self) -> bool:
        return self._data.get("overlay_enabled", True)

    @property
    def overlay_position(self) -> str:
        return self._data.get("overlay_position", "topLeft")

    @property
    def overlay_max_chars(self) -> int:
        return self._data.get("overlay_max_chars", 128)

    # ── Polling ───────────────────────────────────────────────────────────────
    @property
    def poll_interval(self) -> int:
        raw = self._data.get("poll_interval_seconds", 300)
        # Safety floor — NWS asks for no more frequent than once per minute.
        return max(60, int(raw))

    # ── Mode ─────────────────────────────────────────────────────────────────
    @property
    def mock_mode(self) -> bool:
        return self._data.get("mock_mode", False)

    @property
    def log_level(self) -> str:
        return self._data.get("log_level", "INFO").upper()

    @property
    def nws_user_agent(self) -> str:
        return self._data.get("nws_user_agent", "WeatherACAP/1.0 (your-email@example.com)")

    # ── Serialisation ─────────────────────────────────────────────────────────
    def as_dict(self) -> dict:
        """Return a copy of the resolved config (without credentials)."""
        safe = dict(self._data)
        safe.pop("vapix_pass", None)
        return safe

    def __repr__(self) -> str:
        return (
            f"<Config zip={self.zip_code!r} mock={self.mock_mode} "
            f"alerts={self.enabled_alert_types} port_start={self.virtual_port_start}>"
        )

    # ── Factory ───────────────────────────────────────────────────────────────
    @classmethod
    def load(cls) -> "Config":
        """
        Build a Config by merging defaults → stored → env vars.
        This is cheap to call repeatedly; the only I/O is reading two small files.
        """
        defaults = _load_json(_DEFAULTS_FILE)
        stored = _load_json(_STORE_FILE)

        # Layer 1 + 2: merge stored on top of defaults
        merged: dict = {**defaults, **stored}

        # Layer 3: environment variable overrides
        # VAPIX connection
        merged["vapix_host"] = _env_str("VAPIX_HOST", merged.get("vapix_host", "localhost"))
        merged["vapix_user"] = _env_str("VAPIX_USER", merged.get("vapix_user", "root"))
        merged["vapix_pass"] = _env_str("VAPIX_PASS", merged.get("vapix_pass", ""))

        # Location
        merged["zip_code"] = _env_str("ZIP_CODE", merged.get("zip_code", ""))
        lat = _env_float("LAT_OVERRIDE", merged.get("lat_override"))
        lon = _env_float("LON_OVERRIDE", merged.get("lon_override"))
        merged["lat_override"] = lat
        merged["lon_override"] = lon

        # Behaviour
        merged["poll_interval_seconds"] = _env_int(
            "POLL_INTERVAL_SECONDS", merged.get("poll_interval_seconds", 300)
        )
        merged["virtual_port_start"] = _env_int(
            "VIRTUAL_PORT_START", merged.get("virtual_port_start", 20)
        )
        merged["mock_mode"] = _env_bool("MOCK_MODE", merged.get("mock_mode", False))
        merged["log_level"] = _env_str("LOG_LEVEL", merged.get("log_level", "INFO"))
        merged["nws_user_agent"] = _env_str(
            "NWS_USER_AGENT", merged.get("nws_user_agent", "WeatherACAP/1.0")
        )
        merged["overlay_enabled"] = _env_bool(
            "OVERLAY_ENABLED", merged.get("overlay_enabled", True)
        )

        cfg = cls(merged)
        logger.debug("Config loaded: %r", cfg)
        return cfg


def save_config_store(updates: dict) -> None:
    """
    Merge *updates* into config_store.json and write it atomically.
    Called by the Flask web UI on POST /save.
    """
    existing = _load_json(_STORE_FILE)
    existing.update(updates)

    # Validate types before writing
    if "poll_interval_seconds" in existing:
        try:
            existing["poll_interval_seconds"] = max(60, int(existing["poll_interval_seconds"]))
        except (ValueError, TypeError):
            existing["poll_interval_seconds"] = 300

    if "virtual_port_start" in existing:
        try:
            existing["virtual_port_start"] = int(existing["virtual_port_start"])
        except (ValueError, TypeError):
            existing["virtual_port_start"] = 20

    tmp = _STORE_FILE.with_suffix(".tmp")
    try:
        with open(tmp, "w", encoding="utf-8") as fh:
            json.dump(existing, fh, indent=2)
        tmp.replace(_STORE_FILE)
        logger.info("config_store.json updated: %s", list(updates.keys()))
    except OSError as exc:
        logger.error("Failed to write config_store.json: %s", exc)
        raise
