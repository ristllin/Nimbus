#!/usr/bin/env bash
# battery2.sh - the rebuild's regression battery, by tier (PRD section 11; lane RB-B0b).
#
#   tools/battery2.sh --tier free  [--log DIR] [--base-ref REF] [--declared PR_BODY.md]
#                                                                 # L0 static + L1 host properties
#   tools/battery2.sh --tier live                                 # L2 conformance + L3 parity
#   tools/battery2.sh --tier bench                                # L4 bench + L5 soak
#
# free: every gate runs with its real exit status, nothing piped into a gate, nothing
# excluded: the whole `pio test -e native` (the L1 property suites run inside it; a test
# waiting for its lane reports PENDING RB-<lane> and is on test/pending_roster.txt), then
# the pending-roster gate over that run, the host builds (nimbusd, harness-lab) and the
# static gates. A gate whose lane has not landed yet prints "PENDING <lane>: <gate>" and
# is listed in the final line; it never counts as run. The last line is the marker:
#   BATTERY2 FREE GREEN <sha> (<n> gates run; pending: ...)   exit 0
#   BATTERY2 FREE RED <sha>: <failed gates>                  exit 1
# live and bench answer "NOT BUILT" with the lane that builds them and exit 2: a tier that
# does not exist is never reported green.
set -u

TIER=""
LOGDIR=""
BASE_REF="rb-main"
DECLARED=""
while [ $# -gt 0 ]; do
  case "$1" in
    --tier|--log|--base-ref|--declared)
      [ $# -ge 2 ] || { echo "battery2: $1 needs a value" >&2; exit 2; } ;;
  esac
  case "$1" in
    --tier) TIER=${2:-}; shift 2 ;;
    --log) LOGDIR=${2:-}; shift 2 ;;
    --base-ref) BASE_REF=${2:-}; shift 2 ;;
    --declared) DECLARED=${2:-}; shift 2 ;;
    -h|--help) sed -n '2,21p' "$0"; exit 0 ;;
    *) echo "battery2: unknown argument $1" >&2; exit 2 ;;
  esac
done

ROOT=$(cd "$(dirname "$0")/.." && pwd)
cd "$ROOT" || exit 2
SHA=$(git rev-parse --short HEAD 2>/dev/null || echo "nogit")

case "$TIER" in
  free) ;;
  live) echo "BATTERY2 LIVE NOT BUILT: L2 and L3 arrive with lane RB-B1 (nightly-parity.yml, check_parity.py)"; exit 2 ;;
  bench) echo "BATTERY2 BENCH NOT BUILT: L4 and L5 arrive with lanes RB-B3a and RB-B3b"; exit 2 ;;
  *) echo "battery2: --tier free|live|bench is required" >&2; exit 2 ;;
esac

[ -n "$LOGDIR" ] || LOGDIR=$(mktemp -d "${TMPDIR:-/tmp}/battery2.XXXXXX")
mkdir -p "$LOGDIR" || exit 2
SUMMARY="$LOGDIR/battery2-free.log"
: > "$SUMMARY"
say() { echo "[$(date -u +%H:%M:%SZ)] $*" | tee -a "$SUMMARY"; }

FAILED=""
PENDING=""
RAN=0

# gate <name> <log file> <command...>: run it, keep its own log, record its real exit status.
gate() {
  local name=$1 log=$2
  shift 2
  say "-- $name"
  "$@" > "$log" 2>&1
  local rc=$?
  RAN=$((RAN + 1))
  say "   rc=$rc ($log)"
  [ $rc -eq 0 ] || FAILED="$FAILED $name"
}

# needs <path> <lane> <name>: true when the gate's script exists; else it is pending, loudly.
needs() {
  if [ -e "$1" ]; then return 0; fi
  say "PENDING $2: $3 ($1 not in this tree)"
  PENDING="$PENDING $3($2)"
  return 1
}

say "== battery2 free tier for $SHA in $ROOT (logs: $LOGDIR)"

# L1: the whole native suite, no filter, no exclusion.
NATIVE_LOG="$LOGDIR/native.log"
gate native "$NATIVE_LOG" pio test -e native
grep -E "test cases:" "$NATIVE_LOG" | tail -1 | tee -a "$SUMMARY"

# L0: the pending roster equals the ignored set of that run, and only shrinks.
if needs tools/release_gate/check_pending_roster.py RB-GATE0 pending-roster; then
  gate pending-roster "$LOGDIR/pending-roster.log" \
    python3 tools/release_gate/check_pending_roster.py --native-log "$NATIVE_LOG" --base-ref "$BASE_REF"
  grep -E "PENDING ROSTER (GREEN|RED)" "$LOGDIR/pending-roster.log" | tee -a "$SUMMARY"
fi

# L1 on the hosts: nimbusd and the harness lab link the same lib/core and lib/harness.
gate nimbusd-test "$LOGDIR/nimbusd-test.log" make -C nimbusd test
gate harness-lab "$LOGDIR/harness-lab.log" make -C tools/harness-lab check

# L0 static gates that exist today.
gate param-consumers "$LOGDIR/param-consumers.log" python3 tools/check_param_consumers.py
gate status-doc "$LOGDIR/status-doc.log" python3 tools/check_status_doc.py
gate webui-concat "$LOGDIR/webui-concat.log" python3 tools/webui_concat_check.py

# L0 gates of lanes still to land: listed, never silently skipped.
BRANCH=$(git rev-parse --abbrev-ref HEAD 2>/dev/null || echo "")
if needs tools/release_gate/check_lane_files.py RB-GATE0 lane-files; then
  if [ "${BRANCH#lane/}" = "$BRANCH" ]; then
    say "-- lane-files: not on a lane/<id> branch ($BRANCH): applies to lane branches only"
  elif [ -n "$DECLARED" ]; then
    gate lane-files "$LOGDIR/lane-files.log" \
      python3 tools/release_gate/check_lane_files.py --base-ref "$BASE_REF" --declared "$DECLARED"
  else
    gate lane-files "$LOGDIR/lane-files.log" python3 tools/release_gate/check_lane_files.py --base-ref "$BASE_REF"
  fi
fi
if needs tools/release_gate/check_no_prompt_patch.py RB-B5 no-prompt-patch; then
  gate no-prompt-patch "$LOGDIR/no-prompt-patch.log" python3 tools/release_gate/check_no_prompt_patch.py
fi
if needs tools/release_gate/check_plain_ceilings.py RB-E2 plain-ceilings; then
  gate plain-ceilings "$LOGDIR/plain-ceilings.log" python3 tools/release_gate/check_plain_ceilings.py
fi
# The ELF scan exists; its engine-1 symbol list for e2 images arrives with RB-I0.
needs tools/engine/e1_symbols.txt RB-I0 e1-symbols-in-e2-image || true

if [ -n "$FAILED" ]; then
  say "BATTERY2 FREE RED $SHA:$FAILED"
  exit 1
fi
say "BATTERY2 FREE GREEN $SHA ($RAN gates run; pending:${PENDING:- none})"
exit 0
