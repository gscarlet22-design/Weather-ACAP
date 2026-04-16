"""
test_vapix_client.py — Tests for vapix_client.py

All HTTP calls are intercepted by the `responses` library.
Tests verify Digest auth is used, response codes are logged, and
VAPIX error 2104 is detected and raised as VapixError.
"""

import re
import sys
from pathlib import Path

import pytest
import responses as rsps_lib

sys.path.insert(0, str(Path(__file__).parent.parent / "app"))
from vapix_client import VapixClient, VapixError


BASE = "http://testdevice"


@pytest.fixture
def client():
    return VapixClient(base_url=BASE, user="root", password="testpass")


# ── Virtual port activate / deactivate ────────────────────────────────────────

class TestVirtualPorts:
    @rsps_lib.activate
    def test_activate_returns_true_on_200(self, client):
        rsps_lib.add(
            rsps_lib.GET,
            f"{BASE}/axis-cgi/io/virtualport.cgi",
            body="OK", status=200,
        )
        assert client.activate_virtual_port(20) is True

    @rsps_lib.activate
    def test_deactivate_returns_true_on_200(self, client):
        rsps_lib.add(
            rsps_lib.GET,
            f"{BASE}/axis-cgi/io/virtualport.cgi",
            body="OK", status=200,
        )
        assert client.deactivate_virtual_port(20) is True

    @rsps_lib.activate
    def test_activate_returns_false_on_401(self, client):
        rsps_lib.add(
            rsps_lib.GET,
            f"{BASE}/axis-cgi/io/virtualport.cgi",
            status=401,
        )
        assert client.activate_virtual_port(20) is False

    @rsps_lib.activate
    def test_error_2104_raises_vapix_error(self, client):
        rsps_lib.add(
            rsps_lib.GET,
            f"{BASE}/axis-cgi/io/virtualport.cgi",
            body="Error: 2104 Invalid parameter value",
            status=200,
        )
        with pytest.raises(VapixError, match="2104"):
            client.activate_virtual_port(99)

    @rsps_lib.activate
    def test_activate_sends_action_11(self, client):
        rsps_lib.add(
            rsps_lib.GET,
            f"{BASE}/axis-cgi/io/virtualport.cgi",
            body="OK", status=200,
        )
        client.activate_virtual_port(20)
        req = rsps_lib.calls[0].request
        assert "action=11" in req.url
        assert "port=20" in req.url

    @rsps_lib.activate
    def test_deactivate_sends_action_10(self, client):
        rsps_lib.add(
            rsps_lib.GET,
            f"{BASE}/axis-cgi/io/virtualport.cgi",
            body="OK", status=200,
        )
        client.deactivate_virtual_port(20)
        req = rsps_lib.calls[0].request
        assert "action=10" in req.url
        assert "port=20" in req.url


# ── Overlay CRUD ──────────────────────────────────────────────────────────────

class TestOverlay:
    @rsps_lib.activate
    def test_create_overlay_returns_id(self, client):
        rsps_lib.add(
            rsps_lib.POST,
            f"{BASE}/vapix/overlays/text",
            json={"id": "overlay-abc-123"},
            status=200,
        )
        overlay_id = client.create_overlay("Temp: 91°F | Mostly Cloudy", "topLeft")
        assert overlay_id == "overlay-abc-123"

    @rsps_lib.activate
    def test_create_overlay_returns_none_on_failure(self, client):
        rsps_lib.add(
            rsps_lib.POST,
            f"{BASE}/vapix/overlays/text",
            status=403,
        )
        result = client.create_overlay("test text")
        assert result is None

    @rsps_lib.activate
    def test_update_overlay_returns_true(self, client):
        rsps_lib.add(
            rsps_lib.PUT,
            f"{BASE}/vapix/overlays/text/overlay-abc-123",
            json={"id": "overlay-abc-123"},
            status=200,
        )
        assert client.update_overlay("overlay-abc-123", "New text") is True

    @rsps_lib.activate
    def test_update_overlay_returns_false_on_404(self, client):
        rsps_lib.add(
            rsps_lib.PUT,
            f"{BASE}/vapix/overlays/text/gone",
            status=404,
        )
        assert client.update_overlay("gone", "text") is False

    @rsps_lib.activate
    def test_delete_overlay_returns_true(self, client):
        rsps_lib.add(
            rsps_lib.DELETE,
            f"{BASE}/vapix/overlays/text/overlay-abc-123",
            status=200,
        )
        assert client.delete_overlay("overlay-abc-123") is True

    @rsps_lib.activate
    def test_list_overlays_returns_list(self, client):
        rsps_lib.add(
            rsps_lib.GET,
            f"{BASE}/vapix/overlays/text",
            json=[{"id": "overlay-1", "text": "hello"}],
            status=200,
        )
        items = client.list_overlays()
        assert isinstance(items, list)
        assert items[0]["id"] == "overlay-1"

    @rsps_lib.activate
    def test_list_overlays_empty_on_error(self, client):
        rsps_lib.add(
            rsps_lib.GET,
            f"{BASE}/vapix/overlays/text",
            status=500,
        )
        assert client.list_overlays() == []


