# refloop - the reference loop rig

The measuring stick for the Nimbus harness: the simplest competent agent loop, and a
side-by-side comparison against the harness on the same model and the same tools.

- **Arm A** is nimbusd's own harness, driven over its chat API the way the web page drives it.
- **Arm B** is `refloop.py` with the same model, calling the tools of its own nimbusd over
  the MCP endpoint (`tools/list`, `tools/call`), on a separate data dir seeded identically.

The rule the rig exists to check: the harness must be at least as good as arm B.

## Run it

```bash
# once: the daemon (unmodified) and the Python SDKs
make -C nimbusd daemon          # needs ArduinoJson under .pio/libdeps (pio test -e native once),
                                # or point at another checkout: make -C nimbusd daemon AJ_SRC=<path>/ArduinoJson/src
python3 -m venv .venv && .venv/bin/pip install -r tools/refloop/requirements.txt

# everything: both arms, every scenario, 3 repetitions, checks, blind judge, tables
.venv/bin/python tools/refloop/compare.py run --evidence <dir> --env-file <dotenv>
```

That one command is PAID (real provider turns on both arms). It stops starting new runs
once `<dir>/spend.jsonl` reaches `--max-usd` (default 40), skips runs already stored, and
ends with a scan of the evidence for key material that must find nothing.

| Option | Meaning |
|---|---|
| `--models a,b` | default `claude-sonnet-5-5,gpt-5.6-luna`; `claude-haiku-5-5` is also set up (`MODELS` in `compare.py`) |
| `--reps N` | repetitions of every scenario (default 3) |
| `--only id,id` | only these scenario ids |
| `--arms A,B` | one arm alone, for a rerun |
| `--engine 2` | arm A's nimbusd runs the owner chat on engine 2 (`NIMBUSD_ENGINE=2`); default `1` |
| `--parallel N` | runs at once, each on its own daemon and port (default 4) |
| `--max-usd N` | the spend ceiling for the evidence dir's ledger |
| `--no-judge` | deterministic checks only |
| `--force` | run and judge again what is already stored |

Other commands:

```bash
compare.py grade  --evidence <dir> --env-file <dotenv>   # grade stored runs again, no new turns
compare.py report --evidence <dir>                       # rebuild RESULTS.md and summary.json
compare.py show   --run <dir>/<model>/<scenario>/rep1/A --turn 4 [--system] [--request N]
                                                         # what the model was shown on that turn
mechanisms.py --evidence <dir>                           # count known harness behaviors in arm A's captures;
                                                         # writes MECHANISMS.md and LOSSES.md (one line per lost repetition)
habits.py --evidence <dir> [--arm A]                     # per run: writes in turns that asked for nothing, calc on
                                                         # one-step sums, a calc result restated as another figure,
                                                         # and the failed checks (no model call)
python3 tests/evals/refloop/test_refloop_offline.py      # the free self-test: no keys, no network
refloop.py --mcp http://127.0.0.1:8787 --token <web token> --model claude-sonnet-5-5
                                                         # chat with the reference loop by hand
```

The dotenv file needs `ANTHROPIC_API_KEY` and `OPENAI_API_KEY`. Both arms' daemons get both:
the head runs on one, and long-term memory embeds with OpenAI `text-embedding-3-small` at
256 wide in both arms. Keys are read inside the Python process and reach a daemon only
through its environment.

## The reference loop (`refloop.py`)

One file. The loop itself is `run_turn`:

1. append what the owner sent (text, and photos as native image parts), stamped with the time in the
   device's own form (`[2026-11-19 Thu 08:10]`, with `(day 2026-11-18 until 04:00)` before the day start;
   `message_stamp`, ruling 64), so both arms see the same stamp;
2. call the model with the whole history, the fixed system prompt and the tool list;
3. append the answer exactly as served (text, thinking blocks with their signatures, tool calls);
4. no tool call: that text is the reply. Otherwise run each call over MCP, append the results, go to 2.

What it deliberately is:

- **System prompt under 1.5 KB**, fixed for the conversation: who the assistant is, when the
  conversation started, the channel. Each owner message carries its own time. Nothing in
  the prompt changes between requests, so the provider cache and thinking signatures hold.
