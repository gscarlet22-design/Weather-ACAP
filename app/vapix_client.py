"""
vapix_client.py — VAPIX HTTP API wrapper for Weather ACAP.

Handles:
  - Digest authentication (required by all Axis devices)
  - Virtual input port activate/deactivate  (event triggers)
  - Dynamic text overlay CRUD               (video overlay)
  - Parameter list                          (device capabilities)
  - Virtual port list                       (capability probe)
  - Basic device info                       (capability probe)

Every outbound HTTP call logs the method, URL, and response status code.
Non-2xx responses are logged at WARNING level and never raise by default
(callers decide whether to escalate). VAPIX error 2104 (invalid port) is
detected and logged as ERROR.

Authentication note:
  Axis devices use HTTP Digest authentication. This client uses
  requests.auth.HTTPDigestAuth which handles nonce/qop negotiation
  automatically. The first call triggers a 401 challenge; subsequent
  calls reuse the session's cached credentials.
"""

import logging
from typing import Optional

import requests
from requests.auth import HTTPDigestAuth

logger = logging.getLogger(__name__)

# VAPIX error code returned when a virtual port number is out of range.
_VAPIX_ERR_INVALID_PORT = "2104"


class VapixError(Exception):
    """Raised when a VAPIX call returns a recognisable error code."""


