# Testing tiers

One taxonomy for the whole product (this repo, the cloud repo, notify, solide-drivers).
Every new test states its tier; every tier has one runner.

| Tier | What | This repo | Runs |
|---|---|---|---|
| T0 | Static gates | pre-commit hooks, param-consumer + status-doc + webui-concat gates | every commit |
| T1 | Unit (host, fast) | `pio test -e native` suites without goldens | every commit |
| T2 | Integration (fakes, seams) | native suites exercising module seams (`test_harness_*`, wire codecs) | every commit |
| T3 | Golden / snapshot | `test/golden*`, webui snapshot, QR/nsn vectors | every commit |
| T4 | End-to-end on simulators | host rigs (`tools/harness-lab`), emulated-device tier if adopted | every push |
| T5 | Hardware-in-the-loop | `python3 -m pytest tests/hil -m "hil and not manual" --allow-hardware` (device lock protocol applies) | bench |
| T6 | Live paid (real providers) | evals + HIL "net" layers; each SKIPS cleanly unless its key env var is set (keys via repo-root `.env`) | milestones + gate, budget-capped |

Rules: never mock the thing under test to make a tier pass; hardware claims need hardware
(T5) or an explicit handed-off manual step; T6 results persist to `~/nimbus-evals/`
(JSONL per run with model, scores, token and dollar cost) - never into the repo.

## The release gate

A cross-cutting battery that must be green before any firmware tag or client flash.
It exists because the 2026-08-24 incident shipped a white screen, a universal tunnel
502, a rotated touch surface, and reconnect flapping in one release. Each of those
classes now has a test that fails on the pre-fix build and passes on the fix.

- Host checks (T0-adjacent, run in CI): `python3 tools/release_gate/run_gate.py --host-only`.
  The driver-pin check refuses a build on a known white-screen driver; the ELF check
  keeps the e-paper footprint out of the TFT image. `python3 -m pytest tools/release_gate`
  tests the gate logic.
- On-hardware legs (T5): `tests/hil/test_l29_release_gate.py` - loopback serves the local
  page (not a 502), render reaches the glass (with a recorded human glance), touch lands
  where tapped, a wedged loop is caught by the watchdog, a bad OTA image rolls back.
- Cloud legs (in the cloud repo, run by `pnpm e2e`): the connected-device 5xx interstitial,
  the reconnect storm, and the read-only live smoke.

`tools/release_gate/run_gate.py` prints the full battery with exact commands and the block
condition. See `tools/release_gate/README.md` for what each leg catches.
