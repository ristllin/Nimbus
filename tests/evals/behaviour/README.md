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
| Follow-up ledger arms | The same ledger episodes with the same model-facing bytes as L1: L1u (every op takes `id` or `ids`, an add takes `day`, `time`, `meal` beside `row`), L1y (the message stamp carries the year), L1uy (both). |
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
- `audit.json`: the hand audit of every judged positive. `report.py` applies its overrides and also
  reports the judge's raw counts.
- `report.py`: the numbers, `behaviour.json` and a RESULTS.md table.

## Run it

```bash
# free self-test: no keys, no network
python3 tests/evals/behaviour/test_behaviour_offline.py

# PAID: one head, the whole plan (460 episodes, 420 effect-shaped turns); reruns skip stored episodes
python tests/evals/behaviour/run.py --head claude-haiku-5-5 --evidence <dir> --env-file <dotenv> --max-usd 1
# PAID: the follow-up ledger arms (100 episodes each)
python tests/evals/behaviour/run.py --head claude-haiku-5-5 --evidence <dir> --env-file <dotenv> --max-usd 1 --follow-up L1u,L1y,L1uy
python tests/evals/behaviour/judge.py --evidence <dir> --env-file <dotenv> --max-usd 4
python tests/evals/behaviour/report.py --evidence <dir> --out tests/evals/measured/behaviour.json --md <dir>/RESULTS.md
```

The dotenv needs `ANTHROPIC_API_KEY`, `OPENAI_API_KEY` and, for glm-5.3, `Z_AI_TOKEN`; keys are read
inside the process and never printed. Measured cost on 18 Nov 2026: about 0.0003 USD per episode on
claude-haiku-5-5 and gpt-5.6-luna, 0.005 on claude-sonnet-5-5 and 0.009 on gpt-5.6.

## Limits

- The tools are a faithful model of the PRD's shapes, not the firmware's code: a receipt's wording or a
  validation rule that the implementing lane changes can move these numbers. The tool texts here are
  the reference the implementing lanes start from.
- Episodes are one or two owner turns after a short prior exchange, not 15-turn conversations; the
  L3 parity suite (RB-B2b) measures the long shapes.
- The tools index names groups this world does not implement (people, models, docs, connectors); a
  `more` list call on one is left out of first-call success as a rig artifact.
- Episodes are paired across arms and drawn from a few templates, so the turns are not independent
  samples: the bounds are optimistic about variance across templates.
- The judge is a model. Every turn whose effect is missing or whose tools failed, which is where false
  claims and interim stops live, is judged by claude-sonnet-5-5.

# The general-records arms (lane RB-REC6)

Under ruling 42 tracking of any kind is an ordinary CSV file and the plain file tools. These arms measure
that, in three domains of equal weight (food, expenses, training) with invented content carrying the
owner file's real-row classes at about his rates (ruling 49: about 11% of rows with more fields than the
header, 3% with an empty summed cell, stray blank lines, about 12% `#` history lines). The numbers are in
`tests/evals/measured/behaviour.json` under `records_rb_rec6`; the decisions they feed are in the lane's
REPORT.md.

| Arm | What the head gets |
|---|---|
| F | files only: read_file and edit_file as declared before RB-REC4 (no column/from/to/sum, no line); the model sums |
| S | rb-main's declarations: read_file column/from/to/sum, edit_file line |
| Sp, Sq, Spq | S with the sum clause rewritten by the rig (candidate wordings, rulings 88 and 92) |
| Sn | the selection without sum (ruling 94): read_file without `sum`; run with `--aidserve` built from `lanes/RB-REC6/evidence/part4/orch_csv_sum.sn.cpp` (a selected row that does not parse is a row of the result) |
| K0 | S tools, a fresh chat, no skill and no note naming the table |
| K1 | K0 plus the owner's skill (about 3.5 KB with front matter) in the skills index |
| K2 | the same skill marked `inject: context`: the old engine's `[ACTIVE SKILLS]` block rides the owner's message |
| K3 | K2 with two user skills attached at once (one 4,096 B block, the second cut) |

The tools, the system text and every file tool, calc and `more` answer are the REAL engine-2 code:
`aidserve.cpp` runs FileTools over nimbusd's PosixFileToolsIo, `plain::composeSystem`, `plain::toolManifest`
and `agent::skillsAttachText` behind a JSON-lines pipe, one process per episode. A refused allocation is
the real `not enough memory for this call; nothing changed` (the call under a refusing working allocator;
for append and write, which take no working block, the core's stated refusal).

| File | What it is |
|---|---|
| `records.py` | domains, file generation, the owner's invented skill, episodes, the checks (`judge_total`, `judge_write`, `totals_after_fix`) |
| `run_records.py` | one cell: episodes through a head, one record per episode with the file bytes before and after |
| `judge_records.py` | claude-sonnet-5-5 labels the write turns that can hold a false claim (a fault, or a file not as asked) |
| `judge_totals.py` | claude-sonnet-5-5 decides, for a reply that states the device's partial sum, whether it says rows were left out (the regex in `records.names_left_out` is the fallback) |
| `report_records.py` | applies the checks to the stored records and prints the tables with exact 95% bounds |
| `scripted_heads.py` | offline heads for dry runs: `good` and `sloppy` |
| `aidserve.cpp`, `build_aidserve.sh` | the real tools behind a pipe; built against nimbusd's core objects |

```bash
python3 tests/evals/behaviour/test_records_offline.py                       # free: checks, both sides
tests/evals/behaviour/build_aidserve.sh <build dir> [ArduinoJson src dir]     # offline build
AIDSERVE=<build dir>/aidserve python3 tests/evals/behaviour/test_records_offline.py   # + every arm, scripted
# PAID, one cell:
python3 tests/evals/behaviour/run_records.py --head claude-haiku-5-5 --arm S --evidence <dir> --env-file <dotenv> \
  --aidserve <build dir>/aidserve --work <scratch> --max-usd 1 --totals 150 --writes 190 --faults 30
python3 tests/evals/behaviour/judge_records.py --evidence <dir> --env-file <dotenv> --max-usd 1
python3 tests/evals/behaviour/judge_totals.py --evidence <dir> --env-file <dotenv> --max-usd 1
python3 tests/evals/behaviour/report_records.py --evidence <dir> --json <out.json>
```

Limits: episodes are one owner turn (two with a prior log), not long conversations; the K2 and K3 block is the
old engine's mechanism carried onto engine 2's shapes (its text names skill_get, which engine 2 does not have);
the total check reads figures from the reply (a regex finds the number, with dates, clock times and line
references taken out first); whether a partial total names the rows it left out is judged by a model.
