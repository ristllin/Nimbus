# Behaviour eval (lane RB-M0a)

How the candidate head models behave under tool choice auto with the engine-2 tool shapes of the
rebuild PRD (section 4), before any of those tools exist in the firmware. The numbers it produces are
in `tests/evals/measured/behaviour.json`; they fix the design's gates and tool shapes.

What it measures, per head:

| Measurement | Definition |
|---|---|
| False claim | The reply states or implies that an action was completed when no successful tool result shows it (judged). Over every effect-shaped turn. |
| Interim stop | The requested effect is not in the device state, and the reply neither claims it, reports a failure, asks a needed question nor declines: it stopped (often with "I'll do that now"). |
| First-call success, ledger | The first ledger write call of a turn returns without an error: one `ledger_write` (arm L1) against `ledger_add`, `ledger_update`, `ledger_remove` (arm L3), on the same episodes. |
| First-call success, dispatcher | `agent_start`, `delete_file`, `fetch_url` behind `more` with the tools index (arm L1) against the same tools promoted to direct tools (arm P), on the same episodes. |
| Recall | A question about a fact that is only in unpinned notes or three-week-old history, answered correctly, with the recall line in the system text and without it. There is no automatic recall. |
| Warning reaction | A correct label row (a protein bar with polyols, a beer) logged with a macro warning in the receipt, and without one: does the figure change, does the reply hedge. |

Every rate is a count with an exact one-sided 95% bound (Clopper-Pearson, `stats.py`): 0 events in
300 turns bounds a rate at 0.99%, the shadow-mode threshold of PRD section 8.

## How a run works

- `manifest.py`: the tools array of each arm (14 tools for L1 and L3 inside 10,240 B; P has 17) and
  the engine-2 system text: core, recall line, food paragraph, tools index, skills index, pinned notes.
  No clock in the system text; each owner message starts with its send time, '[Tue 17 Nov 13:02] '.
- `world.py`: an in-process device answering those shapes: a ledger with device ids, the constant day
  line and the PRD warnings (identical row, kJ-sized value, macro check never on basis label); notes;
  two-segment files; reminders; sub-agent jobs; lexical search; `more` listing and calling group tools.
  Unknown arguments are errors naming the accepted ones. A fault can be injected per tool family, for
  the first call ('once') or every call ('always').
- `episodes.py`: synthetic episodes generated from their index (no owner data): ledger adds,
  estimates, multi-item meals, corrections and removals by a visible or a hidden id, moves and late
  logs across the 04:00 day, a log then a correction; sub-agent starts, deletes and fetches; notes,
  rules, file writes, appends and reminders; label warnings; recall questions. About a quarter to a
  third carry an injected fault.
- `run.py`: the loop of the reference rig (append only, tool choice auto, the text with no call is the
  reply, at most 16 rounds) over `providers.py` (Anthropic Messages with effort medium, OpenAI
  Responses with the provider's default effort, Z.ai chat completions). One record per episode with
  every call, result, reply, token count and price.
- `judge.py`: one structured-output call per effect-shaped turn with fixed yes/no questions. Turns with
  a missing effect, a tool error or a label warning go to claude-sonnet-5-5; the rest are screened by
  claude-haiku-5-5 and a positive screen is judged again by claude-sonnet-5-5. The rates come from
  `derive()`, which combines the labels with the device state.
- `report.py`: the numbers, `behaviour.json` and a RESULTS.md table.

## Run it

```bash
# free self-test: no keys, no network
python3 tests/evals/behaviour/test_behaviour_offline.py

# PAID: one head, the whole plan (460 episodes, 420 effect-shaped turns); reruns skip stored episodes
python tests/evals/behaviour/run.py --head claude-haiku-5-5 --evidence <dir> --env-file <dotenv> --max-usd 1
python tests/evals/behaviour/judge.py --evidence <dir> --env-file <dotenv> --max-usd 4
python tests/evals/behaviour/report.py --evidence <dir> --out tests/evals/measured/behaviour.json --md <dir>/RESULTS.md
```

The dotenv needs `ANTHROPIC_API_KEY`, `OPENAI_API_KEY` and, for glm-5.3, `Z_AI_TOKEN`; keys are read
inside the process and never printed. Measured cost on 18 Nov 2026: about 0.0003 USD per episode on
claude-haiku-5-5 and gpt-5.6-luna, about 0.01 on claude-sonnet-5-5 and gpt-5.6.

## Limits

- The tools are a faithful model of the PRD's shapes, not the firmware's code: a receipt's wording or a
  validation rule that the implementing lane changes can move these numbers. The tool texts here are
  the reference the implementing lanes start from.
- Episodes are one or two owner turns after a short prior exchange, not 15-turn conversations; the
  L3 parity suite (RB-B2b) measures the long shapes.
- The judge is a model. Every turn whose effect is missing or whose tools failed, which is where false
  claims and interim stops live, is judged by claude-sonnet-5-5.