- **Append only.** Nothing is rewritten, trimmed or re-rendered between turns.
- **Native tool calling, tool choice auto**, up to 40 requests in one turn.
- **Prompt caching**: Anthropic gets a cache point on the system prompt and automatic caching
  of the growing history; OpenAI caches by prefix with one `prompt_cache_key` per conversation.
- **One summary** of the oldest half, cut at an owner message, only when a request's prompt
  passes 150,000 tokens. The kept half loses its thinking blocks (a thinking block is bound
  to the history before it).
- **Saved after every step** (`transcript-N.json`): a restart reloads it.
- **A burst**: `serve()` runs one turn at a time; messages that arrive while a turn runs ride
  the next turn together.

Providers: Anthropic Messages (`claude-sonnet-5-5`, `claude-haiku-5-5`) and OpenAI Responses,
stateless (`gpt-5.6-luna`). Effort is the one model knob, and the rig sets it to what the
harness itself sends for that model, read off the wire: `medium` on Claude, no effort field
on gpt-5.6-luna. Thinking stays adaptive. No server-side fallback to another model.

## The engine-2 transcript, emulated (`E2Transcript`)

`refloop.py` also holds `E2Transcript`: the rebuild PRD's section 2 transcript as a pure renderer over
an append-only item list (stub pointer, outside stubs, superseded reads, photo retention, closed-turn
thinking, resident bound H / L per profile with digests, the four cache points). The loop above does
not use it; `tests/evals/replay` drives it to measure what the design costs before it exists in
firmware (see that folder's README and `tests/evals/measured/cost.json`).

## What the MCP surface cannot carry

Arm B gets every tool the registry lists over MCP. The gaps between that and what the
harness's head has:

| The harness has | Over MCP | What arm B does |
|---|---|---|
| `image.look`, `image.note` | listed, but a result is text only, and they read the harness's own photo store | not offered: photos stay in the conversation as image parts |
| `orch_turn` reply fields `mem_write`, `mem_query`, `scratchpad`, `memory` | not tools | the `memory.*` tools do the same jobs |
| sub-agents (`session_ops` spawn, the job engine, results on a later turn) | no tool | none: arm B has no sub-agents |
| `reply.speak`, `reply.telegram`, `device` actions, `ask` | not tools | none (no scenario needs them) |
| provider built-ins and connectors (web search on a sub-agent, Studio connectors) | not in the registry | none |
| `web.search` | listed only with a Tavily key | absent in both arms (the rig sets no Tavily key) |
| automatic recall, the food ledger block, skills attached by the harness | harness context, not tools | arm B calls `memory.search`, `files.*`, `skill.get` itself when it chooses to |
| routines and wake-ups firing as turns | the daemon fires them into its own harness | arm B can create and cancel them; a fired one would run on arm B's daemon harness, not the loop |

## How arm A's requests are captured

nimbusd keeps no request bodies by design and has no base-URL override for Anthropic or
OpenAI, and the rig does not change its source. So the daemon is started with
`capture_shim` interposed on libcurl's `curl_easy_setopt` (`DYLD_INSERT_LIBRARIES` on
macOS): it rewrites only the URL of requests to `api.anthropic.com`, `api.openai.com` and
`api.mistral.ai` to a local proxy (`capture_proxy.py`), which forwards the same method,
headers and body bytes over TLS and writes each exchange to `requests.jsonl.gz`. Arm B's
SDK and arm B's daemon go through the same proxy code, so requests, tokens, cost and
connection reuse are measured one way for both arms. Credential headers are never written.
The shim's Linux path (`LD_PRELOAD`) is written but has not been run.

## Scenarios (`tests/evals/refloop/scenarios/*.yaml`)

