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

The last scenario is a guard: a fix for invented food must not turn into guessing or
into a stall when the owner has named nothing.

A scenario passes a run when every required criterion passes. Advisory criteria are
reported and never gate. A scenario meets the target when at least 4 of its 5 runs pass
(`config.json`).

## How a run works

1. `fcworld.py` builds the world from the scenario file: a food log for today (the same
   columns the device keeps), the owner's targets in long-term memory, the owner-approved
   meal skill, and any earlier chat turns.
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
RAM instead of an SD card, and Telegram, voice and the display are absent. Memory pressure
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
iterations and the model the device runs for graded numbers. `--judge2` adds a second judge
from another provider: the report then shows how often the two agree and a stricter count
where both must pass.

Each run writes, under `<dir>/<label>/<scenario>/`: the run input (`repN.run.json`), the driver
transcript, the saved record with both judges' verdicts and evidence (`repN.json`), the raw
provider usage (`repN.rec/`) and, for the first repetition, the full system prompt
(`rep1.system.json`). `<label>/summary.json` and `results.md` summarize. A saved run is not
repeated, so an interrupted batch resumes. `rejudge` re-grades saved transcripts with other
judges without calling the head again.

## Cost

Real, paid calls. A repetition of one scenario costs about 4 cents on the device's head model
and 1 to 2 cents per judge call; a small head model costs a fraction of a cent. `--max-usd` stops a
batch from starting new runs once the cap is reached. Cost is priced from the providers' own
usage numbers, never from a device ledger.

## Adding a scenario

Add `scenarios/<id>.json` (copy the closest one). Keep the owner's messages short and
in the owner's voice, with generic foods and numbers. A scenario names a food-log set from
`profiles/log_sets.json`, may seed earlier chat turns (they must alternate and start with the
owner), and lists criteria with plain-language text; mark a criterion `required` only when a
failure is a failure of the behavior being protected. `python3 -m pytest
tests/evals/food_coach` checks the files, the verdict rules, the pricing and the runner with a
fake driver and a fake judge, with no network.

## Limits

- One repetition is a data point. The target is a pass rate over five.
- The judge is a model. Run a second one and read a few transcripts yourself; the saved
  evidence lines make that quick.
- The host lab is not a board. A result that depends on the device (heap, SD, Telegram
  timing) needs the hardware.
