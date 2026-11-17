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

1. append what the owner sent (text, and photos as native image parts), stamped with the time;
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
Seventeen ship here: the fourteen shapes of the lane brief (the 15 Nov shape twice, with and
without food), plus `long-haul` (twelve 40 KB documents: the reference loop's 150k-token
summary and its cost) and `long-chat` (22 long messages: the harness's 48 KB fold).

| Key | Meaning |
|---|---|
| `id`, `title`, `pain` | what it is and which owner pain it stands for |
| `seed.files` | files both instances hold before turn 1: `{project, name, text}` or `{project, name, fixture}` |
| `seed.memories` | facts stored with `memory.write` before turn 1 |
| `turns` | steps, each exactly one of `say`, `burst`, `restart`, `new_conversation` |
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
| `new_conversation: true` | Clear conversation (`clearConv`) | a new transcript; memory and files stay |

Checks (`expect` on a turn, `final` on the scenario; table in `grade.py`, one passing and one
failing case each in the self-test): `reply_regex`, `reply_not_regex`, `last_reply_regex`,
`max_replies`, `difference_from_turn`, `tool_called`, `tool_not_called`, `min_tool_calls`,
`file_matching`, `file_contains`, `file_absent`, `files_at_most`, `loops_count`,
`loops_regex`, `loops_not_regex`, `memory_regex`, `memory_not_regex`. An unknown key or
check stops the load.

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