Every scenario is synthetic: made-up text, generated photos and files (`fixtures.py`).
Nineteen ship here: the fourteen shapes of the lane brief (the 15 Nov shape twice, with and
without food), plus `long-haul` (twelve 40 KB documents: the reference loop's 150k-token
summary and its cost), `long-chat` (22 long messages: the harness's 48 KB fold), and, since
food is one validation domain among three (owner ruling 41), `expense-day` (meal-log-day's
shape with money: an undo and a refund) and `training-week` (sets over four days, a correction
two days later, the week's volume).

The three validation families (food, expenses, training; twelve scenarios each, the same shapes)
are generated into `tests/evals/refloop/families/<domain>/` by `tests/evals/families/gen_families.py`
and run with `tests/evals/families/run_families.py` (checks only; it also runs arm X, the counter-test
arm whose appends write twice, which must turn them red). Each seeds the domain's tracking file and the
owner's own skill (`seed.skills`).

| Key | Meaning |
|---|---|
| `id`, `title`, `pain` | what it is and which owner pain it stands for |
| `seed.files` | files both instances hold before turn 1: `{project, name, text}` or `{project, name, fixture}` |
| `seed.memories` | facts stored with `memory.write` before turn 1 |
| `seed.skills` | the owner's own skills before turn 1: `{id, md}`, saved as the owner (live at once) |
| `{D0}`, `{D-1}`, `{D-1:dm}`, `{D-1:wd}`, `{RD}` | the run's dates: day 0 is the running day (day start 04:00) when the arm starts; `dm` is "18 Nov", `wd` the weekday; `{RD}` in a check is the running day at that turn's virtual time |
| `turns` | steps, each exactly one of `say`, `burst`, `restart`, `new_conversation`, `advance` |
| `final` | checks on the instance's state after the last turn |
| `judge_notes` | the ground truth the judge is given |
| `models`, `reps` | optional limits for one scenario |

Steps:

| Step | Arm A | Arm B |
|---|---|---|
| `say: text` (+ `photos: [fixture]`) | `POST /api/chat` (photos uploaded to Files and attached), polled until done and idle | `run_turn` with the photos as image parts |
| `say: "{fixture:name}"` | the same, with that text fixture as the message (long made-up text stays out of the YAML) | the same |
| `burst: [a, b, c]`, `gap_s` | the messages posted `gap_s` apart, replies collected until idle | the messages put on the loop's inbox `gap_s` apart |
| (a turn that starts a sub-agent) | the runner waits up to 300 s for the result's own chat message, so the record holds what the owner finally got | no sub-agents |
| `restart: true` | the daemon stops and starts on the same data dir | its daemon restarts and the loop reloads its saved transcript |
| `new_conversation: true` | Clear conversation (`clearConv`), on a new chat key | a new transcript on a new chat key (its provider cache key); memory and files stay; the clock does not go back |
| `advance: N`, `"90m"`, `"04:00"` | its daemon's clock moves (`POST /test/clock`), then idle steps run until no compaction is running or queued | the same on its daemon, and the loop's own stamps move with it |

Checks (`expect` on a turn, `final` on the scenario; table in `grade.py`, one passing and one
failing case each in the self-test): `reply_regex`, `reply_not_regex`, `last_reply_regex`,
`max_replies`, `difference_from_turn`, `tool_called`, `tool_not_called`, `min_tool_calls`,
`file_matching`, `file_contains`, `file_absent`, `files_at_most`, `loops_count`,
`loops_regex`, `loops_not_regex`, `memory_regex`, `memory_not_regex`, `day_rows` (a
day's rows in a CSV file: `count`, each `match` regex finds exactly one row, no row matches a
`none` regex; comment lines are no rows) and `reply_total` (the full total, or the partial total
the read covered together with a mention of the rows it left out; a partial total presented as
complete fails). An unknown key or check stops the load.

A known trap, found by this rig on 17 Nov 2026: on a brand-new nimbusd the files directory
does not exist until the first save, and `files.append` with no `key` then answers `write
failed`. `meal-log-day` seeds one file so that defect is not what it measures.

## Grading

- **Deterministic checks**, the same on both arms, mostly on the instance's state.
- **A blind judge** (`claude-sonnet-5-5`): both transcripts as X and Y with the ground truth;
  1 to 5 on correct, context, tools, honest, concise. Every repetition is judged twice, the
  second time with X and Y swapped, and scored on the mean of the two passes. The judge sees
  owner messages, tool calls with results, replies, the daemon's own notices (labelled as the
  system's), and the memories the harness's automatic recall put in front of the model.
- **Engine 2 on arm A** (`--engine 2`): its file tools count under the old engine's names in
  `tool_called` checks (`read_file` as `files_read`, `grade.ENGINE2_NAMES`), and its notes
  (`<data>/mem/notes/*.md`, the state's `notes`) are read as memory by `memory_regex`, and its pinned
  notes block is shown to the judge as recalled memory. nimbusd has no clear for engine 2, so at a
  `new_conversation` step the rig stops the daemon, moves `<data>/mem/tx` and `compact_ctl.txt` aside into
  the arm folder (`engine2-cleared-cN`) and starts it again: engine 2 starts the new conversation empty,
  with notes and files kept, as engine 1 and the loop do (lane E2-H2H).
- **composite** = half the share of checks passed + half the judge mean scaled to 0..1.
  The harness wins a repetition when its composite is more than 0.05 above the loop's,
  loses when more than 0.05 below, ties otherwise.

## Evidence layout

```
<evidence>/
  RESULTS.md, summary.json     the tables and every number behind them
  MECHANISMS.md, LOSSES.md     from mechanisms.py: harness behaviors counted per scenario; every lost repetition
  spend.jsonl                  one line per paid run and judge call
  <model>/<scenario>/repN/
    grade.json                 checks per arm, both judge passes and their mean, the outcome
    A/turns.json               per step: owner text, replies, latency, requests, tokens, cost, tool calls, harness fields
    A/requests.jsonl.gz        every provider exchange, exact request and response bodies
    A/lastturn-NN.txt          the daemon's own anatomy of each turn
    A/api-log.txt, nimbusd.log the daemon's logs
    B/turns.json, B/requests.jsonl.gz, B/nimbusd.log
    B/transcript-N.json        the loop's saved conversation(s)
```

Cost is priced from provider-reported usage with the list prices in `costs.py`. Runs made
close together share the provider's cache of the fixed prefix, so the measured cost is a
warm figure; `cold_cost_usd` prices the same turn with no cache at all, the other bound.

## Time

Every rig daemon runs with the test clock (`NIMBUSD_TEST_CLOCK=1`; nimbusd/README.md, "One
clock"). An `advance` step takes whole seconds, a duration (`90m`, `8h`, `7d`) or a local wall
time `HH:MM`: the next time that wall time comes round in Europe/London, strictly after the
arm's current virtual time (so `"04:00"` from 23:10 is the next morning, across a DST change
too). `12:00` needs no quotes: the scenario loader does not read YAML 1.1 base-60 numbers. A wall
time lands exactly (`to_epoch`); a duration moves by its length from each arm's own virtual
time, so both arms land on the same wall time. A wall time inside a DST gap or overlap takes
what Python's zoneinfo gives (the first reading); scenarios avoid 01:00 to 02:00 on change
nights. Arm B's system prompt keeps the date it was created with, as it always did; each owner
message carries its virtual time. The move runs one idle step on the engine thread; the rig then runs
more (`advance_s: 0`, at least two) while `/api/fold/status` says a compaction is running or
queued, so the compaction an overnight gap makes due is done before the next turn. A restart
puts the offset back (in steps of at most 366 days); the requests a restart's own idle step
makes are counted on the restart step. Advance only between turns: a pending web turn past its 900 s deadline
is dropped.

A provider's prompt cache lives in real time, so after an advance it is still warm. Each
turn's record therefore carries:

- `cost_usd`: what was billed (the spend ledger's figure);
- `virtual_cost_usd`: the same requests on the virtual clock. A request whose virtual gap
  since the previous request on the same model is over 300 s (`costs.COLD_GAP_S`) is priced
  cold from its token counts: every cached prompt token at the cache-write price, the rest
  as reported. `cold_requests` counts them;
- `chat_key`, `gap_s` (virtual seconds since the previous turn on that chat ended) and
  `cold_transition`: the first turn on a chat key, the turn after a restart, or a gap over
  270 s.

The run record has `virtual_cost_usd` too. The judge sees an advance as
`[time passes: it is now Wed 18 Nov 2026 04:00]`.
