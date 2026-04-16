"""
nws_client.py — US National Weather Service API client.

Responsibilities:
  1. Convert a US ZIP code → (lat, lon) via the Census Geocoder API.
     If lat_override / lon_override are set in config, skip the geocoder.
  2. Resolve (lat, lon) → NWS grid office + grid (X, Y) + nearest station.
  3. Fetch current observations from the nearest ASOS/AWOS station.
  4. Fetch active weather alerts for the (lat, lon) point.
  5. In mock mode: read everything from mock_weather.json instead.

All external calls use exponential back-off via a Retry-enabled Session.
NWS requires a User-Agent header; missing it causes 403s.
"""

import json
import logging
import time
from dataclasses import dataclass, field
from pathlib import Path
from typing import Optional

import requests
from requests.adapters import HTTPAdapter
from urllib3.util.retry import Retry

logger = logging.getLogger(__name__)

# ── Constants ─────────────────────────────────────────────────────────────────
CENSUS_GEOCODER_URL = (
    "https://geocoding.geo.census.gov/geocoder/locations/address"
    "?benchmark=2020&format=json&zip={zip}"
)
NWS_POINTS_URL = "https://api.weather.gov/points/{lat},{lon}"
NWS_ALERTS_URL = "https://api.weather.gov/alerts/active?point={lat},{lon}"

_MOCK_FILE = Path(__file__).parent / "mock_weather.json"

# ── Data classes ──────────────────────────────────────────────────────────────

@dataclass
class Conditions:
    """Current weather observations."""
    temperature_f: Optional[float] = None
    temperature_c: Optional[float] = None
    relative_humidity_pct: Optional[float] = None
    wind_speed_mph: Optional[float] = None
    wind_direction: Optional[str] = None
    sky_condition: Optional[str] = None
    observed_at: Optional[str] = None
    location_name: Optional[str] = None

    def summary(self) -> str:
        """Human-readable one-liner for the overlay."""
        parts = []
        if self.temperature_f is not None:
            parts.append(f"Temp: {self.temperature_f:.0f}°F")
        if self.sky_condition:
            parts.append(self.sky_condition)
        if self.wind_speed_mph is not None and self.wind_direction:
            parts.append(f"Wind: {self.wind_speed_mph:.0f}mph {self.wind_direction}")
        if self.relative_humidity_pct is not None:
            parts.append(f"Humidity: {self.relative_humidity_pct:.0f}%")
        return " | ".join(parts) if parts else "No data"


@dataclass
class Alert:
    """A single NWS active alert."""
    id: str
    event: str
    headline: str
    description: str
    instruction: str
    severity: str
    urgency: str
    effective: str
    expires: str
    areas_affected: str
    sender_name: str


@dataclass
class WeatherData:
    """Container returned by NWSClient.fetch()."""
    conditions: Conditions
    alerts: list[Alert] = field(default_factory=list)
    lat: Optional[float] = None
    lon: Optional[float] = None
    source: str = "nws"  # "nws" or "mock"


# ── HTTP session with retry ───────────────────────────────────────────────────

def _build_session(user_agent: str) -> requests.Session:
    retry = Retry(
        total=3,
        backoff_factor=2,  # waits 2, 4, 8 seconds between retries
        status_forcelist=[429, 500, 502, 503, 504],
        allowed_methods=["GET"],
    )
    adapter = HTTPAdapter(max_retries=retry)
    session = requests.Session()
    session.mount("https://", adapter)
    session.mount("http://", adapter)
    session.headers.update({"User-Agent": user_agent, "Accept": "application/geo+json"})
    return session


# ── ZIP → coordinates ─────────────────────────────────────────────────────────

def geocode_zip(zip_code: str, session: requests.Session) -> tuple[float, float]:
    """
    Use the US Census Geocoder to convert a ZIP code to (lat, lon).
    Returns the ZIP centroid.  Raises ValueError if the ZIP is not found.
    """
    url = CENSUS_GEOCODER_URL.format(zip=zip_code)
    logger.debug("Geocoding ZIP %s via Census API: %s", zip_code, url)
    resp = session.get(url, timeout=15)
    logger.info("Census Geocoder HTTP %d for ZIP %s", resp.status_code, zip_code)

    resp.raise_for_status()
    data = resp.json()

    matches = data.get("result", {}).get("addressMatches", [])
    if not matches:
        raise ValueError(f"ZIP code {zip_code!r} not found in Census Geocoder")

    coords = matches[0].get("coordinates", {})
    lat = float(coords["y"])
    lon = float(coords["x"])
    logger.info("ZIP %s → lat=%.4f lon=%.4f", zip_code, lat, lon)
    return lat, lon


