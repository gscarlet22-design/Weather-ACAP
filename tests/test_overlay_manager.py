"""
test_overlay_manager.py — Tests for overlay_manager.py

Covers overlay text building, create/update/cleanup lifecycle,
graceful no-op when device has no video, and text truncation.
"""

import sys
from pathlib import Path
from unittest.mock import ANY

import pytest

sys.path.insert(0, str(Path(__file__).parent.parent / "app"))
from overlay_manager import (
    OverlayState,
    _build_text,
    _find_existing_overlay,
    update,
    cleanup,
)
from nws_client import Alert, Conditions, WeatherData


# ── Helpers ───────────────────────────────────────────────────────────────────

def _make_alert(event: str) -> Alert:
    return Alert(
        id=f"test-{event}",
        event=event,
        headline=f"{event} issued",
        description="",
        instruction="",
        severity="Severe",
        urgency="Immediate",
        effective="",
        expires="",
        areas_affected="Travis",
        sender_name="NWS",
    )


def _make_weather(alerts=None, temp_f=72.0, sky="Clear", wind_mph=10.0, wind_dir="N", humidity=50.0):
    return WeatherData(
        conditions=Conditions(
            temperature_f=temp_f,
            sky_condition=sky,
            wind_speed_mph=wind_mph,
            wind_direction=wind_dir,
            relative_humidity_pct=humidity,
        ),
        alerts=alerts or [],
        source="mock",
    )


# ── _build_text ───────────────────────────────────────────────────────────────

class TestBuildText:
    ENABLED = ["Tornado Warning", "Severe Thunderstorm Warning", "Flash Flood Warning"]

    def test_no_alerts_conditions_only(self, default_config):
        weather = _make_weather()
        text = _build_text(weather, default_config, self.ENABLED)
        assert "72" in text
        assert "Clear" in text
        assert "[" not in text

    def test_matching_alert_prefixed(self, default_config):
        weather = _make_weather(alerts=[_make_alert("Tornado Warning")])
        text = _build_text(weather, default_config, self.ENABLED)
        assert "[TORNADO WARNING]" in text

    def test_multiple_alerts_in_prefix(self, default_config):
        weather = _make_weather(alerts=[
            _make_alert("Tornado Warning"),
            _make_alert("Flash Flood Warning"),
        ])
        text = _build_text(weather, default_config, self.ENABLED)
        assert "TORNADO WARNING" in text
        assert "FLASH FLOOD WARNING" in text

    def test_unmatched_alert_not_shown(self, default_config):
        weather = _make_weather(alerts=[_make_alert("Dense Fog Advisory")])
        text = _build_text(weather, default_config, self.ENABLED)
        assert "DENSE FOG" not in text
        assert "[" not in text

    def test_text_truncated_to_max_chars(self, default_config):
        default_config.overlay_max_chars = 40
        weather = _make_weather(alerts=[_make_alert("Tornado Warning")])
        text = _build_text(weather, default_config, self.ENABLED)
        assert len(text) <= 40

    def test_truncation_ends_with_ellipsis(self, default_config):
        default_config.overlay_max_chars = 30
        weather = _make_weather(alerts=[_make_alert("Tornado Warning")])
        text = _build_text(weather, default_config, self.ENABLED)
        assert text.endswith("…")

    def test_no_truncation_when_short(self, default_config):
        weather = _make_weather(temp_f=72.0)
        text = _build_text(weather, default_config, self.ENABLED)
        assert "…" not in text


# ── _find_existing_overlay ────────────────────────────────────────────────────

