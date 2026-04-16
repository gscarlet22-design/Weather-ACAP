"""
test_config.py — Tests for config.py

Covers:
  - defaults loaded from config_defaults.json
  - config_store.json values override defaults
  - environment variables override both files
  - save_config_store() writes and merges correctly
  - poll_interval floor at 60
  - virtual_port_start type coercion
"""

import json
import os
import sys
from pathlib import Path
from unittest.mock import patch

import pytest

sys.path.insert(0, str(Path(__file__).parent.parent / "app"))
import config as cfg_module
from config import Config, save_config_store


@pytest.fixture(autouse=True)
def clean_env(monkeypatch):
    """Strip all VAPIX/weather env vars before each test."""
    for key in [
        "VAPIX_HOST", "VAPIX_USER", "VAPIX_PASS",
        "ZIP_CODE", "LAT_OVERRIDE", "LON_OVERRIDE",
        "POLL_INTERVAL_SECONDS", "VIRTUAL_PORT_START",
        "MOCK_MODE", "LOG_LEVEL", "OVERLAY_ENABLED",
        "NWS_USER_AGENT",
    ]:
        monkeypatch.delenv(key, raising=False)


@pytest.fixture
def patch_store(tmp_path, monkeypatch):
    """Redirect config_store.json reads/writes to a temp file."""
    store = tmp_path / "config_store.json"
    monkeypatch.setattr(cfg_module, "_STORE_FILE", store)
    return store


# ── Default loading ───────────────────────────────────────────────────────────

class TestDefaults:
    def test_vapix_host_default(self, patch_store):
        c = Config.load()
        assert c.vapix_host == "localhost"

    def test_vapix_user_default(self, patch_store):
        c = Config.load()
        assert c.vapix_user == "root"

    def test_virtual_port_start_default(self, patch_store):
        c = Config.load()
        assert c.virtual_port_start == 20

    def test_overlay_enabled_default(self, patch_store):
        c = Config.load()
        assert c.overlay_enabled is True

    def test_poll_interval_minimum_enforced(self, patch_store):
        """poll_interval should never be below 60."""
        c = Config.load()
        assert c.poll_interval >= 60

    def test_mock_mode_default_false(self, patch_store):
        c = Config.load()
        assert c.mock_mode is False

    def test_enabled_alert_types_not_empty(self, patch_store):
        c = Config.load()
        assert len(c.enabled_alert_types) > 0

    def test_vapix_base_url_no_scheme(self, patch_store, monkeypatch):
        monkeypatch.setenv("VAPIX_HOST", "192.168.1.100")
        c = Config.load()
        assert c.vapix_base_url == "http://192.168.1.100"

    def test_vapix_base_url_with_scheme(self, patch_store, monkeypatch):
        monkeypatch.setenv("VAPIX_HOST", "http://192.168.1.100")
        c = Config.load()
        assert c.vapix_base_url == "http://192.168.1.100"


# ── Environment variable overrides ───────────────────────────────────────────

class TestEnvOverrides:
    def test_vapix_host_from_env(self, patch_store, monkeypatch):
        monkeypatch.setenv("VAPIX_HOST", "10.0.0.50")
        c = Config.load()
        assert c.vapix_host == "10.0.0.50"

    def test_vapix_pass_from_env(self, patch_store, monkeypatch):
        monkeypatch.setenv("VAPIX_PASS", "secret123")
        c = Config.load()
        assert c.vapix_pass == "secret123"

    def test_zip_code_from_env(self, patch_store, monkeypatch):
        monkeypatch.setenv("ZIP_CODE", "90210")
        c = Config.load()
        assert c.zip_code == "90210"

    def test_mock_mode_true_from_env(self, patch_store, monkeypatch):
        monkeypatch.setenv("MOCK_MODE", "true")
        c = Config.load()
        assert c.mock_mode is True

    def test_mock_mode_false_from_env(self, patch_store, monkeypatch):
        monkeypatch.setenv("MOCK_MODE", "false")
        c = Config.load()
        assert c.mock_mode is False

    def test_poll_interval_from_env(self, patch_store, monkeypatch):
        monkeypatch.setenv("POLL_INTERVAL_SECONDS", "120")
        c = Config.load()
        assert c.poll_interval == 120

    def test_poll_interval_below_floor(self, patch_store, monkeypatch):
        monkeypatch.setenv("POLL_INTERVAL_SECONDS", "10")
        c = Config.load()
        assert c.poll_interval == 60  # enforced floor

    def test_lat_override_from_env(self, patch_store, monkeypatch):
        monkeypatch.setenv("LAT_OVERRIDE", "30.2672")
        c = Config.load()
        assert c.lat_override == pytest.approx(30.2672)

    def test_invalid_poll_interval_falls_back(self, patch_store, monkeypatch):
        monkeypatch.setenv("POLL_INTERVAL_SECONDS", "notanumber")
        c = Config.load()
        assert c.poll_interval >= 60  # should not crash

    def test_virtual_port_start_from_env(self, patch_store, monkeypatch):
        monkeypatch.setenv("VIRTUAL_PORT_START", "30")
        c = Config.load()
        assert c.virtual_port_start == 30


# ── config_store.json persistence ────────────────────────────────────────────

class TestConfigStore:
    def test_save_creates_file(self, patch_store):
        save_config_store({"zip_code": "78701"})
        assert patch_store.exists()

    def test_saved_value_loaded(self, patch_store):
        save_config_store({"zip_code": "78701"})
        c = Config.load()
        assert c.zip_code == "78701"

    def test_save_merges_with_existing(self, patch_store):
        save_config_store({"zip_code": "78701"})
        save_config_store({"virtual_port_start": 25})
        c = Config.load()
        assert c.zip_code == "78701"
        assert c.virtual_port_start == 25

    def test_env_overrides_store(self, patch_store, monkeypatch):
        save_config_store({"zip_code": "78701"})
        monkeypatch.setenv("ZIP_CODE", "90210")
        c = Config.load()
        assert c.zip_code == "90210"

    def test_poll_interval_coerced_to_int(self, patch_store):
        save_config_store({"poll_interval_seconds": "600"})
        c = Config.load()
        assert c.poll_interval == 600

    def test_poll_interval_bad_value_defaults(self, patch_store):
        save_config_store({"poll_interval_seconds": "garbage"})
        c = Config.load()
        assert c.poll_interval == 300  # reset to default

    def test_write_is_atomic(self, patch_store):
        """Ensure no .tmp file left behind after a successful save."""
        save_config_store({"zip_code": "12345"})
        tmp = patch_store.with_suffix(".tmp")
        assert not tmp.exists()

    def test_as_dict_excludes_password(self, patch_store, monkeypatch):
        monkeypatch.setenv("VAPIX_PASS", "topsecret")
        c = Config.load()
        d = c.as_dict()
        assert "vapix_pass" not in d