# ── NWS grid resolution ───────────────────────────────────────────────────────

def resolve_nws_grid(lat: float, lon: float, session: requests.Session) -> dict:
    """
    Call the NWS /points endpoint to get:
      - gridId, gridX, gridY  (for forecast)
      - observationStations URL (to find nearest ASOS station)
      - relativeLocation.properties.city / .state
    """
    url = NWS_POINTS_URL.format(lat=f"{lat:.4f}", lon=f"{lon:.4f}")
    logger.debug("Resolving NWS grid: %s", url)
    resp = session.get(url, timeout=15)
    logger.info("NWS /points HTTP %d for (%.4f, %.4f)", resp.status_code, lat, lon)
    resp.raise_for_status()
    return resp.json().get("properties", {})


def get_nearest_station(observation_stations_url: str, session: requests.Session) -> str:
    """Return the stationIdentifier of the nearest NWS observation station."""
    logger.debug("Fetching observation stations: %s", observation_stations_url)
    resp = session.get(observation_stations_url, timeout=15)
    logger.info("NWS observationStations HTTP %d", resp.status_code)
    resp.raise_for_status()
    features = resp.json().get("features", [])
    if not features:
        raise ValueError("No observation stations found near this location")
    station_id = features[0]["properties"]["stationIdentifier"]
    logger.info("Nearest station: %s", station_id)
    return station_id


# ── Current observations ──────────────────────────────────────────────────────

def _c_to_f(celsius) -> Optional[float]:
    if celsius is None:
        return None
    return round(celsius * 9 / 5 + 32, 1)


def _ms_to_mph(ms) -> Optional[float]:
    if ms is None:
        return None
    return round(ms * 2.23694, 1)


def fetch_observations(station_id: str, session: requests.Session) -> Conditions:
    url = f"https://api.weather.gov/stations/{station_id}/observations/latest"
    logger.debug("Fetching observations from %s", url)
    resp = session.get(url, timeout=15)
    logger.info("NWS observations HTTP %d for station %s", resp.status_code, station_id)
    resp.raise_for_status()

    props = resp.json().get("properties", {})

    temp_c = props.get("temperature", {}).get("value")
    dewpoint_c = props.get("dewpoint", {}).get("value")  # noqa: F841 — available if needed
    wind_ms = props.get("windSpeed", {}).get("value")
    wind_dir_deg = props.get("windDirection", {}).get("value")
    humidity = props.get("relativeHumidity", {}).get("value")
    text_desc = props.get("textDescription", "")
    observed_at = props.get("timestamp", "")

    # Convert wind direction degrees to compass label
    direction_label: Optional[str] = None
    if wind_dir_deg is not None:
        dirs = ["N", "NNE", "NE", "ENE", "E", "ESE", "SE", "SSE",
                "S", "SSW", "SW", "WSW", "W", "WNW", "NW", "NNW"]
        idx = round(float(wind_dir_deg) / 22.5) % 16
        direction_label = dirs[idx]

    return Conditions(
        temperature_f=_c_to_f(temp_c),
        temperature_c=round(float(temp_c), 1) if temp_c is not None else None,
        relative_humidity_pct=round(float(humidity), 1) if humidity is not None else None,
        wind_speed_mph=_ms_to_mph(wind_ms),
        wind_direction=direction_label,
        sky_condition=text_desc or None,
        observed_at=observed_at,
    )


# ── Active alerts ─────────────────────────────────────────────────────────────

def fetch_alerts(lat: float, lon: float, session: requests.Session) -> list[Alert]:
    url = NWS_ALERTS_URL.format(lat=f"{lat:.4f}", lon=f"{lon:.4f}")
    logger.debug("Fetching active alerts: %s", url)
    resp = session.get(url, timeout=15)
    logger.info("NWS alerts HTTP %d for (%.4f, %.4f)", resp.status_code, lat, lon)
    resp.raise_for_status()

    alerts = []
    for feature in resp.json().get("features", []):
        p = feature.get("properties", {})
        alert = Alert(
            id=p.get("id", ""),
            event=p.get("event", ""),
            headline=p.get("headline", ""),
            description=p.get("description", ""),
            instruction=p.get("instruction", ""),
            severity=p.get("severity", ""),
            urgency=p.get("urgency", ""),
            effective=p.get("effective", ""),
            expires=p.get("expires", ""),
            areas_affected=p.get("areaDesc", ""),
            sender_name=p.get("senderName", ""),
        )
        alerts.append(alert)
        logger.info(
            "Active alert: %s | Severity: %s | Urgency: %s",
            alert.event, alert.severity, alert.urgency,
        )

    if not alerts:
        logger.info("No active alerts for (%.4f, %.4f)", lat, lon)

    return alerts


