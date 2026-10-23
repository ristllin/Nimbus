#!/bin/bash
# SDLOW final bench session on unit A (lane-local; port passed explicitly per the
# 22 Oct scratchpad hygiene rule). L70 both legs, storm leg e x3 in bot mode,
# post-panic check, on the tip image. Evidence under lanes/SDLOW/evidence/.
set -u
P=/Users/roydarnell/Projects/nimbus-project/program-2026-10-18
LANE=$P/lanes/SDLOW
IMG_WT=$P/bench/wt/SDLOW-fcd70a7c
TST_WT=$P/bench/wt/SDLOW-fcd70a7c-tests
PORT=/dev/cu.usbmodem1101
MAC=14:C1:9F:D2:2A:D0
PY=/Users/roydarnell/miniconda3/bin/python3
LABEL=tip-fcd70a7c
EV=$LANE/evidence
LOG=$EV/bench_tip-fcd70a7c_$(date -u +%Y%m%dT%H%M%SZ).log
say(){ echo "[$(date -u +%H:%M:%SZ)] $*" | tee -a "$LOG"; }

# the unit lock: wait for it, hold it for the whole session
while ! mkdir "$P/locks/device-unitA" 2>/dev/null; do say "waiting for device-unitA lock"; sleep 30; done
echo "SDLOW $$" > "$P/locks/device-unitA/owner"
trap 'rm -rf "$P/locks/device-unitA"' EXIT
say "lock held; label=$LABEL image=$IMG_WT tests=$TST_WT runs=3"

bash $P/bench/flash_app.sh $LABEL $MAC $PORT $IMG_WT 2>&1 | tee -a "$LOG"
sleep 45   # let the boot fully settle (relay connect, steady snapshot) before driving the console

export NIMBUS_PORT=$PORT
export NIMBUS_TEST_IP=$(cat /Users/roydarnell/Projects/nimbus-project/secrets/devices/unitA_ip.txt)
export NIMBUS_SD_IMAGE=$LABEL
export NIMBUS_SD_EVIDENCE=$EV/l70
export NIMBUS_ENV_FILE=/Users/roydarnell/Projects/nimbus-project/nimbus/.env
export NIMBUS_STORM_UNIT=A
export NIMBUS_STORM_EVIDENCE=$EV/storm
export NIMBUS_STORM_TG=1
export NIMBUS_STORM_MODEL=claude-haiku-5-5

cd "$TST_WT"

say "l70 legs (ballast + outage)"
# Leg 1 (the ballast leg) has a documented transport-flake start: unit A's console input
# can wedge right after boot (UNFINISHED with no device fault; every card probe and read
# good whenever the console flows). An UNFINISHED attempt is retried, up to three attempts,
# each leaving its own evidence record; the leg itself stays strict.
L70_RC=1
for attempt in 1 2 3; do
  say "l70 attempt $attempt"
  $PY -m pytest tests/hil/test_l70_sd_low_memory.py -m "hil or net" --allow-hardware -s >> "$LOG" 2>&1
  L70_RC=$?
  say "l70 attempt $attempt rc=$L70_RC"
  [ $L70_RC -eq 0 ] && break
  say "l70 attempt $attempt did not pass; resetting the console link for the next attempt"
done
say "l70 rc=$L70_RC"

for i in 1 2 3; do
  say "storm e run $i (bot mode)"
  $PY -m pytest tests/hil/storm/test_storm_e_low_heap.py -m "hil or net" --allow-hardware -s >> "$LOG" 2>&1; say "storm e run $i rc=$?"
  say "errlog read after run $i"
  $PY $LANE/nimbus/scratchpad/SDLOW/errlog_read.py 2>&1 | tail -3 | tee -a "$LOG"
done

say "post-panic check"
$PY $LANE/nimbus/scratchpad/SDLOW/postpanic.py 2>&1 | tee -a "$LOG"
say "panic check done"
say "lock released"
