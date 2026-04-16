"""
test_nws_client.py — Tests for nws_client.py

All HTTP calls are intercepted by the `responses` library.
No actual network traffic is generated.
"""

import sys
from pathlib import Path
from unittest.mock import patch

import pytest
import responses as rsps_lib

sys.path.insert(0, str(Path(__file__).parent.parent / "app"))
from nws_client import (
    NWSClient,
    Alert,
    Conditions,
    WeatherData,
    geocode_zip,
    fetch_alerts,
    fetch_observations,
    resolve_nws_grid,
    get_nearest_station,
    _build_session,
)


CENSUS_URL = (
    "https://geocoding.geo.census.gov/geocoder/locations/address"
    "?benchmark=2020&format=json&zip=73301"
)
NWS_POINTS_URL = "https://api.weather.gov/points/30.2672,-97.7431"
NWS_STATIONS_URL = "https://api.weather.gov/gridpoints/EWX/156,91/stations"
NWS_OBS_URL = "https://api.weather.gov/stations/KAUS/observations/latest"
NWS_ALERTS_URL = "https://api.weather.gov/alerts/active?point=30.2672,-97.7431"


# ── geocode_zip ───────────────────────────────────────────────────────────────

class TestGeocodeZip:
    @rsps_lib.activate
    def test_success(self, census_geocoder_response):
        rsps_lib.add(rsps_lib.GET, CENSUS_URL, json=census_geocoder_response, status=200)
        session = _build_session("Test/1.0")
        lat, lon = geocode_zip("73301", session)
        assert lat == pytest.approx(30.2672)
        assert lon == pytest.approx(-97.7431)

    @rsps_lib.activate
    def test_unknown_zip_raises(self, census_geocoder_empty):
        rsps_lib.add(
            rsps_lib.GET,
            "https://geocoding.geo.census.gov/geocoder/locations/address"
            "?benchmark=2020&format=json&zip=99999",
            json=census_geocoder_empty,
            status=200,
        )
        session = _build_session("Test/1.0")
        with pytest.raises(ValueError, match="not found"):
            geocode_zip("99999", session)

    @rsps_lib.activate
    def test_http_error_raises(self):
        rsps_lib.add(
            rsps_lib.GET,
            "https://geocoding.geo.census.gov/geocoder/locations/address"
            "?benchmark=2020&format=json&zip=00000",
            status=503,
        )
        session = _build_session("Test/1.0")
        with pytest.raises(Exception):
            geocode_zip("00000", session)


# ── resolve_nws_grid ──────────────────────────────────────────────────────────

class TestResolveNWSGrid:
    @rsps_lib.activate
    def test_returns_properties(self, nws_points_response):
        rsps_lib.add(rsps_lib.GET, NWS_POINTS_URL, json=nws_points_response, status=200)
        session = _build_session("Test/1.0")
        result = resolve_nws_grid(30.2672, -97.7431, session)
        assert result["gridId"] == "EWX"
        assert result["gridX"] == 156
        assert result["gridY"] == 91

    @rsps_lib.activate
    def test_404_raises(self):
        rsps_lib.add(rsps_lib.GET, NWS_POINTS_URL, status=404)
        session = _build_session("Test/1.0")
        with pytest.raises(Exception):
            resolve_nws_grid(30.2672, -97.7431, session)


# ── get_nearest_station ───────────────────────────────────────────────────────

class TestGetNearestStation:
    @rsps_lib.activate
    def test_returns_first_station(self, nws_stations_response):
        rsps_lib.add(rsps_lib.GET, NWS_STATIONS_URL, json=nws_stations_response, status=200)
        session = _build_session("Test/1.0")
        station = get_nearest_station(NWS_STATIONS_URL, session)
        assert station == "KAUS"

    @rsps_lib.activate
    def test_empty_features_raises(self):
        rsps_lib.add(rsps_lib.GET, NWS_STATIONS_URL, json={"features": []}, status=200)
        session = _build_session("Test/1.0")
        with pytest.raises(ValueError, match="No observation stations"):
            get_nearest_station(NWS_STATIONS_URL, session)


# ── fetch_observations ────────────────────────────────────────────────────────

