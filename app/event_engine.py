"""
event_engine.py — Match active NWS alerts to configured alert types
                   and fire/clear VAPIX virtual input ports.

Design:
  - Each enabled alert type maps to a virtual input port in order:
      enabled_alert_types[0] → virtual_port_start + 0
      enabled_alert_types[1] → virtual_port_start + 1
      ...
  - The engine maintains a set of currently-active port numbers.
  - On each poll cycle, it compares the new alert set to the previous state:
      new alert  → activate the corresponding virtual port (rising edge)
      cleared alert → deactivate (falling edge)
  - Alert matching is case-insensitive substring match so that
    "Tornado Warning" matches "Tornado Warning (PDS)" etc.
  - VapixError from out-of-range ports is caught and logged but does not
    stop other alerts from being processed.
"""

import logging
from dataclasses import dataclass, field

from vapix_client import VapixClient, VapixError
from nws_client import WeatherData

logger = logging.getLogger(__name__)


@dataclass
class EngineState:
    """
    Mutable state carried between poll cycles.
    active_ports: set of virtual port numbers currently held HIGH.
    """
    active_ports: set[int] = field(default_factory=set)


def _alert_type_to_port(alert_type: str, enabled_types: list[str], port_start: int) -> int | None:
    """
    Return the virtual port number for an alert type, or None if not configured.
    Match is normalised to lower-case stripped strings.
    """
    needle = alert_type.strip().lower()
    for idx, enabled in enumerate(enabled_types):
        if needle == enabled.strip().lower():
            return port_start + idx
    return None


def _active_ports_for_alerts(
    alerts,
    enabled_types: list[str],
    port_start: int,
) -> set[int]:
    """
    Given a list of Alert objects, return the set of virtual port numbers
    that should be HIGH (active) based on the configured enabled alert types.
    """
    ports: set[int] = set()
    for alert in alerts:
        port = _alert_type_to_port(alert.event, enabled_types, port_start)
        if port is not None:
            ports.add(port)
            logger.debug(
                "Alert %r matched → port %d", alert.event, port
            )
        else:
            logger.debug(
                "Alert %r has no configured port (not in enabled_alert_types)", alert.event
            )
    return ports


def run(
    weather: WeatherData,
    cfg,
    vapix: VapixClient,
    state: EngineState,
) -> EngineState:
    """
    Compare the current weather alerts against the previous engine state,
    then fire/clear virtual input ports as needed.

    Returns a new EngineState reflecting which ports are currently HIGH.
    This function is intentionally side-effect-free on the state object —
    it returns a new EngineState rather than mutating the passed-in one.
    """
    enabled = cfg.enabled_alert_types
    port_start = cfg.virtual_port_start

    if not enabled:
        logger.debug("No alert types configured — skipping event engine.")
        return EngineState(active_ports=set())

    desired_ports = _active_ports_for_alerts(weather.alerts, enabled, port_start)
    current_ports = set(state.active_ports)

    ports_to_activate = desired_ports - current_ports
    ports_to_deactivate = current_ports - desired_ports

    # ── Activate newly triggered alerts ──────────────────────────────────────
    for port in sorted(ports_to_activate):
        alert_type = enabled[port - port_start]
        logger.info(
            "ALERT ACTIVE: %r → activating virtual port %d", alert_type, port
        )
        try:
            vapix.activate_virtual_port(port)
        except VapixError as exc:
            logger.error("Could not activate port %d: %s", port, exc)
        except Exception as exc:
            logger.warning("Unexpected error activating port %d: %s", port, exc)

    # ── Deactivate cleared alerts ─────────────────────────────────────────────
    for port in sorted(ports_to_deactivate):
        idx = port - port_start
        alert_type = enabled[idx] if 0 <= idx < len(enabled) else f"port {port}"
        logger.info(
            "ALERT CLEARED: %r → deactivating virtual port %d", alert_type, port
        )
        try:
            vapix.deactivate_virtual_port(port)
        except VapixError as exc:
            logger.error("Could not deactivate port %d: %s", port, exc)
        except Exception as exc:
            logger.warning("Unexpected error deactivating port %d: %s", port, exc)

    # ── Summary ───────────────────────────────────────────────────────────────
    if not desired_ports and not current_ports:
        logger.info("No active alerts matching configured types.")
    elif desired_ports == current_ports:
        logger.debug("Alert state unchanged. Active ports: %s", sorted(desired_ports))
    else:
        logger.info(
            "Alert state updated. Active ports: %s (activated: %s, cleared: %s)",
            sorted(desired_ports),
            sorted(ports_to_activate),
            sorted(ports_to_deactivate),
        )

    return EngineState(active_ports=desired_ports)
