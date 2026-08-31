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
ARG SOLIDE_DRIVERS_REF=v0.5.1
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
RUN apt-get update && apt-get install -y --no-install-recommends \
        libcurl4 ca-certificates tzdata \
    && rm -rf /var/lib/apt/lists/* \
    && useradd -r -u 10001 -m -d /home/nimbus nimbus \
    && mkdir -p /data && chown nimbus:nimbus /data
COPY --from=build /src/nimbusd/build/nimbusd /usr/local/bin/nimbusd
USER nimbus
VOLUME ["/data"]
ENV NIMBUSD_DATA_DIR=/data \
    NIMBUSD_CONTROL_ADDR=127.0.0.1 \
    NIMBUSD_CONTROL_PORT=8787
EXPOSE 8787
# Liveness: the ungated health endpoint. Readiness (/readyz) is gated and probed
# by the sidecar/relay, not the kubelet, since it needs the web token.
HEALTHCHECK --interval=30s --timeout=3s --start-period=10s --retries=3 \
    CMD curl -fsS http://127.0.0.1:${NIMBUSD_CONTROL_PORT}/healthz || exit 1
ENTRYPOINT ["/usr/local/bin/nimbusd"]
