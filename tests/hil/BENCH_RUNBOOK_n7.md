# N7 bench runbook - live before/after intLargest/heapMin + 24 h soak

Turnkey steps for the CUM-24 before/after and the CUM-36 soak. Read-only measurement
uses `tests/hil/sram_sampler.py`. Whoever runs this holds the device lock per the
fleet-lane protocol (create `/tmp/nimbus-devices/<port>.lock`, 45 min max per hold,
release after). Nimbus-4 (`usbmodem2101`) is the personal board - restore its NVS from
`~/nimbus-bench/nimbus4-backup/` when done. Freenove (`usbmodem101`) is the scratch board.

## What win #2 should show
Static: internal DRAM data+bss 104,307 -> 97,595 B (-6,712 B), already proven at build time.
Runtime expectation: under identical load, the `after` build shows ~6.7 KB higher
`mem.intFree` and a `mem.intLargest`/`heapMin` floor no worse (ideally higher) than `before`,
with NO regression in `mem.pollStackMin` / `mem.asyncStackMin`. No heap-floor constants changed.

## Firmwares
- BEFORE = baseline `main` @ `4143ebf`, `test` env.
- AFTER  = `lane/N7` @ HEAD (`69a6883`), `test` env.
Build: `pio run -e test` (solide_s3 / Nimbus-4) and `pio run -e test-cyd` (Freenove).

## Procedure (repeat per board; do BEFORE then AFTER on each)
1. Lock the port; USB-reset-clear if the console is wedged (CUM-141 pyusb recipe).
2. Flash the firmware under test: `pio run -e <test|test-cyd> -t upload --upload-port <port>`.
3. Provision Wi-Fi (HIL `net.py` provision, or the device captive portal), get the LAN IP.
4. Start load: turns + web + BLE active. Use the mock LLM to avoid paid spend -
   `tests/hil/mock_llm.py` + the scenario runner drives turns; hit a couple of web
   endpoints; keep a BLE scan/notify active. (Measuring heap under load, not turn quality,
   so mock turns are the right tool and cost $0.)
5. Sample for ~10-15 min:
   `python3 tests/hil/sram_sampler.py --ip <ip> --token "$NIMBUS_ADMIN_TOKEN" \
       --minutes 15 --interval 20 --label <before|after> --out n7_<board>_<phase>.jsonl`
6. Release the lock.
7. Diff: `python3 tests/hil/sram_sampler.py --diff n7_<board>_before.jsonl n7_<board>_after.jsonl`
   Acceptance: intFree.min up ~+6-7 KB; intLargest.min and heapMin.min not lower;
   pollStackMin/asyncStackMin.min not lower.

## 24 h soak (CUM-36, orchestrator-run - exceeds the 45 min lock)
On the AFTER build, both boards, with turns+web+BLE active for 24 h:
`python3 tests/hil/sram_sampler.py --ip <ip> --token "$NIMBUS_ADMIN_TOKEN" \
    --hours 24 --interval 60 --out n7_<board>_soak.jsonl`
Pass = no downward drift in intFree/intLargest/heapMin (no leak), no reset, floors held.
Then refresh the `docs/memory.md` measured matrix with the observed numbers.
