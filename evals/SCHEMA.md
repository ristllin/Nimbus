# Result schema (version 1)

The benchmark produces two artifacts: a **corpus** (`*.jsonl`, written live by the
runner) and a **summary** (`summary.json`, produced by `summarize.py`). This is the
contract both files honor; `report.py` and `summarize.py` read the corpus, and the
unit tests pin the shapes. Bump `schema_version` and this document together when the
contract changes.

## Corpus row (`*.jsonl`)

One JSON object per scenario x repetition, appended by
[`tests/hil/qa_recorder.py`](../tests/hil/qa_recorder.py) (`Step`). Fields:

| field | type | meaning |
|---|---|---|
| `scenario` | string | scenario id (matches an `id` in `scenarios_complex.py`) |
| `step` | string | repetition label, e.g. `rep0` |
| `prompt` | string | the turns sent, joined with ` \| ` |
| `reply` | string | the final delivered reply (empty string = no reply captured) |
| `host` | string | provider host parsed from the turn anatomy |
| `result` | string | turn result line (`ok` / `fail`) |
| `tools` | list[string] | connector/tool names scraped from the turn anatomy text |
| `tool_calls` | list[string] | real device-side tool calls (episodic `tool_output`) |
| `replies` | list[string] | every delivered reply for the turn, in order |
| `jobs_peak` | int | max concurrent sub-agents seen (0 = fan-out never ran) |
| `heap_min` | int | device `heapMin` during the turn |
| `oracle` | object \| null | the deterministic verdict (below); null if not scored |
| `judge` | object \| null | the LLM-judge verdict (below); null if no judge/key |
| `ts` | float | Unix timestamp when the row was recorded |

### `oracle` object

| field | type | meaning |
|---|---|---|
| `name` | string | oracle function name (e.g. `marker_roundtrip`) |
| `passed` | bool | the hard gate: did the ground-truth side effect occur? |
| `detail` | string | what was observed (also the wedge/timeout signal - see below) |

### `judge` object

| field | type | meaning |
|---|---|---|
| `satisfied` | bool | did the reply satisfy the request? |
| `honest` | bool | do the reply's claims match the oracle ground truth? |
| `score` | number | quality, 0..1 |
| `rationale` | string | one-line explanation (<= 200 chars) |

A judge row may instead be `{"error": "<...>"}` if the judge call itself failed; a
judge failure never fails the gate, and downstream tools treat a missing `honest`
field as "not judged".

## The wedge rule

A row is **wedged** - a device/runner failure, distinct from a dishonest reply -
when `reply` is empty OR `oracle.detail` (lowercased) contains any of `timed out`,
`wedge`, `runner error`. This is the single rule in `report.py::is_wedge`. Honesty
violations are counted only on non-wedged rows.

## Summary (`summary.json`)

Produced by `summarize.py`. Shape:

| field | type | meaning |
|---|---|---|
| `schema_version` | int | `1` |
| `run` | string | experiment id |
| `date` | string | run date (`YYYY-MM-DD`) |
| `fw` | string | firmware describe/sha tested |
| `board` | string | board id |
| `categories` | list[string] | categories the run covered |
| `note` | string | free-form note |
| `totals` | object | `{ran, pass, fail, fail_by_class}` |
| `scenarios` | object | id -> per-scenario entry (below) |

`totals.fail_by_class` maps triage class -> count of failing scenarios in it.

### Per-scenario entry

| field | type | meaning |
|---|---|---|
| `pass` | bool | no repetition failed the oracle and none was judged dishonest |
| `reps` | int | repetitions recorded for this scenario |
| `oracle_fails` | int | repetitions with `oracle.passed == false` |
| `judge_fails` | int | repetitions with `judge.honest == false` |
| `triage` | string | present only on failures; class or `unclassified` |
| `note` | string | present only when a `triage.json` note is supplied |

## Triage overrides (`triage.json`, optional)

Placed next to a corpus; consumed by `summarize.py`. It classifies failures without
changing pass/fail:

```json
{
  "<scenario-id>": {
    "class": "apparatus|behavior|regression|transient|untestable",
    "note": "why this failure is classified this way"
  }
}
```
