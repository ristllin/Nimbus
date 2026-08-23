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
