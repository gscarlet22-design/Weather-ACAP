"""
web_ui.py — Flask-based local configuration web UI for Weather ACAP.

Runs as a daemon thread on port 8080 inside the container.
Access it at http://<device-ip>:8080 from any browser on the same network.

Routes:
  GET  /         → render the config form (index.html)
  POST /save     → validate + write config_store.json, redirect back
  GET  /status   → JSON snapshot of last weather poll (for debugging)
  GET  /health   → simple {"ok": true} liveness check
"""

import json
import logging
import threading
from typing import Optional

from flask import Flask, jsonify, redirect, render_template, request, url_for

from config import Config, save_config_store

logger = logging.getLogger(__name__)

app = Flask(__name__, template_folder="templates")
app.secret_key = "weather-acap-local-only"  # not internet-facing; low risk

# Shared state injected by weather_acap.py after each poll cycle.
# Protected by _status_lock for thread-safety.
_status_lock = threading.Lock()
_last_status: dict = {}


def update_status(data: dict) -> None:
    """Called by the main poll loop to record the latest weather snapshot."""
    with _status_lock:
        _last_status.clear()
        _last_status.update(data)


# ── Routes ────────────────────────────────────────────────────────────────────

@app.route("/")
def index():
    cfg = Config.load()
    return render_template("index.html", cfg=cfg)


@app.route("/save", methods=["POST"])
def save():
    form = request.form

    # ── ZIP code ──────────────────────────────────────────────────────────────
    zip_code = form.get("zip_code", "").strip()
    if zip_code and not zip_code.isdigit():
        return render_template(
            "index.html",
            cfg=Config.load(),
            error="ZIP code must be numeric (e.g. 73301).",
        ), 400
    if zip_code and len(zip_code) != 5:
        return render_template(
            "index.html",
            cfg=Config.load(),
            error="ZIP code must be exactly 5 digits.",
        ), 400

    # ── Coordinate overrides ──────────────────────────────────────────────────
    lat_raw = form.get("lat_override", "").strip()
    lon_raw = form.get("lon_override", "").strip()

    lat_override: Optional[float] = None
    lon_override: Optional[float] = None

    if lat_raw:
        try:
            lat_override = float(lat_raw)
            if not (-90.0 <= lat_override <= 90.0):
                raise ValueError("out of range")
        except ValueError:
            return render_template(
                "index.html",
                cfg=Config.load(),
                error="Latitude must be a decimal number between -90 and 90.",
            ), 400

    if lon_raw:
        try:
            lon_override = float(lon_raw)
            if not (-180.0 <= lon_override <= 180.0):
                raise ValueError("out of range")
        except ValueError:
            return render_template(
                "index.html",
                cfg=Config.load(),
                error="Longitude must be a decimal number between -180 and 180.",
            ), 400

    # Require both or neither
    if bool(lat_override is not None) != bool(lon_override is not None):
        return render_template(
            "index.html",
            cfg=Config.load(),
            error="Provide both Latitude and Longitude overrides, or leave both empty.",
        ), 400

    # ── Alert types ───────────────────────────────────────────────────────────
    enabled_alerts = form.getlist("enabled_alert_types")
    # Sanitise: only accept values from the known list
    known = set(Config.load().all_alert_types)
    enabled_alerts = [a for a in enabled_alerts if a in known]
    if len(enabled_alerts) > 8:
        enabled_alerts = enabled_alerts[:8]
        logger.warning("Truncated enabled_alert_types to 8 (max supported).")

    # ── Virtual port start ────────────────────────────────────────────────────
    try:
        port_start = int(form.get("virtual_port_start", "20"))
        if port_start < 1:
            raise ValueError("must be >= 1")
    except ValueError:
        return render_template(
            "index.html",
            cfg=Config.load(),
            error="Virtual port start must be a positive integer.",
        ), 400

    # ── Poll interval ─────────────────────────────────────────────────────────
    try:
        poll_interval = max(60, int(form.get("poll_interval_seconds", "300")))
    except ValueError:
        poll_interval = 300

    # ── Overlay ───────────────────────────────────────────────────────────────
    overlay_enabled = form.get("overlay_enabled") == "on"
    overlay_position = form.get("overlay_position", "topLeft")
    valid_positions = {"topLeft", "topRight", "bottomLeft", "bottomRight"}
    if overlay_position not in valid_positions:
        overlay_position = "topLeft"

    # ── Mock mode ─────────────────────────────────────────────────────────────
    mock_mode = form.get("mock_mode") == "on"

    # ── Write ─────────────────────────────────────────────────────────────────
    updates = {
        "zip_code": zip_code,
        "lat_override": lat_override,
        "lon_override": lon_override,
        "enabled_alert_types": enabled_alerts,
        "virtual_port_start": port_start,
        "poll_interval_seconds": poll_interval,
        "overlay_enabled": overlay_enabled,
        "overlay_position": overlay_position,
        "mock_mode": mock_mode,
    }

    try:
        save_config_store(updates)
        logger.info("Config saved via web UI: zip=%r alerts=%s", zip_code, enabled_alerts)
    except OSError as exc:
        return render_template(
            "index.html",
            cfg=Config.load(),
            error=f"Failed to save configuration: {exc}",
        ), 500

    return redirect(url_for("index") + "?saved=1")


@app.route("/status")
def status():
    """Return the last weather poll result as JSON. Useful for debugging."""
    with _status_lock:
        snapshot = dict(_last_status)
    return jsonify(snapshot)


@app.route("/health")
def health():
    return jsonify({"ok": True})


# ── Thread entrypoint ─────────────────────────────────────────────────────────

def run_web_ui(host: str = "0.0.0.0", port: int = 8080) -> None:
    """
    Start the Flask dev server.  Called from weather_acap.py as a daemon thread.
    Use threaded=True so concurrent requests (browser + status poll) don't block.
    """
    logger.info("Config web UI starting on http://%s:%d", host, port)
    app.run(host=host, port=port, threaded=True, use_reloader=False)
