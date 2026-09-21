# nimbusd - the hosted Nimbus daemon (Virtual Nimbus).
#
# Multi-stage: a build stage with the toolchain + ArduinoJson + solide-drivers
# headers compiles the daemon; the runtime stage carries only the binary, its
# shared libs (libcurl + libstdc++), CA certificates, and a non-root user.
#
# Build context is the NIMBUS REPO ROOT (the daemon needs lib/core, lib/harness,
# include/, and a sibling solide-drivers checkout):
#   docker build -f nimbusd/Dockerfile -t nimbusd:dev .
#
# The image reads secrets from the environment (a mounted Secret in k8s) and
# persists memory under /data (mount a PVC there). See nimbusd/README.md.
FROM debian:12-slim AS build
RUN apt-get update && apt-get install -y --no-install-recommends \
        g++ make git ca-certificates libcurl4-openssl-dev python3 \
    && rm -rf /var/lib/apt/lists/*
WORKDIR /src
# The engine sources + the daemon.
COPY lib ./lib
COPY include ./include
COPY nimbusd ./nimbusd
# The daemon's test suite reuses the host suite's shared fakes (test/support/);
# without them `make test` cannot build in the image and the release build fails.
COPY test/support ./test/support
# Header-only deps the Makefile expects: ArduinoJson (pinned v7) as the native
# libdep, and the solide-drivers portable headers as a sibling checkout.
ARG ARDUINOJSON_REF=v7.4.3
# Pin the solide-drivers headers to the SAME tag platformio.ini consumes for the
# device build (currently v0.8.1). A stale pin here compiled the daemon against
# older portable headers than the firmware, so a header the engine now needs was
# missing and the image build failed (or, worse, drifted).
ARG SOLIDE_DRIVERS_REF=v0.8.1
# The Makefile resolves ArduinoJson from .pio/libdeps/native and solide-drivers
# as the sibling $(REPO)/../solide-drivers; REPO is /src, so the sibling is
# exactly /solide-drivers.
RUN git clone --depth 1 --branch ${ARDUINOJSON_REF} https://github.com/bblanchon/ArduinoJson.git \
        /src/.pio/libdeps/native/ArduinoJson \
    && git clone --depth 1 --branch ${SOLIDE_DRIVERS_REF} https://github.com/ristllin/solide-drivers.git \
        /solide-drivers
WORKDIR /src/nimbusd
# make clean first: the build context may carry a host (macOS) build/ dir, and a
# stale Mach-O object would fail the Linux link with "file format not recognized".
RUN make clean && make daemon && make test

FROM debian:12-slim AS runtime
# curl (not just libcurl4) is required by the HEALTHCHECK below and by the release
# smoke test: libcurl4 is only the shared library, so the `curl` CLI must be present.
RUN apt-get update && apt-get install -y --no-install-recommends \
        curl libcurl4 ca-certificates tzdata \
    && rm -rf /var/lib/apt/lists/* \
    && useradd -r -u 10001 -m -d /home/nimbus nimbus \
    && mkdir -p /data && chown nimbus:nimbus /data
COPY --from=build /src/nimbusd/build/nimbusd /usr/local/bin/nimbusd
# The image tag this build carries, surfaced by /api/state as `image` (CUM-444) so
# the portal can show the engine version (`fw`) primary and the image tag secondary.
# The release workflow passes the pushed tag; unset in a local/dev build (honestly "").
ARG NIMBUSD_IMAGE_TAG=""
USER nimbus
VOLUME ["/data"]
ENV NIMBUSD_DATA_DIR=/data \
    NIMBUSD_CONTROL_ADDR=127.0.0.1 \
    NIMBUSD_CONTROL_PORT=8787 \
    NIMBUSD_IMAGE_TAG=${NIMBUSD_IMAGE_TAG}
EXPOSE 8787
# Liveness: the ungated health endpoint. Readiness (/readyz) is gated and probed
# by the sidecar/relay, not the kubelet, since it needs the web token.
HEALTHCHECK --interval=30s --timeout=3s --start-period=10s --retries=3 \
    CMD curl -fsS http://127.0.0.1:${NIMBUSD_CONTROL_PORT}/healthz || exit 1
ENTRYPOINT ["/usr/local/bin/nimbusd"]
