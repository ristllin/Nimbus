# tests/hil - the hardware-in-the-loop suite

Python pytest suite that drives a real Nimbus board over its serial test console and its LAN web API. The full
contract lives in `pytest.ini` (markers), `conftest.py` (gates and fixtures), `docs/testing-tiers.md` (the tier
taxonomy, T5/T6) and the gate text in `tools/release_gate/run_gate.py`. This README is the entry point.

## Running it

```bash
# collection must stay clean with no board attached:
python3 -m pytest tests/hil --collect-only -q

# host tier only (what CI runs, pytest alone, no board, no pyserial):
python3 -m pytest tests/hil -q

# a device leg (needs the board, the lock protocol and --allow-hardware):
python3 -m pytest tests/hil -m "hil and not manual" --allow-hardware --port /dev/cu.usbmodemXXXX
python3 -m pytest tests/hil -m "net and not manual" --allow-hardware          # LAN legs
python3 -m pytest tests/hil -m manual --allow-hardware                        # human-assisted steps
python3 -m pytest tests/hil -m destructive --allow-hardware                   # factory-resets the board; run alone
```

- The port comes from `--port` or `NIMBUS_PORT`. Never rely on the auto-glob: with one board plugged the harness
  would open whatever is there. Identify the board by MAC first (`tools/bench_guard.py list`,
  `tools/bench_guard.py port-for-mac <mac>`); a port name is not an identity.
- LAN-only mode: set `NIMBUS_TEST_IP` and `NIMBUS_TEST_TOKEN` (the device token travels only in the
  `X-Nimbus-Token` header; `?t=` and `Bearer` are dead since CUM-45) and `net` legs run without serial.
- Provider keys come from the repo-root `.env` by exact name (`NIMBUS_ENV_FILE` overrides the path). Values are
  scrubbed from every report (`redact.py`, `secrets.py`); never print, log or commit one.
- Every hardware marker is gated behind `--allow-hardware`; without it the tests collect and skip loudly, so a
  device-less box stays green and honest.

## The forbidden-device guard

Two boards are never a target: the owner's other project and Lumi, the production device. Every tool in this tree
and in `tools/` that opens, resets, flashes or scans a USB port asks `tools/bench_guard.py` first, through
`tests/hil/port_guard.py`. The guard reads the USB serial number from the OS listing and never opens the port, so
asking cannot disturb any board. `tests/hil/test_bench_guard_host.py` proves each entry point refuses, with a fake
bus and a fake pyserial whose open is recorded: a refusal means no open happened. Do not write a forbidden MAC into
any runner, test or doc; the host test fails on one.

## The bench drivers (coverage/BENCH-5.0.0.md rows)

The 5.0.0 bench plan's rows that had no command now have one each, all host-self-tested against
`fake_bench_device.py` (a real HTTP server on localhost plus a console stand-in; no board, no port):

| Command | Row | What it does |
|---|---|---|
| `python3 tests/hil/soak_24h.py --ip IP --token-file F --out DIR [--port P --mac MAC]` | K01 | the 24 h soak: raw polls, turns, errlog reads on disk, a case-insensitive alloc-failure scanner, grades PASS/OPEN/BLOCKED/FAIL from the files alone (`--grade DIR` recomputes) |
| `python3 tests/hil/onboarding_gate.py --port P --mac MAC --out DIR --env-file F` | H27 | factory-fresh unit to a first answer, every step asserted, screen-driven through the console (`--no-console` for a production image: the person answers the screen steps) |
| `python3 tests/hil/ota_rehearsal.py --ip IP --token-file F --out DIR --port P --mac MAC --url U --want-build B` | B01 | the update rehearsal from v4.6.4 as one command, settings store at Lumi's level |
| `python3 tests/hil/bench_rows.py <ROW> --ip IP --token-file F --out DIR [--port P --mac MAC]` | 5.1 rows | one command per former "no command" row (E01 cable pull gets a precise prompt plus automatic before/after checks); `bench_rows.py list` shows them |
| `python3 -m pytest tests/hil/test_ota_freenove.py -m net --allow-hardware` | B04 | the Freenove twin of `test_ota.py` |

Host self tests: `test_soak_24h_host.py`, `test_onboarding_gate_host.py`, `test_bench_guard_host.py`,
`test_bench_tooling_host.py` (all `host`-marked, run by CI with pytest alone).
