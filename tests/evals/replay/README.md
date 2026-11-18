# Replay rig (lane RB-M0b)

What the engine-2 transcript of the rebuild PRD (section 2) costs and how fast it answers, measured
before it exists in firmware, and what engine 1 at main (v5.1.2) costs on the same synthetic days.
The numbers are in `tests/evals/measured/cost.json`; they replace the assumptions of the PRD's budget
table (requests a turn, cold share, tokens a turn, USD a month) and decide the 1-hour cache write and
the hold.

## Inputs

- `shapes/*.json`: the numeric shape of three bad days of the owner's device, written by
  `tools/context_gaps.py shape` from a private corpus that stays off the repo: per owner turn its
  start (seconds after the day's first turn), whether that time is exact (30 Oct has hour stamps
  only, so its turns are spread inside their hour), messages, photos, text bytes, the device's tool
  calls with result sizes, reply bytes and the device's token counts. No text. A test fails if a
  shape holds any other field.
- `synth.py`: made-up owner text of each turn's size and generated 600 x 800 photos (a plate with a
  note, a nutrition label). The kind of a turn follows the shape: photos make a photo log, several
  device tool calls a log, correction, total, search or note, none a chat. The first logging turn
  plants exact quantities and the last turn asks for them back (the 30 Oct failure).

## Engine 2 (`run_replay.py`)

`tools/refloop/refloop.py` `E2Transcript` renders each request from an append-only item list by the
PRD's rules: the stub pointer (cold open over 270 s, warm move over 24,576 B of whole bulk, outside
results stubbed after their turn), photos for the next two owner turns (none after 60 minutes idle),
closed-turn thinking dropped (kept with `--replay-thinking`), receipts whole up to 2,048 B (4,096 B
for ledger tools), the resident bound H / L per profile with one Haiku digest per local day at idle
(120 s quiet, at most two summarizer requests per idle window, 768 B each, the block capped per
profile), and the four cache points (notes block, system, last block of the previous turn, the
moving last block). Tools are RB-M0a's engine-2 world (`tests/evals/behaviour`, arm L1uy: the shapes
ruling 11 binds); the system text is that arm's, with made-up pinned notes up to Lumi's size.

- `--aged`: the transcript starts in steady state: made-up text-only turns up to (H + L) / 2 and a
  full digest block. From an empty transcript one day never reaches lean's H, so the profiles would
  not differ; both are reported.
- `--warm N`: the first N turns 45 s apart, for the warm follow-up arm (with and without replayed
  thinking).

Time is virtual: turns start at their shape time (or when the previous one ended) and the rig does
not wait. Each request is priced from its usage: cold when it is a run's first or comes over 300 s
after the previous head request ended (every cached token then pays the cache-write price), and also
under the 1-hour write (writes at 2 x input, cold only after 3,600 s). Seconds are the real seconds
of the provider requests and the in-process tools: no device, no Telegram.

## Engine 1 (`e1_arm.py`)

Rig arm A (`tools/refloop/arms.py`) on a nimbusd binary built at main: each synthetic turn posted to
its web chat, every exchange captured. Main has no test clock, so the shape's gaps apply in pricing
only: a turn's first head request after a gap over 300 s is priced cold. The daemon's own monthly
ceiling is raised so it cannot clamp the measurement.

## Run it

```bash
PY=<venv with anthropic, openai, pillow>
python3 tools/context_gaps.py --dir <private corpus> gaps --json gaps.json          # free, local
$PY tests/evals/replay/test_replay_offline.py                                         # free self-test
$PY tests/evals/replay/run_replay.py --shape day-2026-11-04 --profile standard --aged \
    --evidence <dir>/e2-aged --env-file <dotenv>                                      # PAID
$PY tests/evals/replay/run_replay.py --shape days-2026-11-15-17 --profile standard --warm 8 \
    [--replay-thinking] --rep 1 --evidence <dir>/thinking --env-file <dotenv>         # PAID
$PY tests/evals/replay/e1_arm.py --daemon <nimbusd at main> --model claude-sonnet-5-5 \
    --evidence <dir>/e1 --env-file <dotenv>                                           # PAID
$PY tests/evals/replay/report.py --e2 <dir>/e2-aged --e1 <dir>/e1 --thinking <dir>/thinking \
    --gaps gaps.json --out tests/evals/measured/cost.json
```

Keys are read inside the process from the dotenv and never printed. Measured cost on 18 Nov 2026:
about 0.01 USD a turn for an engine-2 replay on Sonnet, 0.04 for engine 1 on Sonnet.

## Limits

- The tools are a model of the PRD's shapes (RB-M0a's world), not firmware; result sizes follow the
  world, not the device (a made-up web page is a few KB).
- Aged state is text-only made-up turns: real older turns also hold receipts and stubs, which this
  understates slightly.
- Seconds exclude the device: the arrival path, the hold and the send path (RB-M2 and RB-TS1 measure
  those) and the device's upload rate.
- A synthetic day is not the owner's day: the kinds of turns follow the shape's counts, the content
  is invented.