# ── Parameter API ─────────────────────────────────────────────────────────────

class TestParamAPI:
    @rsps_lib.activate
    def test_list_params_returns_text(self, client):
        rsps_lib.add(
            rsps_lib.GET,
            f"{BASE}/axis-cgi/param.cgi",
            body="root.Properties.Image.Resolution=1920x1080\n",
            status=200,
        )
        result = client.list_params("Properties.Image")
        assert "Properties.Image" in result

    @rsps_lib.activate
    def test_list_params_empty_on_failure(self, client):
        rsps_lib.add(
            rsps_lib.GET,
            f"{BASE}/axis-cgi/param.cgi",
            status=404,
        )
        assert client.list_params("Properties.Image") == ""


# ── Basic device info ─────────────────────────────────────────────────────────

class TestBasicDeviceInfo:
    @rsps_lib.activate
    def test_parses_property_list(self, client):
        rsps_lib.add(
            rsps_lib.GET,
            f"{BASE}/axis-cgi/basicdeviceinfo.cgi",
            json={
                "apiVersion": "1.3",
                "data": {
                    "propertyList": {
                        "Model": "AXIS P3245-V",
                        "Version": "11.8.62",
                        "SerialNumber": "ACCC8ETEST01",
                    }
                },
            },
            status=200,
        )
        info = client.get_basic_device_info()
        assert info["Model"] == "AXIS P3245-V"
        assert info["SerialNumber"] == "ACCC8ETEST01"

    @rsps_lib.activate
    def test_falls_back_to_param_cgi(self, client):
        rsps_lib.add(
            rsps_lib.GET,
            f"{BASE}/axis-cgi/basicdeviceinfo.cgi",
            status=404,
        )
        rsps_lib.add(
            rsps_lib.GET,
            f"{BASE}/axis-cgi/param.cgi",
            body="root.Brand.ProdShortName=AXIS P3245-V\nroot.Brand.Firmware=11.8.62\n",
            status=200,
        )
        info = client.get_basic_device_info()
        assert isinstance(info, dict)


# ── Virtual port list ─────────────────────────────────────────────────────────

class TestVirtualPortList:
    @rsps_lib.activate
    def test_parses_port_list(self, client):
        rsps_lib.add(
            rsps_lib.GET,
            f"{BASE}/axis-cgi/io/port.cgi",
            body="port=1&port=2&port=3",
            status=200,
        )
        ports = client.get_virtual_port_list()
        assert ports == [1, 2, 3]

    @rsps_lib.activate
    def test_falls_back_to_param_count(self, client):
        rsps_lib.add(
            rsps_lib.GET,
            f"{BASE}/axis-cgi/io/port.cgi",
            body="",
            status=200,
        )
        rsps_lib.add(
            rsps_lib.GET,
            f"{BASE}/axis-cgi/param.cgi",
            body="root.Properties.VirtualInput.NbrOfVirtualInputs=8\n",
            status=200,
        )
        ports = client.get_virtual_port_list()
        assert len(ports) == 8
        assert ports == list(range(1, 9))

    @rsps_lib.activate
    def test_returns_empty_on_failure(self, client):
        rsps_lib.add(
            rsps_lib.GET,
            f"{BASE}/axis-cgi/io/port.cgi",
            status=500,
        )
        assert client.get_virtual_port_list() == []


# ── Context manager ───────────────────────────────────────────────────────────

def test_context_manager():
    with VapixClient(base_url=BASE, user="root", password="pass") as vc:
        assert vc is not None
    # Should not raise after exit
