# Food coach eval

A scripted, qualitative eval of how the assistant coaches food. It replays short
conversations through the real agent harness with real model calls, then has a
separate judge model grade each transcript against a written rubric. Nothing is
graded by keyword.

It exists because three things went wrong in daily use and a host unit test cannot
see any of them:

- **Answers the question asked.** A direct request for a quantity ("how many grams of
  pasta and how many slices to use up my calories") took several asks before a number
  came.
- **Uses only what the owner has.** A dinner plan added rice, potatoes, roasted
  vegetables and oil that the owner never said they had.
- **Remembers what it was told.** The owner had already listed their food a few messages
  earlier and was asked for it again.

## What it measures

| Scenario | Shape | What a pass looks like |
|---|---|---|
| `qty-vague` | A loosely worded "how much to max out my calories" ask, with a bowl of pasta already in the chat | The amounts (grams of pasta, a cheese amount) come first and use up the day's remaining calories |
| `qty-pressed` | The same ask after a non-answer, from an impatient owner | The amounts in grams and slices come first, with no apology or recap before them |
| `plan-stated` | A dinner plan; the foods the owner has are listed in the same message | Only listed foods are counted in the plan, with quantities that fit the budget |
| `plan-earlier` | A dinner plan; the foods were listed several messages earlier | The earlier list is used and never asked for again |
| `complaint-recovery` | "You invented things I didn't say we have" | One short acknowledgement, then a corrected plan from the listed foods |
| `no-inventory-guard` | A dinner plan when no food was ever named | One short question, or a plan clearly labeled as suggestions: never a confident guess |
| `evening-replay` | The 26 Oct evening: a day of meals after a compaction (a stale summary with an old pending question and another day's total, a 16 Oct meal stored as a standing rule), "the full list by meal", a photo nobody could read, "you estimated earlier, what's the total?" | The list and the total come from today's ledger, the dropped question stays dropped, the photo failure is said, the dinner is written to the ledger, the total adds up, nothing is "not saved" |

The last scenario is a guard: a fix for invented food must not turn into guessing or
into a stall when the owner has named nothing.

A scenario passes a run when every required criterion passes. Advisory criteria are
reported and never gate. A scenario meets the target when at least 4 of its 5 runs pass
(`config.json`).

## How a run works

1. `fcworld.py` builds the world from the scenario file: today's food ledger (the
   device's `food/food-log.csv`, one row per eating occasion), the owner's targets in
   long-term memory, the owner-approved meal skill, any earlier chat turns and, when the
   scenario has them, the owner's standing rules (as the device's file holds them; a
   firmware that moves dated facts out of the rules at boot gets that move) and a
   conversation summary from an earlier fold.
2. `driver/fc_driver.cpp` replays the owner's messages through the **host harness lab**
   (`tools/harness-lab`): the device's own turn engine, prompt composer, tool registry,
   memory engines and provider adapters, with real API calls. The prompt carries what a
   device's does and the lab leaves out: the date and time, the firmware line, the
   hardware manifest and the owner's stored directive. The lab itself is not modified.
3. `fcjudge.py` sends the rubric, the scenario's facts and criteria, and the transcript
   to a judge model in one stateless call. The judge sees none of the assistant's prompt.
   A criterion with no verdict fails.
4. `fcrun.py` saves the record, prices the usage from provider-reported numbers, and
   `fcreport.py` summarizes.

What is real: the system prompt composition, the skills index and the playbooks, the tool
set and its schemas, the provider adapters and their output caps (the reasoning effort is
the one the device sends), prompt caching, the turn-structured history, associative recall
over real embeddings, and the model. What is a stand-in: files and the skill store live in
RAM instead of an SD card, the connector and sub-agent catalogs are empty, and Telegram, voice and
the display are absent. Memory pressure
is not modelled (the lab reports plenty of heap), so a failure that depends on it needs a
board.

The owner's directive is a **stored copy** (`profiles/stored_directive.txt`): a device whose
owner saved the directive keeps that text, and a later change to the shipped default does not
reach it. The eval is run against the stored copy because that is the device it protects.

## Run it

Build the driver once (it builds the lab first) and keep a dotenv file with
`ANTHROPIC_API_KEY` and, for the second judge, `OPENAI_API_KEY`. Keys are read by name and
never printed.

```bash
make -C tests/evals/food_coach/driver
export NIMBUS_ENV_FILE=<dotenv>

python3 tests/evals/food_coach/run_eval.py list
python3 tests/evals/food_coach/run_eval.py run --label try --model <head-model> --reps 1 \
    --judge anthropic/<judge-model> --judge2 none --out <dir>
python3 tests/evals/food_coach/run_eval.py report --label try --out <dir>
```

Models are parameters; `config.json` holds the defaults. Use a small, cheap head for
iterations and the model the device runs for graded numbers. `--judge2` adds second
judges (a comma list) from another provider: the report then shows how often the two agree and a stricter count
where both must pass; with `--judge2-on-pass` the other judges only re-check the runs the first judge
passes, which saves their cost because a failure needs no second opinion.

Each run writes, under `<dir>/<label>/<scenario>/`: the run input (`repN.run.json`), the driver
transcript, the saved record with both judges' verdicts and evidence (`repN.json`), the raw
provider usage (`repN.rec/`) and, for the first repetition, the full system prompt
(`rep1.system.json`). `<label>/summary.json` and `results.md` summarize. A saved run is not
repeated, so an interrupted batch resumes. `rejudge` re-grades saved transcripts with other
judges without calling the head again.

## Device leg

The same scenarios, judge and records can run on a bench unit over its web API, as a
confirmation that the host lab and a real board agree. It needs a `test` build (the tool seam
`/api/test/astool` seeds the memory), a unit that holds an Anthropic key, and nothing else.

```bash
python3 tests/evals/food_coach/run_eval.py run --backend device --unit A --label try-device \
    --model <head-model> --reps 2 --scenarios plan-stated,no-inventory-guard --max-minutes 40
```

- **Only a bench unit.** The unit comes from the bench secrets (`unitA_*` or `unitL_*`); any other
  device is refused, and so is Lumi's address. The leg holds the unit's bench lock (an atomic
  `mkdir` under `NIMBUS_BENCH_LOCKS`) for the whole run and stops starting runs after
  `--max-minutes`.
- **What it changes and puts back.** The head provider and model, the stored directive, the
  monthly spend ceiling (raised only when the month's room is gone), the web chat's history, the
  vector memory, the food log's file project (`food`) and the `meal-photo-science` skill. All of it is
  restored or removed in a `finally`.
- **Earlier chat turns are replayed live.** A device cannot be given assistant replies it never
  wrote, so a scenario's earlier owner messages are sent for real and the unit's own replies are
  the earlier chat the judge reads.
- **Cost** comes from the unit's own usage record (the tokens each provider reported, priced
  with the lab's table), because a device has no provider-side recorder.
- **A parked provider ends the leg.** A unit that learned a billing or key refusal earlier (another
  leg's fault injection, for example) refuses every turn without a request. The leg reads that from
  the unit's turn log, stops, and says to clear it on the console (`PARK clear`), then run again.
- Differences from the lab: the targets reach the unit as a stored memory and the unit's real
  clock, hardware manifest and channel are in the prompt; the unit's other settings (its own
  directive aside) are whatever the bench holds.

## Cost

Real, paid calls. A repetition of one scenario costs about 4 cents on the device's head model
and 1 to 2 cents per judge call; a small head model costs a fraction of a cent. `--max-usd` stops a
batch from starting new runs once the cap is reached. Cost is priced from the providers' own
usage numbers, never from a device ledger.

## Adding a scenario

Add `scenarios/<id>.json` (copy the closest one). Keep the owner's messages short and
in the owner's voice, with generic foods and numbers. A scenario names a food-log set from
`profiles/log_sets.json` (or gives its ledger rows as `ledger_rows`: meal, items, kcal,
protein), may seed earlier chat turns (they must alternate and start with the owner),
standing rules (`prefs`) and a fold summary (`summary`), and lists criteria with
plain-language text; mark a criterion `required` only when a
failure is a failure of the behavior being protected. `python3 -m pytest
tests/evals/food_coach` checks the files, the verdict rules, the pricing and the runner with a
fake driver and a fake judge, with no network.

## Limits

- One repetition is a data point. The target is a pass rate over five.
- The judge is a model. Run a second one and read a few transcripts yourself; the saved
  evidence lines make that quick.
- The host lab is not a board. A result that depends on the device (heap, SD, Telegram
  timing) needs the hardware.
