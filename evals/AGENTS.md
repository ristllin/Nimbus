# AGENTS.md - evals

Contributor notes for the orchestrator benchmark. Read `README.md` for what it
measures and `SCHEMA.md` for the result contract.

## Ground rules

- **No diary, no emoji, no first-person.** These files are documentation and tools,
  not a lab notebook. Keep copy present-tense and professional. Per-run narrative
  and dated findings belong in local `ops/`, never in `evals/`.
- **The schema is a contract.** The corpus row and `summary.json` shapes are pinned
  in `SCHEMA.md` and the unit tests. Change the shape only by bumping
  `schema_version` in `summarize.py` and updating `SCHEMA.md` in the same change.
- **One wedge rule.** Wedge/timeout detection lives in exactly one place,
  `report.py::is_wedge` (empty reply OR `oracle.detail` names `timed out` / `wedge`
  / `runner error`). Do not add a second, divergent rule.

## Adding a scenario

Scenarios are defined in [`tests/hil/scenarios_complex.py`](../tests/hil/scenarios_complex.py):

1. Add a `dict(id=..., cat=..., feas=..., needs=[...], host=..., oracle=..., turns=[...])`
   in one of the four categories (`multi-subsession`, `research-artifacts`,
   `resilience`, `connector-chains`).
2. Point `oracle` at a function registered in `ORACLES` in the same file. Add a new
   oracle there if none fits - it takes `(Bundle, scenario) -> (ok, detail)` and must
   check a **ground-truth side effect**, not the reply's wording.
3. Keep the scenario deterministic on the oracle; the judge is a soft signal only.

## Running and reporting

```bash
python3 tests/hil/run_scenarios.py --reps 3 --cat resilience     # run (paid, live)
python3 evals/summarize.py <corpus.jsonl> --run <name> --date <YYYY-MM-DD>
python3 evals/report.py <a.jsonl> [<b.jsonl> ...] --format text
```

## Tests

`python3 -m pytest evals/tests` runs against the redacted corpus in `samples/` - no
hardware, network, or keys. Keep it green, and keep `samples/` free of real
identifiers (a fresh clone must be able to run these tools and tests).
