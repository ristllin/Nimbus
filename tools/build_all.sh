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
#   native                              host unit-test toolchain
#   esp32s3                             production firmware
#   notifierdbg                         esp32s3 + NIMBUS_NOTIFIER_DEBUG (status echo)
#   provision                           serial provisioning / STA test tool
#   esp32s3 + -DNIMBUS_HAS_FUEL_GAUGE   the src/hw/power_fuelgauge path (no gauge
#                                       on the board yet, so it never builds by
#                                       default - this is the ONLY thing that
#                                       compiles it)
#   test  (esp32s3 + -DNIMBUS_TEST)     the HIL serial test-affordances layer
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
MATRIX=(
  "native|native|"
  "esp32s3|esp32s3|"
  "notifierdbg|notifierdbg|"
  "provision|provision|"
  "esp32s3+fuelgauge|esp32s3|-DNIMBUS_HAS_FUEL_GAUGE"
  "esp32s3+battadc|esp32s3|-DNIMBUS_HAS_BATTERY_ADC"
  "esp32s3+test|test|"
)

pass=0
fail=0
failed_labels=()

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
    pio run -e "$env"
    rc=$?
  else
    log="$(pio run -e "$env" 2>&1)"
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

echo "== matrix: $pass passed, $fail failed =="
if [ $fail -ne 0 ]; then
  echo "failed: ${failed_labels[*]}" >&2
  exit 1
fi
exit 0
