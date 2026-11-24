#!/usr/bin/env bash
# build_all.sh - the full-matrix build check.
#
# Compiles every environment + every flag path on one command so debug/flag code
# ("WiFi undeclared", fuel-gauge glue, the NIMBUS_TEST affordances) can't rot
# unnoticed between hardware sessions. This is a COMPILE-ONLY gate: it never
# uploads, never opens a serial port, never touches the (currently bricked)
# board - it only proves each target still links.
#
# The matrix (F15 - "debug/flag paths not in any build-matrix check"):
#   native                              host unit-test toolchain (compiles every
#                                       test program; `pio run -e native` has no
#                                       sources to build and always failed)
#   esp32s3                             production firmware
#   notifierdbg                         esp32s3 + NIMBUS_NOTIFIER_DEBUG (status echo)
#   provision                           serial provisioning / STA test tool
#   esp32s3 + -DNIMBUS_HAS_FUEL_GAUGE   the src/hw/power_fuelgauge path (no gauge
#                                       on the board yet, so it never builds by
#                                       default - this is the ONLY thing that
#                                       compiles it)
#   test  (esp32s3 + -DNIMBUS_TEST)     the HIL serial test-affordances layer
#   esp32s3-cyd, esp32s3-cyd-35,        the Freenove CYD product images (engine 1) and the CYD
#   esp32s3-cyd-40, test-cyd            test console: gen_e1_symbols.py --check reads all six
#                                       engine-1 envs and their -e2 twins
#   esp32s3-e2, esp32s3-cyd-e2,         the engine-2 images (lane RB-I0): each is its
#   esp32s3-cyd-35-e2, esp32s3-cyd-40-e2, engine-1 twin built with custom_nimbus_engine = 2
#   test-e2, test-cyd-e2                and -DNIMBUS_ENGINE=2 (never both engines in one image)
#
# After the matrix, the engine gates run on what it built (PRD section 10, L0): every map
# links one engine (check_engine_filter.py), no engine-2 ELF defines an engine-1 symbol
# (check_elf_symbols.py --engine1-absent), no old-engine tool registers on engine 2
# (list_tool_registrations.py --check), and the engine-2 images keep the tg_poll stack floor
# and their static-DRAM ceilings (undeclared engine-2 growth fails on a lane branch), and the
# image projection prints what engine-2 parity would add.
#
# The two flag-only targets reuse an existing env and inject the extra define via
# PLATFORMIO_BUILD_FLAGS (appended to that env's build_flags for this run only),
# so production 'esp32s3' behaviour is never modified on disk.
#
# Usage:   tools/build_all.sh            # build the whole matrix
#          tools/build_all.sh -v         # verbose (stream pio output)
# Exit code is non-zero if ANY target fails to compile.

set -u -o pipefail

# Repo root = this script's parent dir (works from any CWD).
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT" || exit 2

VERBOSE=0
[ "${1:-}" = "-v" ] && VERBOSE=1

command -v pio >/dev/null 2>&1 || {
  echo "ERROR: 'pio' (PlatformIO Core) not found on PATH." >&2
  exit 2
}

# Static gates FIRST (fail fast, no compile needed): every user-facing knob must
# have a real consumer (no lying knobs), and the web UI must concatenate cleanly.
echo "== static gates =="
python3 tools/check_param_consumers.py || { echo "== param-consumer gate FAILED ==" >&2; exit 1; }
python3 tools/webui_concat_check.py    || { echo "== webui concat check FAILED =="  >&2; exit 1; }

# Each row: "<label>|<env>|<extra build flags>". Empty flags => plain env build.
# The flag rows come first: PlatformIO removes the WHOLE build directory (every env) when the
# project configuration changes, and PLATFORMIO_BUILD_FLAGS is part of it. With the flag rows
# after the plain ones, the plain esp32s3, notifierdbg and provision builds were gone before the
# gates below read them (the engine gates need the plain esp32s3 map and ELF). Every row after
# the last flag row is a plain build and stays on disk.
MATRIX=(
  "native|native|"
  "esp32s3+fuelgauge|esp32s3|-DNIMBUS_HAS_FUEL_GAUGE"
  "esp32s3+battadc|esp32s3|-DNIMBUS_HAS_BATTERY_ADC"
  "esp32s3|esp32s3|"
  "notifierdbg|notifierdbg|"
  "provision|provision|"
  "esp32s3+test|test|"
  "test-cyd|test-cyd|"
  "esp32s3-cyd|esp32s3-cyd|"
  "esp32s3-cyd-35|esp32s3-cyd-35|"
  "esp32s3-cyd-40|esp32s3-cyd-40|"
  "esp32s3-e2|esp32s3-e2|"
  "esp32s3-cyd-e2|esp32s3-cyd-e2|"
  "esp32s3-cyd-35-e2|esp32s3-cyd-35-e2|"
  "esp32s3-cyd-40-e2|esp32s3-cyd-40-e2|"
  "test-e2|test-e2|"
  "test-cyd-e2|test-cyd-e2|"
)
E2_ENVS=(esp32s3-e2 esp32s3-cyd-e2 esp32s3-cyd-35-e2 esp32s3-cyd-40-e2 test-e2 test-cyd-e2)