class TestFindExistingOverlay:
    def test_finds_by_marker_in_id(self, mock_vapix_client):
        mock_vapix_client.list_overlays.return_value = [
            {"id": "weather_acap_1", "text": "old text"}
        ]
        result = _find_existing_overlay(mock_vapix_client)
        assert result == "weather_acap_1"

    def test_returns_none_when_no_match(self, mock_vapix_client):
        mock_vapix_client.list_overlays.return_value = [
            {"id": "some-other-overlay", "text": "not ours"}
        ]
        result = _find_existing_overlay(mock_vapix_client)
        assert result is None

    def test_returns_none_on_empty_list(self, mock_vapix_client):
        mock_vapix_client.list_overlays.return_value = []
        assert _find_existing_overlay(mock_vapix_client) is None

    def test_returns_none_on_exception(self, mock_vapix_client):
        mock_vapix_client.list_overlays.side_effect = Exception("VAPIX down")
        assert _find_existing_overlay(mock_vapix_client) is None


# ── update() lifecycle ────────────────────────────────────────────────────────

class TestUpdate:
    def test_no_video_is_noop(self, mock_vapix_client, default_config):
        weather = _make_weather()
        state = update(weather, default_config, mock_vapix_client, OverlayState(), has_video=False)
        mock_vapix_client.create_overlay.assert_not_called()
        mock_vapix_client.update_overlay.assert_not_called()

    def test_overlay_disabled_is_noop(self, mock_vapix_client, default_config):
        default_config.overlay_enabled = False
        weather = _make_weather()
        update(weather, default_config, mock_vapix_client, OverlayState(), has_video=True)
        mock_vapix_client.create_overlay.assert_not_called()

    def test_creates_overlay_when_no_id(self, mock_vapix_client, default_config):
        mock_vapix_client.list_overlays.return_value = []
        mock_vapix_client.create_overlay.return_value = "new-id-123"

        weather = _make_weather()
        state = update(weather, default_config, mock_vapix_client, OverlayState(), has_video=True)

        mock_vapix_client.create_overlay.assert_called_once()
        assert state.overlay_id == "new-id-123"

    def test_updates_existing_overlay(self, mock_vapix_client, default_config):
        mock_vapix_client.update_overlay.return_value = True

        weather = _make_weather()
        prev = OverlayState(overlay_id="existing-id")
        state = update(weather, default_config, mock_vapix_client, prev, has_video=True)

        mock_vapix_client.update_overlay.assert_called_once_with("existing-id", ANY)
        mock_vapix_client.create_overlay.assert_not_called()

    def test_recreates_overlay_when_update_fails(self, mock_vapix_client, default_config):
        mock_vapix_client.update_overlay.return_value = False
        mock_vapix_client.list_overlays.return_value = []
        mock_vapix_client.create_overlay.return_value = "recreated-id"

        weather = _make_weather()
        prev = OverlayState(overlay_id="stale-id")
        state = update(weather, default_config, mock_vapix_client, prev, has_video=True)

        mock_vapix_client.create_overlay.assert_called_once()
        assert state.overlay_id == "recreated-id"

    def test_finds_existing_overlay_on_startup(self, mock_vapix_client, default_config):
        """If no overlay_id is in state but one exists on device, use it."""
        mock_vapix_client.list_overlays.return_value = [
            {"id": "weather_acap_recover", "text": "old text"}
        ]
        mock_vapix_client.update_overlay.return_value = True

        weather = _make_weather()
        state = update(weather, default_config, mock_vapix_client, OverlayState(), has_video=True)

        mock_vapix_client.update_overlay.assert_called_once_with("weather_acap_recover", ANY)


# ── cleanup ───────────────────────────────────────────────────────────────────

class TestCleanup:
    def test_deletes_overlay_on_shutdown(self, mock_vapix_client):
        state = OverlayState(overlay_id="overlay-to-delete")
        cleanup(mock_vapix_client, state)
        mock_vapix_client.delete_overlay.assert_called_once_with("overlay-to-delete")

    def test_noop_when_no_overlay_id(self, mock_vapix_client):
        cleanup(mock_vapix_client, OverlayState(overlay_id=None))
        mock_vapix_client.delete_overlay.assert_not_called()

    def test_exception_does_not_propagate(self, mock_vapix_client):
        mock_vapix_client.delete_overlay.side_effect = Exception("network error")
        # Should not raise
        cleanup(mock_vapix_client, OverlayState(overlay_id="some-id"))