# ── Mock mode ─────────────────────────────────────────────────────────────────

def _load_mock() -> WeatherData:
    logger.info("[MOCK] Reading weather data from %s", _MOCK_FILE)
    with open(_MOCK_FILE, "r", encoding="utf-8") as fh:
        data = json.load(fh)

    c = data.get("current_conditions", {})
    conditions = Conditions(
        temperature_f=c.get("temperature_f"),
        temperature_c=c.get("temperature_c"),
        relative_humidity_pct=c.get("relative_humidity_pct"),
        wind_speed_mph=c.get("wind_speed_mph"),
        wind_direction=c.get("wind_direction"),
        sky_condition=c.get("sky_condition"),
        observed_at=c.get("observed_at"),
        location_name=data.get("location_name"),
    )

    alerts = []
    for a in data.get("active_alerts", []):
        alerts.append(Alert(
            id=a.get("id", ""),
            event=a.get("event", ""),
            headline=a.get("headline", ""),
            description=a.get("description", ""),
            instruction=a.get("instruction", ""),
            severity=a.get("severity", ""),
            urgency=a.get("urgency", ""),
            effective=a.get("effective", ""),
            expires=a.get("ends", a.get("expires", "")),
            areas_affected=a.get("areas_affected", ""),
            sender_name=a.get("sender_name", ""),
        ))
        logger.info("[MOCK] Alert: %s | %s", a.get("event"), a.get("severity"))

    coords = data.get("coordinates", {})
    return WeatherData(
        conditions=conditions,
        alerts=alerts,
        lat=coords.get("lat"),
        lon=coords.get("lon"),
        source="mock",
    )


# ── Main client class ─────────────────────────────────────────────────────────

class NWSClient:
    """
    Stateful client that caches the resolved NWS grid between polls.
    Cache is invalidated when the ZIP code / coordinates change.
    """

    def __init__(self, user_agent: str):
        self._session = _build_session(user_agent)
        self._cached_lat: Optional[float] = None
        self._cached_lon: Optional[float] = None
        self._cached_station: Optional[str] = None

    def _resolve_coordinates(self, cfg) -> tuple[float, float]:
        """Return (lat, lon) from override or Census Geocoder."""
        if cfg.lat_override is not None and cfg.lon_override is not None:
            logger.debug(
                "Using coordinate override: lat=%.4f lon=%.4f",
                cfg.lat_override, cfg.lon_override,
            )
            return float(cfg.lat_override), float(cfg.lon_override)

        if not cfg.zip_code:
            raise ValueError(
                "No ZIP code or lat/lon override configured. "
                "Open the config UI at http://<device>:8080 and enter a ZIP code."
            )

        return geocode_zip(cfg.zip_code, self._session)

    def _resolve_station(self, lat: float, lon: float) -> str:
        """Resolve + cache the nearest observation station."""
        if self._cached_station and self._cached_lat == lat and self._cached_lon == lon:
            return self._cached_station

        grid = resolve_nws_grid(lat, lon, self._session)
        stations_url = grid.get("observationStations", "")
        if not stations_url:
            raise ValueError("NWS /points response did not include observationStations URL")

        station = get_nearest_station(stations_url, self._session)
        self._cached_lat = lat
        self._cached_lon = lon
        self._cached_station = station
        return station

    def fetch(self, cfg) -> WeatherData:
        """
        Main entry point.  Returns a WeatherData instance.
        Uses mock_weather.json when cfg.mock_mode is True.
        Raises on unrecoverable errors (no ZIP configured, API down, etc.).
        """
        if cfg.mock_mode:
            return _load_mock()

        lat, lon = self._resolve_coordinates(cfg)

        # Fetch station observations and alerts concurrently would be ideal,
        # but sequential is simpler and the NWS rate limit is generous.
        station = self._resolve_station(lat, lon)

        conditions = fetch_observations(station, self._session)
        alerts = fetch_alerts(lat, lon, self._session)

        return WeatherData(
            conditions=conditions,
            alerts=alerts,
            lat=lat,
            lon=lon,
            source="nws",
        )
