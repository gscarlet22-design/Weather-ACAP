# ─────────────────────────────────────────────────────────────────────────────
# Weather ACAP — Containerized ACAP v4 (Python)
# Target architecture: linux/arm64 (Axis devices with aarch64 SoCs)
#
# Build for device:
#   docker buildx build --platform linux/arm64 -t weather-acap:latest --load .
#
# Build for local x86 testing/development:
#   docker build --platform linux/amd64 -t weather-acap:dev .
#
# The container uses --network host so it can reach the Axis device's
# VAPIX endpoints at http://localhost/... from inside the container.
# ─────────────────────────────────────────────────────────────────────────────

# Alpine-based image — ~50MB vs ~130MB for slim, keeping the .eap small enough
# to fit in Axis device internal flash (M3086-V, P3245, etc.).
# All app dependencies are pure Python so musl libc is not an issue.
FROM python:3.11-alpine

# Metadata
LABEL maintainer="your-team@example.com" \
      version="1.0.2" \
      description="NWS weather polling ACAP with VAPIX event and overlay integration"

# ── System dependencies ───────────────────────────────────────────────────────
# No system packages needed — pytz bundles its own timezone data,
# and all app deps are pure Python wheels.

# ── Application directory ─────────────────────────────────────────────────────
WORKDIR /app

# Copy requirements first to leverage Docker layer cache — only invalidated
# when requirements.txt changes, not when source changes.
COPY app/requirements.txt ./requirements.txt

RUN pip install --no-cache-dir --no-compile -r requirements.txt

# Copy the rest of the application source
COPY app/ ./

# ── Runtime configuration ─────────────────────────────────────────────────────
# These environment variables provide defaults. Override them via the ACAP
# manifest runOptions or the Axis device's application parameter page.
#
# VAPIX_USER / VAPIX_PASS — credentials for localhost VAPIX calls.
#   Use an Axis account with Operator or Administrator role.
# VAPIX_HOST — default localhost; keep as-is for on-device deployment.
#   Set to the device IP when running off-device for development.
# POLL_INTERVAL_SECONDS — how often to query NWS (minimum 60 recommended).
# MOCK_MODE — set to "true" to read from mock_weather.json instead of NWS.
# LOG_LEVEL — DEBUG | INFO | WARNING | ERROR
ENV VAPIX_HOST="localhost" \
    VAPIX_USER="root" \
    VAPIX_PASS="" \
    POLL_INTERVAL_SECONDS="300" \
    MOCK_MODE="false" \
    LOG_LEVEL="INFO" \
    # NWS requires a User-Agent header identifying your app and contact email.
    NWS_USER_AGENT="WeatherACAP/1.0 (your-email@example.com)" \
    # Python: force unbuffered stdout so logs appear immediately in docker logs.
    PYTHONUNBUFFERED="1"

# ── Healthcheck ───────────────────────────────────────────────────────────────
# The main loop writes a heartbeat timestamp to /tmp/heartbeat.
# docker inspect --format='{{json .State.Health}}' <container> shows status.
HEALTHCHECK --interval=60s --timeout=10s --start-period=30s --retries=3 \
    CMD python -c "import os,time,sys; hb='/tmp/heartbeat'; sys.exit(0 if os.path.exists(hb) and time.time()-os.path.getmtime(hb)<700 else 1)"

# ── Entrypoint ────────────────────────────────────────────────────────────────
CMD ["python", "-u", "weather_acap.py"]
