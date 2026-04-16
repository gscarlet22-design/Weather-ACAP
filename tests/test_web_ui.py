"""
test_web_ui.py — Tests for web_ui.py (Flask config UI)

Uses Flask's built-in test client.  Does not start an HTTP server.
Tests cover GET / renders form, POST /save validates and writes config,
/health returns 200, /status returns JSON.
"""

import json
import sys
from pathlib import Path
from unittest.mock import patch, MagicMock

import pytest

sys.path.insert(0, str(Path(__file__).parent.parent / "app"))

# Patch save_config_store before importing web_ui so we control disk writes.
import config as cfg_module
import web_ui


@pytest.fixture(autouse=True)
def patch_store(tmp_path, monkeypatch):
    """Redirect config_store.json reads/writes to a temp directory."""
    store = tmp_path / "config_store.json"
    monkeypatch.setattr(cfg_module, "_STORE_FILE", store)
    return store


@pytest.fixture
def flask_client():
    web_ui.app.config["TESTING"] = True
    with web_ui.app.test_client() as client:
        yield client


# ── GET / ─────────────────────────────────────────────────────────────────────

class TestIndexPage:
    def test_get_returns_200(self, flask_client):
        resp = flask_client.get("/")
        assert resp.status_code == 200

    def test_page_contains_form(self, flask_client):
        resp = flask_client.get("/")
        assert b"<form" in resp.data

    def test_page_contains_zip_input(self, flask_client):
        resp = flask_client.get("/")
        assert b"zip_code" in resp.data

    def test_page_contains_virtual_port_start(self, flask_client):
        resp = flask_client.get("/")
        assert b"virtual_port_start" in resp.data

    def test_saved_banner_shown(self, flask_client):
        resp = flask_client.get("/?saved=1")
        assert b"Saved" in resp.data

    def test_error_message_shown_after_bad_post(self, flask_client):
        resp = flask_client.post("/save", data={"zip_code": "abc"})
        assert resp.status_code == 400
        assert b"numeric" in resp.data.lower() or b"error" in resp.data.lower()


# ── POST /save ────────────────────────────────────────────────────────────────

class TestSave:
    def _valid_form(self, **overrides):
        base = {
            "zip_code": "73301",
            "lat_override": "",
            "lon_override": "",
            "enabled_alert_types": ["Tornado Warning", "Flash Flood Warning"],
            "virtual_port_start": "20",
            "poll_interval_seconds": "300",
            "overlay_position": "topLeft",
        }
        base.update(overrides)
        return base

    def test_valid_save_redirects(self, flask_client):
        resp = flask_client.post("/save", data=self._valid_form())
        assert resp.status_code == 302
        assert b"saved=1" in resp.headers["Location"].encode()

    def test_saves_zip_code(self, flask_client, patch_store):
        flask_client.post("/save", data=self._valid_form(zip_code="90210"))
        data = json.loads(patch_store.read_text())
        assert data["zip_code"] == "90210"

    def test_saves_enabled_alert_types(self, flask_client, patch_store):
        flask_client.post("/save", data=self._valid_form())
        data = json.loads(patch_store.read_text())
        assert "Tornado Warning" in data["enabled_alert_types"]

    def test_saves_virtual_port_start(self, flask_client, patch_store):
        flask_client.post("/save", data=self._valid_form(virtual_port_start="25"))
        data = json.loads(patch_store.read_text())
        assert data["virtual_port_start"] == 25

    def test_invalid_zip_non_numeric(self, flask_client):
        resp = flask_client.post("/save", data=self._valid_form(zip_code="abcde"))
        assert resp.status_code == 400

    def test_invalid_zip_wrong_length(self, flask_client):
        resp = flask_client.post("/save", data=self._valid_form(zip_code="1234"))
        assert resp.status_code == 400

    def test_empty_zip_is_valid(self, flask_client):
        resp = flask_client.post("/save", data=self._valid_form(zip_code=""))
        assert resp.status_code == 302

    def test_valid_lat_lon_override(self, flask_client, patch_store):
        resp = flask_client.post("/save", data=self._valid_form(
            zip_code="",
            lat_override="30.2672",
            lon_override="-97.7431",
        ))
        assert resp.status_code == 302
        data = json.loads(patch_store.read_text())
        assert abs(data["lat_override"] - 30.2672) < 0.001

    def test_only_one_coord_override_rejected(self, flask_client):
        resp = flask_client.post("/save", data=self._valid_form(
            lat_override="30.2672",
            lon_override="",
        ))
        assert resp.status_code == 400

    def test_out_of_range_lat_rejected(self, flask_client):
        resp = flask_client.post("/save", data=self._valid_form(
            lat_override="999.0",
            lon_override="-97.7431",
        ))
        assert resp.status_code == 400

    def test_mock_mode_on_saves_true(self, flask_client, patch_store):
        form = self._valid_form()
        form["mock_mode"] = "on"
        flask_client.post("/save", data=form)
        data = json.loads(patch_store.read_text())
        assert data["mock_mode"] is True

    def test_mock_mode_off_saves_false(self, flask_client, patch_store):
        flask_client.post("/save", data=self._valid_form())  # no mock_mode key
        data = json.loads(patch_store.read_text())
        assert data["mock_mode"] is False

    def test_overlay_enabled_on_saves_true(self, flask_client, patch_store):
        form = self._valid_form()
        form["overlay_enabled"] = "on"
        flask_client.post("/save", data=form)
        data = json.loads(patch_store.read_text())
        assert data["overlay_enabled"] is True

    def test_too_many_alert_types_truncated(self, flask_client, patch_store):
        form = self._valid_form(
            enabled_alert_types=[
                "Tornado Warning", "Tornado Watch",
                "Severe Thunderstorm Warning", "Severe Thunderstorm Watch",
                "Flash Flood Warning", "Flash Flood Watch",
                "Flash Flood Statement", "Winter Storm Warning",
                "Extra Type",  # 9th type — should be dropped
            ]
        )
        flask_client.post("/save", data=form)
        data = json.loads(patch_store.read_text())
        assert len(data["enabled_alert_types"]) <= 8

    def test_unknown_alert_types_rejected(self, flask_client, patch_store):
        form = self._valid_form(enabled_alert_types=["Fake Alert Type"])
        flask_client.post("/save", data=form)
        data = json.loads(patch_store.read_text())
        assert "Fake Alert Type" not in data.get("enabled_alert_types", [])


# ── /health ───────────────────────────────────────────────────────────────────

class TestHealth:
    def test_returns_200(self, flask_client):
        resp = flask_client.get("/health")
        assert resp.status_code == 200

    def test_returns_ok_true(self, flask_client):
        data = json.loads(flask_client.get("/health").data)
        assert data["ok"] is True


# ── /status ───────────────────────────────────────────────────────────────────

class TestStatus:
    def test_returns_200(self, flask_client):
        resp = flask_client.get("/status")
        assert resp.status_code == 200

    def test_returns_json(self, flask_client):
        resp = flask_client.get("/status")
        data = json.loads(resp.data)
        assert isinstance(data, dict)

    def test_reflects_updated_status(self, flask_client):
        web_ui.update_status({"last_poll_utc": "2026-04-05T18:00:00Z", "source": "mock"})
        data = json.loads(flask_client.get("/status").data)
        assert data.get("source") == "mock"
        assert "last_poll_utc" in data
