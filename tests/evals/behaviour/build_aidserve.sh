#!/bin/bash
# build_aidserve.sh <build dir> [ArduinoJson src dir]: builds aidserve (the real engine-2 file tools and
# request composition behind a pipe, for the records arms) against nimbusd's core objects. Offline.
# The core objects are compiled by nimbusd's own Makefile rules into <build dir>/core.
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
REPO=$(cd "$HERE/../../.." && pwd)
OUT=${1:?build dir}
AJ=${2:-}
mkdir -p "$OUT"
ARGS=(-C "$REPO/nimbusd" "BUILD=$OUT")
[ -n "$AJ" ] && ARGS+=("AJ_SRC=$AJ")
# Compile every core object (the daemon's rule); the daemon itself is not linked.
read -r -a OBJS <<<"$(make "${ARGS[@]}" -s -p -n daemon 2>/dev/null | sed -n 's/^CORE_OBJS := //p' | head -n1)"
make "${ARGS[@]}" -s -j"${JOBS:-4}" "${OBJS[@]}" >/dev/null
AJI=${AJ:-$(make "${ARGS[@]}" -s -p -n daemon 2>/dev/null | sed -n 's/^AJ_SRC := //p' | head -n1)}
CURL_PREFIX=$(curl-config --prefix 2>/dev/null || true)
read -r -a LIBS <<<"$(curl-config --libs 2>/dev/null || echo -lcurl)"
[ -n "$CURL_PREFIX" ] && LIBS+=("-Wl,-rpath,$CURL_PREFIX/lib")
c++ -std=gnu++17 -O1 -DNIMBUS_NATIVE -I"$REPO/nimbusd/src" -I"$REPO/lib/harness/include" -I"$REPO/lib/core/include" \
  -I"$REPO/include" -I"$REPO/../solide-drivers/include" -I"$AJI" "$HERE/aidserve.cpp" "${OBJS[@]}" "${LIBS[@]}" -lpthread \
  -o "$OUT/aidserve"
echo "built $OUT/aidserve"