pass=0
fail=0
failed_labels=()

# The host env has no firmware sources (build_src_filter = -<*>), so build its
# test programs instead; every firmware env is a plain compile + link.
build_env() {
  if [ "$1" = native ]; then
    pio test -e native --without-testing
  else
    pio run -e "$1"
  fi
}

echo "== Nimbus build matrix (compile-only; no upload) =="
for row in "${MATRIX[@]}"; do
  IFS='|' read -r label env flags <<<"$row"
  printf '  [ .. ] %-20s (env:%s%s)\n' "$label" "$env" \
    "${flags:+ +$flags}"

  # Inject extra flags for this single invocation only; unset immediately after
  # so the next env builds clean. `pio run` (no -t upload) compiles + links only.
  if [ -n "$flags" ]; then
    export PLATFORMIO_BUILD_FLAGS="$flags"
  fi

  if [ "$VERBOSE" -eq 1 ]; then
    build_env "$env"
    rc=$?
  else
    log="$(build_env "$env" 2>&1)"
    rc=$?
    [ $rc -ne 0 ] && printf '%s\n' "$log"
  fi

  unset PLATFORMIO_BUILD_FLAGS

  if [ $rc -eq 0 ]; then
    printf '  [ OK ] %-20s\n' "$label"
    pass=$((pass + 1))
  else
    printf '  [FAIL] %-20s (exit %d)\n' "$label" "$rc"
    fail=$((fail + 1))
    failed_labels+=("$label")
  fi
done

# harness-lab: the agent harness built as a host executable. It links the same
# lib/core + lib/harness the firmware compiles, so a lib/core refactor that drops
# a source it needs breaks its link - and nothing else builds it, so that rot
# stays silent until the next live run (CUM-268). Build-only: `make check` never
# opens a socket or reads a key. It needs the native ArduinoJson libdeps, which the
# 'native' matrix row above has just populated.
printf '  [ .. ] %-20s (make check)\n' "harness-lab"
if [ "$VERBOSE" -eq 1 ]; then
  make -C tools/harness-lab check
  rc=$?
else
  log="$(make -C tools/harness-lab check 2>&1)"
  rc=$?
  [ $rc -ne 0 ] && printf '%s\n' "$log"
fi
if [ $rc -eq 0 ]; then
  printf '  [ OK ] %-20s\n' "harness-lab"
  pass=$((pass + 1))
else
  printf '  [FAIL] %-20s (exit %d)\n' "harness-lab" "$rc"
  fail=$((fail + 1))
  failed_labels+=("harness-lab")
fi

# Image-size gate after the builds: every built env's firmware.bin must fit the app
# partition of ITS OWN partition table (lane TESTFIT - the test-console builds
# overflow the stock 6.5 MB slot while PlatformIO's section report stays
# green, and the ROM then refuses to boot the image). FAIL on overflow and on a product
# image under 200,000 B free (the release floor, lane E2-GATE), WARN under 300,000 B on a
# product image and under 64 KB elsewhere; one table per env: image, free, the code change
# against image_baseline.json and the allowance that explains it.
# The image-size gate and the engine gates (lane RB-I0). Each runs bare; a failure fails the
# matrix, and every gate still runs.
gate() {
  local label=$1; shift
  printf '  [ .. ] %-20s\n' "$label"
  if "$@"; then
    printf '  [ OK ] %-20s\n' "$label"
    pass=$((pass + 1))
  else
    printf '  [FAIL] %-20s\n' "$label"
    fail=$((fail + 1))
    failed_labels+=("$label")
  fi
}
echo "== image-size gate (every built env vs its app partition) =="
gate "image size" python3 tools/release_gate/check_image_size.py
echo "== engine gates (engine-1 and engine-2 images) =="
gate "engine-filter maps" python3 tools/release_gate/check_engine_filter.py \
  --require-maps esp32s3 esp32s3-cyd test "${E2_ENVS[@]}"
gate "e2 tool registrations" python3 tools/engine/list_tool_registrations.py --check
gate "e1_symbols.txt current" python3 tools/engine/gen_e1_symbols.py --check
for env in "${E2_ENVS[@]}"; do
  gate "e1-absent $env" python3 tools/release_gate/check_elf_symbols.py --engine1-absent \
    --elf ".pio/build/$env/firmware.elf"
done
gate "tg_poll stack" python3 tools/release_gate/check_tg_poll_stack.py --quiet
gate "static DRAM" python3 tools/release_gate/check_static_dram.py
# Report only (exit 0 unless a map cannot be read): the engine-1 feature code not yet in the
# engine-2 image, by group, and the engine-2 image with every returning group at engine-1 size.
echo "== image projection (engine 1 against engine 2, Freenove 2.8 inch) =="
gate "image projection" python3 tools/release_gate/image_projection.py \
  .pio/build/esp32s3-cyd/firmware.map .pio/build/esp32s3-cyd-e2/firmware.map \
  --bin .pio/build/esp32s3-cyd-e2/firmware.bin --top 15

echo "== matrix: $pass passed, $fail failed =="
if [ $fail -ne 0 ]; then
  echo "failed: ${failed_labels[*]}" >&2
  exit 1
fi
exit 0