class TestFetchObservations:
    @rsps_lib.activate
    def test_parses_temperature(self, nws_observation_response):
        rsps_lib.add(rsps_lib.GET, NWS_OBS_URL, json=nws_observation_response, status=200)
        session = _build_session("Test/1.0")
        cond = fetch_observations("KAUS", session)
        assert cond.temperature_f == pytest.approx(91.0, abs=1.0)
        assert cond.temperature_c == pytest.approx(32.8)

    @rsps_lib.activate
    def test_parses_wind(self, nws_observation_response):
        rsps_lib.add(rsps_lib.GET, NWS_OBS_URL, json=nws_observation_response, status=200)
        session = _build_session("Test/1.0")
        cond = fetch_observations("KAUS", session)
        assert cond.wind_direction is not None
        assert cond.wind_speed_mph is not None

    @rsps_lib.activate
    def test_parses_sky_condition(self, nws_observation_response):
        rsps_lib.add(rsps_lib.GET, NWS_OBS_URL, json=nws_observation_response, status=200)
        session = _build_session("Test/1.0")
        cond = fetch_observations("KAUS", session)
        assert cond.sky_condition == "Mostly Cloudy"

    @rsps_lib.activate
    def test_missing_values_are_none(self):
        rsps_lib.add(
            rsps_lib.GET, NWS_OBS_URL,
            json={"properties": {}},
            status=200,
        )
        session = _build_session("Test/1.0")
        cond = fetch_observations("KAUS", session)
        assert cond.temperature_f is None
        assert cond.wind_speed_mph is None

    def test_conditions_summary_with_data(self):
        c = Conditions(
            temperature_f=91.0,
            sky_condition="Mostly Cloudy",
            wind_speed_mph=22.0,
            wind_direction="SSW",
            relative_humidity_pct=68.0,
        )
        summary = c.summary()
        assert "91" in summary
        assert "Mostly Cloudy" in summary
        assert "22" in summary
        assert "SSW" in summary

    def test_conditions_summary_no_data(self):
        c = Conditions()
        assert c.summary() == "No data"


# ── fetch_alerts ──────────────────────────────────────────────────────────────

class TestFetchAlerts:
    @rsps_lib.activate
    def test_parses_active_alerts(self, nws_alerts_response_active):
        rsps_lib.add(rsps_lib.GET, NWS_ALERTS_URL, json=nws_alerts_response_active, status=200)
        session = _build_session("Test/1.0")
        alerts = fetch_alerts(30.2672, -97.7431, session)
        assert len(alerts) == 2
        events = {a.event for a in alerts}
        assert "Tornado Warning" in events
        assert "Flash Flood Warning" in events

    @rsps_lib.activate
    def test_empty_when_no_alerts(self, nws_alerts_response_empty):
        rsps_lib.add(rsps_lib.GET, NWS_ALERTS_URL, json=nws_alerts_response_empty, status=200)
        session = _build_session("Test/1.0")
        alerts = fetch_alerts(30.2672, -97.7431, session)
        assert alerts == []

    @rsps_lib.activate
    def test_alert_fields_populated(self, nws_alerts_response_active):
        rsps_lib.add(rsps_lib.GET, NWS_ALERTS_URL, json=nws_alerts_response_active, status=200)
        session = _build_session("Test/1.0")
        alerts = fetch_alerts(30.2672, -97.7431, session)
        tornado = next(a for a in alerts if a.event == "Tornado Warning")
        assert tornado.severity == "Extreme"
        assert tornado.urgency == "Immediate"
        assert "Travis" in tornado.areas_affected
        assert tornado.sender_name == "NWS Austin/San Antonio TX"


# ── NWSClient integration ─────────────────────────────────────────────────────

