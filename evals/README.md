# Nimbus orchestrator benchmark

A reproducible evaluation of the on-device orchestrator's real behavior - honesty,
security, sub-agent fan-out, and connector use - run against a fixed scenario corpus
on live hardware. Each run produces a machine-readable corpus that reduces to a
comparable summary and renders to a human report.

## The two-layer verdict

Every scenario turn is graded twice, and the layering is the point:

- **Oracle - the hard gate.** A deterministic, ground-truth side-effect check
  (marker round-trip, a real file appearing at `GET /api/files/dl`, an error-free
  `/api/log`, a fan-out peak > 0). The oracle catches a lying "Done!" that no
  amount of fluent prose can talk its way past. A test asserts on the oracle.
- **Judge - a soft signal.** An independent, cross-provider LLM scores each reply
  for quality and honesty, given the prompt, the reply, the real device-side tool
  trace, and the oracle verdict. It is deliberately run on a *different* provider
  from the one under test, so a model is never its own judge. The judge never gates
  a build; it flags dishonesty and quality drift the oracle cannot express.

A scenario passes only when no repetition fails the oracle and none is judged
dishonest.

## The scenario corpus

The 51 scenarios live in [`tests/hil/scenarios_complex.py`](../tests/hil/scenarios_complex.py),
in four categories:

| category | focus |
|---|---|
| `multi-subsession` | sub-agent fan-out, review-then-synthesize, cross-turn recall |
| `research-artifacts` | research, citation, artifact save/deliver |
| `resilience` | degraded capabilities, refusals, honesty under failure |
| `connector-chains` | provider-hosted connector delegation (Gmail, Notion, Drive, ...) |

Each scenario names a deterministic `oracle` (a function in `ORACLES`) and is also
scored by the judge. Scenarios declare a `feasibility` (`runs_now` / `honesty_test`
/ `owner_setup`) and a `host` provider to pin.

## Triage vocabulary

A failing scenario is triaged so history can tell a harness fault from a device
fault. Triage never changes pass/fail - it only classifies the failure:

| class | meaning |
|---|---|
| `apparatus` | the test rig was wrong (bad prompt, bad oracle, flaky fixture) |
| `behavior` | suboptimal device behavior, but not a regression |
| `regression` | the device got worse than a previous known-good run |
| `transient` | a one-off (network blip, provider 429) unlikely to reproduce |
| `untestable` | cannot be judged on this device/config (e.g. trace needs an SD card) |

Overrides are supplied in a `triage.json` next to a corpus (see `SCHEMA.md`).

## Cost and time warning

This benchmark drives **real, paid LLM turns on live hardware**. A full run costs
provider spend and takes roughly minutes per scenario (sub-agent fan-out and
connector turns are the slowest). It is not a unit test - run it deliberately.

## Running a benchmark

The runner is [`tests/hil/run_scenarios.py`](../tests/hil/run_scenarios.py). It
drives each scenario over HTTP against a device on the LAN, applies the oracle,
invokes the judge, and appends a JSONL corpus (one row per scenario x repetition)
under `tests/hil/qa_runs/`.

```bash
python3 tests/hil/run_scenarios.py --reps 3 --cat multi-subsession
```

See the runner's `--help` for scenario/category selection, repetition count, and
preflight gating. Provider keys and device address come from the HIL secrets
fixture, never the command line.

## Turning a run into a report

```bash
# reduce a corpus to the comparable summary point (summary.json)
python3 evals/summarize.py <corpus.jsonl> --run <name> --fw <describe> \
    --board <id> --date <YYYY-MM-DD> --cats <a,b>

# render one or more corpora to a report (text | md | html); 2+ files show deltas
python3 evals/report.py <a.jsonl> [<b.jsonl> ...] --format text
```

`summarize.py` writes a `summary.json` (see `SCHEMA.md`) - per-scenario pass/fail
plus totals broken out by triage class. `report.py` prints headline metrics, a
per-category breakdown, and a per-scenario grid; with multiple corpora it prints
the first-to-last delta.

## Reading a result

- **Corpus** (`*.jsonl`) - the raw record, one JSON object per scenario x
  repetition. Fields are documented in `SCHEMA.md`.
- **Summary** (`summary.json`) - `totals.pass` / `totals.fail`, `fail_by_class`,
  and a `scenarios` map with each scenario's pass, repetition count, and oracle /
  judge failure counts.
- **Report** - headline oracle-pass and mean-judge, wedged count, and the grid.
  A `P` is a pass, `F` a fail, `-` a scenario absent from that run.

## Known limitations

- **N = 1 variance.** A live LLM benchmark is noisy; a single run is a data point,
  not a proof. Compare across repetitions and runs.
- **Trace needs an SD card.** Some oracles read the episodic tool trace, which is
  SD-gated. On a card-less board those scenarios are `untestable`, not failures.
- **The judge is a soft signal.** It can be wrong. It never gates a build; the
  oracle does.

## Layout

```
evals/
  README.md            this file
  AGENTS.md            contributor notes
  SCHEMA.md            the versioned corpus + summary contract
  summarize.py         corpus.jsonl -> summary.json
  report.py            corpus(es) -> text | md | html report
  samples/             a small redacted corpus + its summary (fresh-clone runnable)
  tests/               unit tests for summarize.py + report.py
  runs/                archived run artifacts (gitignored, local only)
```
