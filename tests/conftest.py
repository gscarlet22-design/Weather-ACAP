"""
conftest.py — Shared pytest fixtures for Weather ACAP test suite.

All HTTP calls in tests are intercepted by the `responses` library so no
actual network traffic is generated.  Fixtures are designed to be composable:
tests can override individual pieces by requesting more specific fixtures.
"""

import json
import os
import sys
from pathlib import Path
from unittest.mock import MagicMock

import pytest

# ── Path setup ────────────────────────────────────────────────────────────────
# Add app/ to sys.path so tests can import application modules directly.
_APP_DIR = Path(__file__).parent.parent / "app"
sys.path.insert(0, str(_APP_DIR))


# ── Config fixture ────────────────────────────────────────────────────────────

@pytest.fixture
def default_config():
    """
    A Config-like object with safe test defaults.
    Does NOT read from disk or env vars — values are hardcoded here.
    """

    class _Config:
        vapix_host = "testdevice"
        vapix_user = "root"
        vapix_pass = "testpass"
        vapix_base_url = "http://testdevice"
        zip_code = "73301"
        lat_override = None
        lon_override = None
        enabled_alert_types = [
            "Tornado Warning",
            "Severe Thunderstorm Warning",
            "Flash Flood Warning",
        ]
        all_alert_types = [
            "Tornado Warning",
            "Tornado Watch",
            "Severe Thunderstorm Warning",
            "Severe Thunderstorm Watch",
            "Flash Flood Warning",
            "Flash Flood Watch",
            "Flash Flood Statement",
            "Winter Storm Warning",
        ]
        virtual_port_start = 20
        overlay_enabled = True
        overlay_position = "topLeft"
        overlay_max_chars = 128
        poll_interval = 300
        mock_mode = False
        log_level = "DEBUG"
        nws_user_agent = "WeatherACAP-Test/1.0 (test@example.com)"

    return _Config()


@pytest.fixture
def mock_mode_config(default_config):
    """Config with mock_mode=True."""
    default_config.mock_mode = True
    return default_config


@pytest.fixture
def no_zip_config(default_config):
    """Config with no ZIP code and no coordinate override."""
    default_config.zip_code = ""
    default_config.lat_override = None
    default_config.lon_override = None
    return default_config


@pytest.fixture
def coord_override_config(default_config):
    """Config with lat/lon override set instead of ZIP."""
    default_config.zip_code = ""
    default_config.lat_override = 30.2672
    default_config.lon_override = -97.7431
    return default_config


# ── NWS API response fixtures ─────────────────────────────────────────────────

@pytest.fixture
def nws_points_response():
    """Realistic /points API response for Austin, TX."""
    return {
        "properties": {
            "gridId": "EWX",
            "gridX": 156,
            "gridY": 91,
            "observationStations": "https://api.weather.gov/gridpoints/EWX/156,91/stations",
            "relativeLocation": {
                "properties": {"city": "Austin", "state": "TX"}
            },
            "forecast": "https://api.weather.gov/gridpoints/EWX/156,91/forecast",
        }
    }


@pytest.fixture
def nws_stations_response():
    """Response from the observation stations endpoint."""
    return {
        "features": [
            {
                "properties": {
                    "stationIdentifier": "KAUS",
                    "name": "Austin-Bergstrom International Airport",
                }
            },
            {
                "properties": {
                    "stationIdentifier": "KEDC",
                    "name": "Austin Executive Airport",
                }
            },
        ]
    }


@pytest.fixture
def nws_observation_response():
    """Realistic /stations/KAUS/observations/latest response."""
    return {
        "properties": {
            "timestamp": "2026-04-05T18:45:00+00:00",
            "textDescription": "Mostly Cloudy",
            "temperature": {"value": 32.8, "unitCode": "wmoUnit:degC"},
            "dewpoint": {"value": 25.6, "unitCode": "wmoUnit:degC"},
            "windDirection": {"value": 202, "unitCode": "wmoUnit:degree_(angle)"},
            "windSpeed": {"value": 9.84, "unitCode": "wmoUnit:km_h-1"},
            "relativeHumidity": {"value": 68.0, "unitCode": "wmoUnit:percent"},
            "barometricPressure": {"value": 100330, "unitCode": "wmoUnit:Pa"},
        }
    }