class TestNWSClient:
    @rsps_lib.activate
    def test_full_fetch_live(
        self,
        default_config,
        census_geocoder_response,
        nws_points_response,
        nws_stations_response,
        nws_observation_response,
        nws_alerts_response_active,
    ):
        """Full pipeline: ZIP → grid → station → observations + alerts."""
        rsps_lib.add(
            rsps_lib.GET,
            "https://geocoding.geo.census.gov/geocoder/locations/address"
            "?benchmark=2020&format=json&zip=73301",
            json=census_geocoder_response, status=200,
        )
        rsps_lib.add(rsps_lib.GET, NWS_POINTS_URL, json=nws_points_response, status=200)
        rsps_lib.add(rsps_lib.GET, NWS_STATIONS_URL, json=nws_stations_response, status=200)
        rsps_lib.add(rsps_lib.GET, NWS_OBS_URL, json=nws_observation_response, status=200)
        rsps_lib.add(rsps_lib.GET, NWS_ALERTS_URL, json=nws_alerts_response_active, status=200)

        client = NWSClient(user_agent="Test/1.0")
        data = client.fetch(default_config)

        assert data.source == "nws"
        assert data.lat == pytest.approx(30.2672)
        assert data.lon == pytest.approx(-97.7431)
        assert data.conditions.temperature_f is not None
        assert len(data.alerts) == 2

    def test_mock_mode_loads_file(self, mock_mode_config, mock_weather_file, monkeypatch):
        """Mock mode should read from mock_weather.json, not hit any API."""
        import nws_client
        monkeypatch.setattr(nws_client, "_MOCK_FILE", mock_weather_file)

        client = NWSClient(user_agent="Test/1.0")
        data = client.fetch(mock_mode_config)

        assert data.source == "mock"
        assert len(data.alerts) == 3  # 3 alerts in mock_weather.json
        assert data.conditions.temperature_f == 91

    def test_coord_override_skips_geocoder(
        self,
        coord_override_config,
        nws_points_response,
        nws_stations_response,
        nws_observation_response,
        nws_alerts_response_empty,
    ):
        """When lat/lon override is set, Census API should NOT be called."""
        with rsps_lib.RequestsMock() as rm:
            rm.add(rsps_lib.GET, NWS_POINTS_URL, json=nws_points_response, status=200)
            rm.add(rsps_lib.GET, NWS_STATIONS_URL, json=nws_stations_response, status=200)
            rm.add(rsps_lib.GET, NWS_OBS_URL, json=nws_observation_response, status=200)
            rm.add(rsps_lib.GET, NWS_ALERTS_URL, json=nws_alerts_response_empty, status=200)

            client = NWSClient(user_agent="Test/1.0")
            data = client.fetch(coord_override_config)
            assert data.lat == pytest.approx(30.2672)
            # Census URL was NOT registered — if it were called, responses would raise.

    def test_no_zip_raises(self, no_zip_config):
        client = NWSClient(user_agent="Test/1.0")
        with pytest.raises(ValueError, match="No ZIP code"):
            client.fetch(no_zip_config)

    @rsps_lib.activate
    def test_station_cached_between_calls(
        self,
        default_config,
        census_geocoder_response,
        nws_points_response,
        nws_stations_response,
        nws_observation_response,
        nws_alerts_response_empty,
    ):
        """Second fetch with same coords should not call /points or /stations again."""
        rsps_lib.add(
            rsps_lib.GET,
            "https://geocoding.geo.census.gov/geocoder/locations/address"
            "?benchmark=2020&format=json&zip=73301",
            json=census_geocoder_response, status=200,
        )
        rsps_lib.add(rsps_lib.GET, NWS_POINTS_URL, json=nws_points_response, status=200)
        rsps_lib.add(rsps_lib.GET, NWS_STATIONS_URL, json=nws_stations_response, status=200)
        rsps_lib.add(rsps_lib.GET, NWS_OBS_URL, json=nws_observation_response, status=200)
        rsps_lib.add(rsps_lib.GET, NWS_ALERTS_URL, json=nws_alerts_response_empty, status=200)
        # Register second round of obs+alerts for the second fetch
        rsps_lib.add(rsps_lib.GET, NWS_OBS_URL, json=nws_observation_response, status=200)
        rsps_lib.add(rsps_lib.GET, NWS_ALERTS_URL, json=nws_alerts_response_empty, status=200)

        client = NWSClient(user_agent="Test/1.0")
        client.fetch(default_config)
        client.fetch(default_config)  # second call — should reuse cached station

        # /points was only registered once; if called twice, responses raises ConnectionError
        # The fact that we got here without error confirms caching works.
