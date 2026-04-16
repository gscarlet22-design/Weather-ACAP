"""
test_event_engine.py — Tests for event_engine.py

Covers alert matching, port assignment, state transitions
(new alert, cleared alert, unchanged state), and partial matches.
"""

import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).parent.parent / "app"))
from event_engine import (
    EngineState,
    _alert_type_to_port,
    _active_ports_for_alerts,
    run,
)
from nws_client import Alert, Conditions, WeatherData


# ── Helpers ───────────────────────────────────────────────────────────────────

def _make_alert(event: str, severity: str = "Severe") -> Alert:
    return Alert(
        id=f"test-{event.replace(' ', '-').lower()}",
        event=event,
        headline=f"{event} issued",
        description="Test description.",
        instruction="Take cover.",
        severity=severity,
        urgency="Immediate",
        effective="2026-04-05T18:00:00-05:00",
        expires="2026-04-05T20:00:00-05:00",
        areas_affected="Test County",
        sender_name="NWS Test",
    )


def _make_weather(alerts: list[Alert]) -> WeatherData:
    return WeatherData(
        conditions=Conditions(temperature_f=72.0, sky_condition="Clear"),
        alerts=alerts,
        lat=30.2672,
        lon=-97.7431,
        source="mock",
    )


# ── _alert_type_to_port ───────────────────────────────────────────────────────

class TestAlertTypeToPort:
    ENABLED = ["Tornado Warning", "Severe Thunderstorm Warning", "Flash Flood Warning"]

    def test_exact_match_first(self):
        assert _alert_type_to_port("Tornado Warning", self.ENABLED, 20) == 20

    def test_exact_match_second(self):
        assert _alert_type_to_port("Severe Thunderstorm Warning", self.ENABLED, 20) == 21

    def test_exact_match_third(self):
        assert _alert_type_to_port("Flash Flood Warning", self.ENABLED, 20) == 22

    def test_case_insensitive(self):
        assert _alert_type_to_port("tornado warning", self.ENABLED, 20) == 20

    def test_not_configured_returns_none(self):
        assert _alert_type_to_port("Winter Storm Warning", self.ENABLED, 20) is None

    def test_custom_port_start(self):
        assert _alert_type_to_port("Tornado Warning", self.ENABLED, port_start=5) == 5

    def test_empty_enabled_returns_none(self):
        assert _alert_type_to_port("Tornado Warning", [], 20) is None


# ── _active_ports_for_alerts ──────────────────────────────────────────────────

class TestActivePortsForAlerts:
    ENABLED = ["Tornado Warning", "Severe Thunderstorm Warning", "Flash Flood Warning"]

    def test_no_alerts_returns_empty_set(self):
        result = _active_ports_for_alerts([], self.ENABLED, 20)
        assert result == set()

    def test_single_match(self):
        alerts = [_make_alert("Tornado Warning")]
        result = _active_ports_for_alerts(alerts, self.ENABLED, 20)
        assert result == {20}

    def test_multiple_matches(self):
        alerts = [_make_alert("Tornado Warning"), _make_alert("Flash Flood Warning")]
        result = _active_ports_for_alerts(alerts, self.ENABLED, 20)
        assert result == {20, 22}

    def test_unmatched_alert_ignored(self):
        alerts = [_make_alert("Dense Fog Advisory")]
        result = _active_ports_for_alerts(alerts, self.ENABLED, 20)
        assert result == set()

    def test_mixed_matched_and_unmatched(self):
        alerts = [
            _make_alert("Tornado Warning"),
            _make_alert("Dense Fog Advisory"),
        ]
        result = _active_ports_for_alerts(alerts, self.ENABLED, 20)
        assert result == {20}


# ── run() — state transitions ─────────────────────────────────────────────────

class TestRunStateTransitions:
    def test_new_alert_activates_port(self, mock_vapix_client, default_config):
        weather = _make_weather([_make_alert("Tornado Warning")])
        prev = EngineState(active_ports=set())
        new = run(weather, default_config, mock_vapix_client, prev)

        mock_vapix_client.activate_virtual_port.assert_called_once_with(20)
        mock_vapix_client.deactivate_virtual_port.assert_not_called()
        assert 20 in new.active_ports

    def test_cleared_alert_deactivates_port(self, mock_vapix_client, default_config):
        weather = _make_weather([])  # no active alerts
        prev = EngineState(active_ports={20})
        new = run(weather, default_config, mock_vapix_client, prev)

        mock_vapix_client.deactivate_virtual_port.assert_called_once_with(20)
        mock_vapix_client.activate_virtual_port.assert_not_called()
        assert new.active_ports == set()

    def test_unchanged_state_no_vapix_calls(self, mock_vapix_client, default_config):
        weather = _make_weather([_make_alert("Tornado Warning")])
        prev = EngineState(active_ports={20})  # already active
        new = run(weather, default_config, mock_vapix_client, prev)

        mock_vapix_client.activate_virtual_port.assert_not_called()
        mock_vapix_client.deactivate_virtual_port.assert_not_called()
        assert new.active_ports == {20}

    def test_multiple_alerts_activate_multiple_ports(self, mock_vapix_client, default_config):
        weather = _make_weather([
            _make_alert("Tornado Warning"),
            _make_alert("Flash Flood Warning"),
        ])
        prev = EngineState(active_ports=set())
        new = run(weather, default_config, mock_vapix_client, prev)

        assert mock_vapix_client.activate_virtual_port.call_count == 2
        assert new.active_ports == {20, 22}

    def test_alert_swap_activates_and_deactivates(self, mock_vapix_client, default_config):
        """Tornado clears, Flash Flood activates in the same cycle."""
        weather = _make_weather([_make_alert("Flash Flood Warning")])
        prev = EngineState(active_ports={20})  # tornado was active
        new = run(weather, default_config, mock_vapix_client, prev)

        mock_vapix_client.activate_virtual_port.assert_called_once_with(22)
        mock_vapix_client.deactivate_virtual_port.assert_called_once_with(20)
        assert new.active_ports == {22}

    def test_no_enabled_types_no_calls(self, mock_vapix_client, default_config):
        default_config.enabled_alert_types = []
        weather = _make_weather([_make_alert("Tornado Warning")])
        new = run(weather, default_config, mock_vapix_client, EngineState())

        mock_vapix_client.activate_virtual_port.assert_not_called()
        assert new.active_ports == set()

    def test_vapix_error_does_not_crash(self, mock_vapix_client, default_config):
        """VapixError should be caught and logged, not propagated."""
        from vapix_client import VapixError
        mock_vapix_client.activate_virtual_port.side_effect = VapixError("port 20 out of range")

        weather = _make_weather([_make_alert("Tornado Warning")])
        prev = EngineState(active_ports=set())
        # Should not raise
        new = run(weather, default_config, mock_vapix_client, prev)
        # State should reflect what was *desired* even if the call failed
        assert isinstance(new, EngineState)

    def test_returns_new_state_object(self, mock_vapix_client, default_config):
        weather = _make_weather([])
        prev = EngineState(active_ports={20})
        new = run(weather, default_config, mock_vapix_client, prev)
        assert new is not prev
