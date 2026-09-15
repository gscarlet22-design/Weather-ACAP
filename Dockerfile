# ─────────────────────────────────────────────────────────────────────────────
# Weather ACAP — Native ACAP v4 build using ACAP Native SDK
#
# Build aarch64 (CV25, ARTPEC-8, ARTPEC-9):
#   docker build --build-arg ARCH=aarch64 -t weather-acap-build:aarch64 .
#   container=$(docker create weather-acap-build:aarch64)
#   docker cp "$container:/opt/app/." dist/ && docker rm "$container"
#
# Build armv7hf (ARTPEC-7):
#   docker build --build-arg ARCH=armv7hf -t weather-acap-build:armv7hf .
#
# No AXIS Container Runtime required on the camera — this is a native binary.
# ─────────────────────────────────────────────────────────────────────────────

ARG ARCH=aarch64
FROM axisecp/acap-native-sdk:1.14-${ARCH}-ubuntu22.04 AS builder

COPY app/ /opt/app/

WORKDIR /opt/app

# Source the SDK cross-compilation environment and run acap-build.
# The glob matches exactly one environment-setup-* file per architecture.
# Stamp the target architecture into the manifest (the field is hard-coded to
# aarch64 in the repo; CI does the same with jq).  -a weather_acap.cgi is
# REQUIRED: acap-build only packages the appName binary by default and
# silently omits the FastCGI backend, leaving the web UI at HTTP 500.
RUN sed -i "s/\"architecture\": *\"[a-z0-9]*\"/\"architecture\": \"${ARCH}\"/" manifest.json \
 && . /opt/axis/acapsdk/environment-setup-* \
 && acap-build . -a weather_acap.cgi
