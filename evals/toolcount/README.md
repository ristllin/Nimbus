# Tool-count benchmark

A host-side, provider-driven benchmark that measures how the **number of tools
exposed to the model** affects tool-selection accuracy and answer quality. It
complements the on-device orchestrator benchmark one level up (`../README.md`):
that one grades real turns on live hardware; this one isolates a single design
variable - tool exposure - against a provider API, needs no board, and is cheap
to run.

## The question

The device advertises its whole tool registry on every Orchestrator turn (see
`catalog.json` - 49 tools today). Frontier-model guidance says selection accuracy
erodes as that set grows and that schemas bloat the context. This benchmark tests
that claim on our real tool surface and scores three ways of shrinking it.

## The three conditions

For each task the model is given one of three tool surfaces:

| condition | what the model sees | models |
|---|---|---|
| `full` | every catalog tool at once (the current device reality) | the status quo |
| `curated` | only the tools flagged `core` (the ~15-20 target set) | a hand-picked core |
| `lazy` | a few always-on tools + one `search_tools` meta-tool; real tools are revealed only after the model searches for them | progressive disclosure |

`lazy` models the tool-search / deferred-loading pattern: the runner answers a
`search_tools` call with the top-k matching tools (lexical rank over the catalog)
and adds them to the exposed set, so the model can then call the real tool. The
extra round-trip is counted as `search_hops`.

## The task set and the two-layer verdict

`tasks.json` is a fixed set. Each task names the one ground-truth tool a correct
turn should call (`expected_tool`), or `null` when the turn should answer directly.
Tasks are tagged by subset:

- `core` - the ideal tool is in the curated set, so all three conditions *can*
  answer it. This is the fair cross-condition comparison and isolates the
  distractor/count effect.
- `coverage` - the ideal tool was cut from the curated set, so `curated` cannot
  answer it while `full` and `lazy` still can. This measures the cost of
  over-curation.
- `none` - no tool should be called; measures over-triggering.

Mirroring the device benchmark's two-layer verdict:

- **Selection check (hard signal, deterministic):** did the model's first real
  tool call equal `expected_tool`? This is the oracle - no LLM judgement.
- **Judge (soft signal, optional, cross-provider):** an independent model scores
  how appropriate the action was, 0..1. It never gates anything.

## Running

Validate the plumbing offline first (no keys, no spend):

```bash
python3 evals/toolcount/run_toolcount.py --mock --reps 2 --out evals/toolcount/runs/mock.jsonl
python3 evals/toolcount/score.py evals/toolcount/runs/mock.jsonl --run mock --date 2026-08-30
python3 evals/toolcount/report.py evals/toolcount/runs/mock.summary.json
```

A real, paid run against providers (keys read from the repo `.env`, never echoed):

```bash
python3 evals/toolcount/run_toolcount.py \
    --models openai:gpt-4o-mini,zai:glm-4.6 \
    --conditions full,curated,lazy --reps 5 \
    --judge zai:glm-4.6 \
    --out evals/toolcount/runs/real.jsonl
python3 evals/toolcount/score.py evals/toolcount/runs/real.jsonl --run real --date 2026-08-30
python3 evals/toolcount/report.py evals/toolcount/runs/real.summary.json
```

Runs under `runs/` are gitignored; commit a `summary.json` and the report as the
tracked evidence. `samples/` holds a tiny mock corpus + summary so a fresh clone
can exercise `score.py` / `report.py` with no keys.

## Corpus row (one JSON object per model x condition x task x rep)

| field | meaning |
|---|---|
| `task`, `cat`, `subset` | task id, category, and subset (`core`/`coverage`/`none`) |
| `condition` | `full` / `curated` / `lazy` |
| `model` | `provider/model` under test |
| `expected_tool` | ground-truth tool, or null for a no-tool task |
| `selected_tool` | the first real (non-meta) tool the model called, or null |
| `selected_correct` | selected_tool == expected_tool (both null counts as correct) |
| `wrong_call` | called a real tool that was not the expected one |
| `finished_no_tool` | ended without calling a real tool |
| `called_tools` | every tool call in order (includes `search_tools`) |
| `search_hops` | `search_tools` calls before acting (the lazy latency tax) |
| `n_tools_exposed` | size of the initially-exposed tool list |
| `prompt_tokens`, `completion_tokens`, `total_tokens` | usage (context-bloat proxy) |
| `latency_ms`, `usd_est` | wall time and a rough spend estimate |
| `judge` | soft cross-provider score `{score, appropriate, rationale}` or null |
| `error` | provider/runner error string, or null |

`score.py` reduces a corpus to a `summary.json`: per (model, condition), and per
subset, the selection accuracy, wrong-call / miss / over-trigger rates, mean
prompt tokens, mean search hops, latency, spend, and judge score. `report.py`
renders it as a table.

## Keeping the benchmark honest

`tests/test_toolcount.py` (host, no network) pins the invariants: catalog names
are unique, the curated set stays in the 15-20 band, every `core` task's tool is
curated and every `coverage` task's tool is not, and lazy search can reach every
non-always tool. If someone edits the tool surface, these tests fail loudly rather
than letting the benchmark drift. Run `python3 -m pytest evals/toolcount/tests`.
