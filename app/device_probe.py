"""
device_probe.py — Detect Axis device capabilities via VAPIX on startup.

Checks performed:
  1. Video capability  — determines if text overlay is supported.
  2. Virtual input count — ensures our configured port range fits the device.
  3. Basic reachability — logs the device model for diagnostics.

Results are cached after the first successful probe so re-probing on each
poll cycle is avoided.  Call probe() again after a config change if needed.
"""

import logging
from dataclasses import dataclass
from typing import Optional

logger = logging.getLogger(__name__)


@dataclass
class DeviceCapabilities:
    """Snapshot of what this device supports."""
    has_video: bool
    max_virtual_inputs: int
    model: str
    firmware_version: str
    serial_number: str
    probe_successful: bool

    def __str__(self) -> str:
        return (
            f"Model={self.model!r} FW={self.firmware_version!r} "
            f"Serial={self.serial_number!r} "
            f"Video={self.has_video} MaxVirtualInputs={self.max_virtual_inputs}"
        )


# Sentinel returned when the device cannot be reached (e.g. during unit tests
# or when the app starts before the host VAPIX stack is ready).
UNKNOWN_CAPABILITIES = DeviceCapabilities(
    has_video=False,
    max_virtual_inputs=32,  # conservative safe default
    model="Unknown",
    firmware_version="Unknown",
    serial_number="Unknown",
    probe_successful=False,
)


def probe(vapix_client) -> DeviceCapabilities:
    """
    Probe the Axis device via VAPIX and return its capabilities.

    Args:
        vapix_client: An initialised VapixClient instance.

    Returns:
        DeviceCapabilities — always returns something, never raises.
    """
    model = "Unknown"
    firmware = "Unknown"
    serial = "Unknown"
    has_video = False
    max_virtual_inputs = 32  # safe conservative default

    # ── 1. Basic device info (model, firmware, serial) ────────────────────────
    try:
        info = vapix_client.get_basic_device_info()
        model = info.get("Model", info.get("ProdShortName", "Unknown"))
        firmware = info.get("Version", info.get("Firmware", "Unknown"))
        serial = info.get("SerialNumber", "Unknown")
        logger.info("Device: model=%r firmware=%r serial=%r", model, firmware, serial)
    except Exception as exc:
        logger.warning("Could not retrieve basic device info: %s", exc)

    # ── 2. Video capability ───────────────────────────────────────────────────
    # The presence of Properties.Image in the parameter group confirms video.
    try:
        image_params = vapix_client.list_params(group="Properties.Image")
        # param.cgi returns lines like "root.Properties.Image.Resolution=..."
        has_video = bool(image_params) and "Properties.Image" in image_params
        if has_video:
            logger.info("Device has video capability — overlay enabled.")
        else:
            logger.info("No video capability detected — overlay disabled (speaker/intercom).")
    except Exception as exc:
        logger.warning("Could not determine video capability: %s. Assuming no video.", exc)
        has_video = False

    # ── 3. Virtual input count ────────────────────────────────────────────────
    try:
        port_list = vapix_client.get_virtual_port_list()
        # port_list is a list of port-number ints, e.g. [1, 2, 3, ..., 32]
        if port_list:
            max_virtual_inputs = max(port_list)
            logger.info("Device reports %d virtual input(s) (max port: %d)",
                        len(port_list), max_virtual_inputs)
        else:
            logger.warning(
                "Virtual port list was empty; defaulting to max=%d", max_virtual_inputs
            )
    except Exception as exc:
        logger.warning(
            "Could not query virtual port list: %s. "
            "Defaulting to max_virtual_inputs=%d.",
            exc, max_virtual_inputs,
        )

    caps = DeviceCapabilities(
        has_video=has_video,
        max_virtual_inputs=max_virtual_inputs,
        model=model,
        firmware_version=firmware,
        serial_number=serial,
        probe_successful=True,
    )
    logger.info("Device capabilities: %s", caps)
    return caps


def warn_if_port_range_exceeds(caps: DeviceCapabilities, cfg) -> None:
    """
    Log a WARNING if the configured virtual port range exceeds device maximum.
    Also warns if virtual_port_start < 1.
    """
    start = cfg.virtual_port_start
    n_alerts = len(cfg.enabled_alert_types)
    last_port = start + n_alerts - 1

    if start < 1:
        logger.warning(
            "virtual_port_start=%d is invalid (must be >= 1). "
            "Virtual input ports will likely fail.",
            start,
        )
    if n_alerts > 0 and last_port > caps.max_virtual_inputs:
        logger.warning(
            "Configured port range %d–%d exceeds device maximum of %d virtual inputs. "
            "Ports beyond %d will return VAPIX error 2104. "
            "Reduce virtual_port_start or the number of enabled alert types.",
            start, last_port, caps.max_virtual_inputs, caps.max_virtual_inputs,
        )
    else:
        logger.debug(
            "Virtual port range %d–%d is within device maximum (%d).",
            start, last_port, caps.max_virtual_inputs,
        )
