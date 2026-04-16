"""
overlay_manager.py — Build and push the dynamic text overlay to the Axis device.

Responsibilities:
  - Compose a concise overlay string from current conditions + active alerts.
  - Truncate to the configured max character limit (default 128).
  - Create the overlay on first call; update it in-place on subsequent calls.
  - No-op silently if the device has no video (has_video=False) or overlay is
    disabled in config — so the same code path works on cameras and speakers.
  - Delete the overlay on shutdown (called from weather_acap.py signal handler).

Overlay format:
  With alerts:    "[TORNADO WARNING | FLASH FLOOD WARNING] Temp: 91°F | Mostly Cloudy | Wind: 22mph SSW | Humidity: 68%"
  Without alerts: "Temp: 72°F | Partly Cloudy | Wind: 12mph NW | Humidity: 55%"
"""

import logging
from dataclasses import dataclass
from typing import Optional

from vapix_client import VapixClient
from nws_client import WeatherData

logger = logging.getLogger(__name__)

# Marker baked into the overlay's identity so we can find it on restart.
_OVERLAY_MARKER = "weather_acap"


@dataclass
class OverlayState:
    """Tracks the ID of the overlay we own on the device."""
    overlay_id: Optional[str] = None


def _build_text(weather: WeatherData, cfg, enabled_alert_types: list[str]) -> str:
    """
    Build the overlay text string from weather data.
    Caps at cfg.overlay_max_chars characters.
    """
    cond = weather.conditions

    # Alerts section — only include alerts whose type is in enabled_alert_types
    enabled_lower = {t.lower() for t in enabled_alert_types}
    matching_alerts = [
        a for a in weather.alerts
        if a.event.lower() in enabled_lower
    ]

    parts = []
    if matching_alerts:
        names = " | ".join(a.event.upper() for a in matching_alerts)
        parts.append(f"[{names}]")

    parts.append(cond.summary())

    text = "  ".join(parts)

    max_chars = cfg.overlay_max_chars
    if len(text) > max_chars:
        text = text[: max_chars - 1] + "…"

    return text


def _find_existing_overlay(vapix: VapixClient) -> Optional[str]:
    """
    Search the device's overlay list for one we previously created.
    We identify ours by checking if the overlay's description/id contains
    our _OVERLAY_MARKER string.  Returns the overlay ID or None.
    """
    try:
        overlays = vapix.list_overlays()
        for item in overlays:
            item_id = str(item.get("id") or item.get("overlayId") or "")
            if _OVERLAY_MARKER in item_id.lower():
                logger.info("Found existing overlay: id=%r", item_id)
                return item_id
    except Exception as exc:
        logger.debug("Could not enumerate overlays: %s", exc)
    return None


def update(
    weather: WeatherData,
    cfg,
    vapix: VapixClient,
    state: OverlayState,
    has_video: bool,
) -> OverlayState:
    """
    Main entry point.  Create or update the overlay on the device.

    Returns a (possibly updated) OverlayState.
    Never raises — all errors are logged and the state is returned as-is.
    """
    if not has_video:
        logger.debug("Overlay skipped — device has no video capability.")
        return state

    if not cfg.overlay_enabled:
        logger.debug("Overlay disabled in config — skipping.")
        return state

    text = _build_text(weather, cfg, cfg.enabled_alert_types)
    logger.debug("Overlay text (%d chars): %r", len(text), text)

    new_state = OverlayState(overlay_id=state.overlay_id)

    # ── If we don't have an overlay ID yet, try to find one already on device ─
    if not new_state.overlay_id:
        new_state.overlay_id = _find_existing_overlay(vapix)

    # ── Update existing overlay ───────────────────────────────────────────────
    if new_state.overlay_id:
        success = vapix.update_overlay(new_state.overlay_id, text)
        if success:
            return new_state
        # The overlay may have been deleted externally — fall through to create.
        logger.warning(
            "Failed to update overlay %r — will attempt to re-create it.",
            new_state.overlay_id,
        )
        new_state.overlay_id = None

    # ── Create new overlay ────────────────────────────────────────────────────
    overlay_id = vapix.create_overlay(text, position=cfg.overlay_position)
    if overlay_id:
        new_state.overlay_id = overlay_id
        logger.info("Overlay created successfully: id=%r", overlay_id)
    else:
        logger.warning("Could not create overlay — no overlay ID returned by device.")

    return new_state


def cleanup(vapix: VapixClient, state: OverlayState) -> None:
    """
    Delete the overlay from the device.  Called on graceful shutdown.
    Silently ignores errors.
    """
    if state.overlay_id:
        try:
            vapix.delete_overlay(state.overlay_id)
        except Exception as exc:
            logger.debug("Could not delete overlay on shutdown: %s", exc)
