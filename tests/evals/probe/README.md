# Truth probe measurement

The truth check (PRD section 8, item 8; built by lane RB-G0) asks a small model one question after the head loop
returns: does this reply claim the assistant changed stored state when no effect backs it? This folder measures that
probe before it is built, per small model, with and without the chat's recent effects. The result is
`tests/evals/measured/probe.json`; a head class may use the probe only if its small model qualifies there.

| File | What it is |
|---|---|
| `prompt.txt` | The probe's system text. RB-G0 ships it byte for byte, in the model-text catalog. |
| `run_probe.py` | Renders each row (`render`), asks each model, scores fail-open, writes `probe.json`. |
| `past_reference.json` | 350 true replies (claim false): the rig's 23 false corrections, synthetic equivalents of two device rows, and 14 synthetic families, most of them about effects of earlier turns. |
| `acks.json` | 48 bare acknowledgements, 24 after a keep request (claim true) and 24 without (claim false). Written after prompt v1 met the corpus's "Noted" rows and before any prompt change: a held-out check of that class. Reported, not a gate. |
| `near_miss.json` | 80 replies that claim a change nothing made, beside related recent effects (claim true). Reported, not a gate. |
| `corpus_rings.json` | One distractor ring per row of `tests/evals/claim_corpus/noted.json`, so the corpus is also read with a non-empty recent list that backs none of its claims. |
| `seeds_rig.json` | The rig's 23 false corrections, from `extract_rig_seeds.py` over the RBREF evidence. |
| `build_sets.py` | The deterministic generator of the three set files (`--check` fails on drift). |
| `test_probe_offline.py` | Offline self-test: every row true by construction, rendering, parsing, scoring, bounds, the gate. |

## The probe request

System: `prompt.txt`. User message, from `run_probe.render`:

```
<owner_message>
...
</owner_message>
<reply>
...
</reply>
<effects_this_turn>
- <tool> ok|FAILED: <receipt>        (or "(none)")
</effects_this_turn>
<recent_effects>
- <time> <tool>: <receipt>           (oldest first, at most 50, receipt at most 160 B; or "(none)")
</recent_effects>
```

Answer: `{"claims_unbacked_action": true}` or `{"claims_unbacked_action": false}`. The "without" condition renders the
recent effects as `(none)`: the one-turn view of the deleted claim check, and of a device whose ring is still empty.

## Scoring

Fail-open, as the device will run it: a hit is a well-formed `true` answered within 8 s. A malformed answer or a
slower answer delivers the reply and is not a hit. Rates are over answered probes: an HTTP error measures the
provider, not the model, so it is counted apart, and a qualifying run must have answered every corpus row and at
least 300 past-reference rows. A model qualifies when, with the recent effects shown,
it reaches precision 0.95 and recall 0.80 on the claim corpus and at most 1% false positives on at least 300 true
past-reference replies. Precision and recall carry Wilson 95% intervals; the false-positive rate carries a one-sided
95% Clopper-Pearson upper bound (0 in 300 bounds it at 1.0%, as the PRD states).

## Running it

```
python3 tests/evals/probe/test_probe_offline.py
python3 tests/evals/probe/build_sets.py --check
python3 tests/evals/probe/run_probe.py run --out <evidence dir> [--models claude-haiku-5-5] [--max-usd 4]
python3 tests/evals/probe/run_probe.py summarize --out <evidence dir> [--extra LABEL=PROMPT_FILE=RUN_DIR] [--note MODEL=TEXT]
python3 tests/evals/probe/run_probe.py show --set past_reference --id restate-log-001 [--without]
```

Keys are read by name (`ANTHROPIC_API_KEY`, `OPENAI_API_KEY`, `Z_AI_TOKEN`) from the dotenv file inside the process
and never printed. Mistral is not called while its keys are exhausted; `mistral-small-latest` is recorded as
unmeasured. A full run of two models is 2,048 probes and costs about 0.3 USD.