class VapixClient:
    """
    Thin wrapper around requests.Session configured for VAPIX Digest auth.

    Usage:
        client = VapixClient(base_url="http://localhost", user="root", password="pass")
        client.activate_virtual_port(20)
        client.set_overlay_text("Temp: 72°F | Sunny", overlay_id="weather_acap_1")
    """

    def __init__(self, base_url: str, user: str, password: str):
        self._base = base_url.rstrip("/")
        self._session = requests.Session()
        self._session.auth = HTTPDigestAuth(user, password)
        # Reasonable timeouts: 5s connect, 10s read
        self._timeout = (5, 10)
        self._overlay_id: Optional[str] = None

    # ── Internal helpers ──────────────────────────────────────────────────────

    def _get(self, path: str, params: Optional[dict] = None) -> requests.Response:
        url = f"{self._base}{path}"
        resp = self._session.get(url, params=params, timeout=self._timeout)
        self._log_response("GET", url, resp)
        return resp

    def _post(self, path: str, json_body: Optional[dict] = None,
              data: Optional[dict] = None) -> requests.Response:
        url = f"{self._base}{path}"
        resp = self._session.post(url, json=json_body, data=data, timeout=self._timeout)
        self._log_response("POST", url, resp)
        return resp

    def _put(self, path: str, json_body: Optional[dict] = None) -> requests.Response:
        url = f"{self._base}{path}"
        resp = self._session.put(url, json=json_body, timeout=self._timeout)
        self._log_response("PUT", url, resp)
        return resp

    def _delete(self, path: str) -> requests.Response:
        url = f"{self._base}{path}"
        resp = self._session.delete(url, timeout=self._timeout)
        self._log_response("DELETE", url, resp)
        return resp

    @staticmethod
    def _log_response(method: str, url: str, resp: requests.Response) -> None:
        level = logging.DEBUG if resp.ok else logging.WARNING
        logger.log(level, "VAPIX %s %s → HTTP %d", method, url, resp.status_code)
        if not resp.ok:
            # Log a snippet of the response body for diagnostics
            snippet = resp.text[:200].replace("\n", " ")
            logger.warning("VAPIX response body: %s", snippet)

    @staticmethod
    def _check_vapix_error(text: str, port: Optional[int] = None) -> None:
        """
        Detect VAPIX application-layer error codes in the response text.
        VAPIX error 2104 means the virtual port number is out of range.
        """
        if _VAPIX_ERR_INVALID_PORT in text:
            msg = (
                f"VAPIX error 2104 (Invalid parameter value): "
                f"virtual port {port} is out of range for this device. "
                f"Reduce virtual_port_start in the config UI."
            )
            logger.error(msg)
            raise VapixError(msg)

    # ── Virtual input ports ───────────────────────────────────────────────────

    def activate_virtual_port(self, port: int) -> bool:
        """
        Simulate a virtual input going HIGH (active).
        action=11 → port active (rising edge)

        Returns True on success, False on HTTP error (non-fatal).
        Raises VapixError if VAPIX returns error code 2104 (port out of range).
        """
        resp = self._get(
            "/axis-cgi/io/virtualport.cgi",
            params={"schemaversion": "1", "action": "11", "port": str(port)},
        )
        if resp.ok:
            self._check_vapix_error(resp.text, port)
            logger.info("Virtual port %d activated (HIGH)", port)
            return True
        return False

    def deactivate_virtual_port(self, port: int) -> bool:
        """
        Simulate a virtual input going LOW (inactive).
        action=10 → port inactive (falling edge)
        """
        resp = self._get(
            "/axis-cgi/io/virtualport.cgi",
            params={"schemaversion": "1", "action": "10", "port": str(port)},
        )
        if resp.ok:
            self._check_vapix_error(resp.text, port)
            logger.info("Virtual port %d deactivated (LOW)", port)
            return True
        return False

    def get_virtual_port_list(self) -> list[int]:
        """
        Query the device for its list of configured virtual input ports.
        Returns a list of port numbers (ints).
        On failure returns an empty list (probe will fall back to the default).
        """
        resp = self._get(
            "/axis-cgi/io/port.cgi",
            params={"action": "GetPortList"},
        )
        if not resp.ok:
            return []

        # Response is a simple text format like:
        # port=1&port=2&port=3 or port[1]=...&port[2]=...
        # We extract all integers found in the response text.
        import re
        numbers = re.findall(r"port(?:\[\d+\])?=(\d+)", resp.text)
        if numbers:
            return sorted(int(n) for n in numbers)

        # Some firmware variants return a count instead of a list.
        # Try Properties.VirtualInput.NbrOfVirtualInputs as a fallback.
        count_resp = self._get(
            "/axis-cgi/param.cgi",
            params={"action": "list", "group": "Properties.VirtualInput"},
        )
        if count_resp.ok and count_resp.text.strip():
            count_match = re.search(r"NbrOfVirtualInputs=(\d+)", count_resp.text)
            if count_match:
                count = int(count_match.group(1))
                logger.debug("Device reports %d virtual inputs via param.cgi", count)
                return list(range(1, count + 1))

        return []

    # ── Dynamic text overlay ──────────────────────────────────────────────────

    def create_overlay(self, text: str, position: str = "topLeft") -> Optional[str]:
        """
        Create a new dynamic text overlay via the VAPIX Overlay REST API.
        Returns the overlay ID string, or None on failure.

        POST /vapix/overlays/text
        """
        payload = {
            "text": text,
            "position": position,
        }
        resp = self._post("/vapix/overlays/text", json_body=payload)
        if resp.ok:
            try:
                overlay_id = resp.json().get("id") or resp.json().get("overlayId")
                if overlay_id:
                    logger.info("Overlay created: id=%r text=%r", overlay_id, text[:60])
                    self._overlay_id = str(overlay_id)
                    return self._overlay_id
            except Exception as exc:
                logger.warning("Could not parse overlay creation response: %s", exc)
        return None

    def update_overlay(self, overlay_id: str, text: str) -> bool:
        """
        Update an existing overlay's text in place.
        PUT /vapix/overlays/text/{id}
        """
        resp = self._put(f"/vapix/overlays/text/{overlay_id}", json_body={"text": text})
        if resp.ok:
            logger.debug("Overlay %r updated: %r", overlay_id, text[:60])
            return True
        return False

    def delete_overlay(self, overlay_id: str) -> bool:
        """
        Remove an overlay.  Called on shutdown.
        DELETE /vapix/overlays/text/{id}
        """
        resp = self._delete(f"/vapix/overlays/text/{overlay_id}")
        if resp.ok:
            logger.info("Overlay %r deleted", overlay_id)
            if self._overlay_id == overlay_id:
                self._overlay_id = None
            return True
        return False

    def list_overlays(self) -> list[dict]:
        """
        Return all text overlays currently registered on the device.
        GET /vapix/overlays/text
        """
        resp = self._get("/vapix/overlays/text")
        if resp.ok:
            try:
                return resp.json() if isinstance(resp.json(), list) else resp.json().get("items", [])
            except Exception:
                pass
        return []

    # ── Parameter API ─────────────────────────────────────────────────────────

    def list_params(self, group: str) -> str:
        """
        Call param.cgi to list parameters for a given group.
        Returns the raw response text, or empty string on failure.
        """
        resp = self._get(
            "/axis-cgi/param.cgi",
            params={"action": "list", "group": group},
        )
        return resp.text if resp.ok else ""

    # ── Basic device info ─────────────────────────────────────────────────────

    def get_basic_device_info(self) -> dict:
        """
        Return basic device info (model, firmware, serial) as a flat dict.
        Tries the newer JSON endpoint first, falls back to param.cgi.
        """
        # Newer firmware: /axis-cgi/basicdeviceinfo.cgi returns JSON
        resp = self._get(
            "/axis-cgi/basicdeviceinfo.cgi",
            params={"action": "list"},
        )
        if resp.ok:
            try:
                data = resp.json()
                # Response is {"apiVersion":"...","data":{"propertyList":{...}}}
                if "data" in data and "propertyList" in data["data"]:
                    return data["data"]["propertyList"]
                return data
            except Exception:
                pass

        # Fallback: parse param.cgi brand properties
        text = self.list_params("Brand")
        info: dict = {}
        for line in text.splitlines():
            if "=" in line:
                key, _, val = line.partition("=")
                info[key.split(".")[-1]] = val.strip()
        return info

    # ── Context manager support ───────────────────────────────────────────────

    def __enter__(self):
        return self

    def __exit__(self, *_):
        self._session.close()
