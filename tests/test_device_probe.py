"""
test_device_probe.py — Tests for device_probe.py

Covers camera detection, speaker/no-video detection, virtual port count
querying, and graceful fallback when VAPIX is unreachable.
"""

import sys
from pathlib import Path
from unittest.mock import MagicMock

import pytest

sys.path.insert(0, str(Path(__file__).parent.parent / "app"))
from device_probe import (
    probe,
    warn_if_port_range_exceeds,
    UNKNOWN_CAPABILITIES,
    DeviceCapabilities,
)


# ── probe() ───────────────────────────────────────────────────────────────────

class TestProbe:
    def test_camera_has_video_true(self, mock_vapix_client):
        mock_vapix_client.list_params.return_value = (
            "root.Properties.Image.Resolution=1920x1080\n"
            "root.Properties.Image.Rotation=0\n"
        )
        mock_vapix_client.get_virtual_port_list.return_value = list(range(1, 33))

        caps = probe(mock_vapix_client)
        assert caps.has_video is True

    def test_speaker_has_video_false(self, mock_vapix_client):
        # No Properties.Image in response → speaker/intercom
        mock_vapix_client.list_params.return_value = ""
        mock_vapix_client.get_virtual_port_list.return_value = list(range(1, 9))

        caps = probe(mock_vapix_client)
        assert caps.has_video is False

    def test_probe_populates_model(self, mock_vapix_client):
        caps = probe(mock_vapix_client)
        assert caps.model == "AXIS P3245-V"

    def test_probe_populates_firmware(self, mock_vapix_client):
        caps = probe(mock_vapix_client)
        assert caps.firmware_version == "11.8.62"

    def test_probe_populates_serial(self, mock_vapix_client):
        caps = probe(mock_vapix_client)
        assert caps.serial_number == "ACCC8ETEST01"

    def test_probe_successful_flag(self, mock_vapix_client):
        caps = probe(mock_vapix_client)
        assert caps.probe_successful is True

    def test_max_virtual_inputs_from_port_list(self, mock_vapix_client):
        mock_vapix_client.get_virtual_port_list.return_value = list(range(1, 65))
        caps = probe(mock_vapix_client)
        assert caps.max_virtual_inputs == 64

    def test_device_info_exception_does_not_crash(self, mock_vapix_client):
        mock_vapix_client.get_basic_device_info.side_effect = Exception("timeout")
        caps = probe(mock_vapix_client)
        assert caps.model == "Unknown"
        # Other capabilities still populated from remaining calls
        assert isinstance(caps, DeviceCapabilities)

    def test_video_detection_exception_defaults_no_video(self, mock_vapix_client):
        mock_vapix_client.list_params.side_effect = Exception("403 Forbidden")
        caps = probe(mock_vapix_client)
        assert caps.has_video is False

    def test_port_list_exception_uses_default(self, mock_vapix_client):
        mock_vapix_client.get_virtual_port_list.side_effect = Exception("network error")
        caps = probe(mock_vapix_client)
        # Should fall back to default of 32
        assert caps.max_virtual_inputs == 32

    def test_empty_port_list_uses_default(self, mock_vapix_client):
        mock_vapix_client.get_virtual_port_list.return_value = []
        caps = probe(mock_vapix_client)
        assert caps.max_virtual_inputs == 32


# ── UNKNOWN_CAPABILITIES sentinel ────────────────────────────────────────────

class TestUnknownCapabilities:
    def test_is_not_probe_successful(self):
        assert UNKNOWN_CAPABILITIES.probe_successful is False

    def test_has_no_video(self):
        assert UNKNOWN_CAPABILITIES.has_video is False

    def test_has_conservative_max_inputs(self):
        assert UNKNOWN_CAPABILITIES.max_virtual_inputs == 32


# ── warn_if_port_range_exceeds ────────────────────────────────────────────────

class TestWarnIfPortRangeExceeds:
    def _make_caps(self, max_inputs: int) -> DeviceCapabilities:
        return DeviceCapabilities(
            has_video=True,
            max_virtual_inputs=max_inputs,
            model="Test",
            firmware_version="1.0",
            serial_number="TEST",
            probe_successful=True,
        )

    def test_no_warning_when_in_range(self, default_config, caplog):
        import logging
        caps = self._make_caps(64)
        default_config.virtual_port_start = 20
        # 3 alert types → ports 20, 21, 22 → all <= 64
        with caplog.at_level(logging.WARNING):
            warn_if_port_range_exceeds(caps, default_config)
        warnings = [r for r in caplog.records if r.levelname == "WARNING"]
        assert len(warnings) == 0

    def test_warning_when_exceeds_max(self, default_config, caplog):
        import logging
        caps = self._make_caps(4)  # device has only 4 virtual inputs
        default_config.virtual_port_start = 20  # starts at 20 → exceeds 4
        with caplog.at_level(logging.WARNING):
            warn_if_port_range_exceeds(caps, default_config)
        assert any("exceeds device maximum" in r.message for r in caplog.records)

    def test_warning_when_port_start_below_1(self, default_config, caplog):
        import logging
        caps = self._make_caps(64)
        default_config.virtual_port_start = 0
        with caplog.at_level(logging.WARNING):
            warn_if_port_range_exceeds(caps, default_config)
        assert any("invalid" in r.message.lower() for r in caplog.records)