@pytest.fixture
def nws_alerts_response_active():
    """Alerts response with a Tornado Warning and Flash Flood Warning active."""
    return {
        "features": [
            {
                "properties": {
                    "id": "urn:oid:2.49.0.1.840.0.test-tornado-001",
                    "event": "Tornado Warning",
                    "headline": "Tornado Warning for Travis County",
                    "description": "A tornado has been spotted near Pflugerville.",
                    "instruction": "Take cover now.",
                    "severity": "Extreme",
                    "certainty": "Observed",
                    "urgency": "Immediate",
                    "status": "Actual",
                    "effective": "2026-04-05T18:44:00-05:00",
                    "expires": "2026-04-05T19:30:00-05:00",
                    "ends": "2026-04-05T19:30:00-05:00",
                    "areaDesc": "Central Travis",
                    "senderName": "NWS Austin/San Antonio TX",
                }
            },
            {
                "properties": {
                    "id": "urn:oid:2.49.0.1.840.0.test-flash-001",
                    "event": "Flash Flood Warning",
                    "headline": "Flash Flood Warning for Barton Creek",
                    "description": "Heavy rainfall causing flooding.",
                    "instruction": "Move to higher ground.",
                    "severity": "Severe",
                    "certainty": "Likely",
                    "urgency": "Immediate",
                    "status": "Actual",
                    "effective": "2026-04-05T18:15:00-05:00",
                    "expires": "2026-04-05T21:00:00-05:00",
                    "ends": "2026-04-05T21:00:00-05:00",
                    "areaDesc": "Travis",
                    "senderName": "NWS Austin/San Antonio TX",
                }
            },
        ]
    }


@pytest.fixture
def nws_alerts_response_empty():
    """Alerts response with no active alerts."""
    return {"features": []}


@pytest.fixture
def census_geocoder_response():
    """Census Geocoder response for ZIP 73301 (Austin, TX)."""
    return {
        "result": {
            "addressMatches": [
                {
                    "coordinates": {"x": -97.7431, "y": 30.2672},
                    "matchedAddress": "AUSTIN, TX, 73301",
                }
            ]
        }
    }


@pytest.fixture
def census_geocoder_empty():
    """Census Geocoder response for an unknown ZIP."""
    return {"result": {"addressMatches": []}}


# ── VAPIX mock session ────────────────────────────────────────────────────────

@pytest.fixture
def mock_vapix_client():
    """
    A MagicMock VapixClient.
    All methods return success defaults unless overridden in the test.
    """
    client = MagicMock()
    client.activate_virtual_port.return_value = True
    client.deactivate_virtual_port.return_value = True
    client.create_overlay.return_value = "test-overlay-id"
    client.update_overlay.return_value = True
    client.delete_overlay.return_value = True
    client.list_overlays.return_value = []
    client.get_virtual_port_list.return_value = list(range(1, 33))
    client.get_basic_device_info.return_value = {
        "Model": "AXIS P3245-V",
        "Version": "11.8.62",
        "SerialNumber": "ACCC8ETEST01",
    }
    client.list_params.return_value = "root.Properties.Image.Resolution=1920x1080"
    return client


# ── Mock weather data fixture ─────────────────────────────────────────────────

@pytest.fixture
def mock_weather_file(tmp_path):
    """
    Write a copy of mock_weather.json to a temp directory and return its path.
    Useful for tests that exercise mock mode file loading.
    """
    src = _APP_DIR / "mock_weather.json"
    dest = tmp_path / "mock_weather.json"
    dest.write_text(src.read_text(encoding="utf-8"), encoding="utf-8")
    return dest
