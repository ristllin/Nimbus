# harness-lab - run the Nimbus agent on your Mac

A host build of the device's agent harness. The same `TurnEngine`, tool registry,
memory engines and provider adapters the firmware runs, wired to libcurl instead
of `WiFiClientSecure` and to RAM instead of an SD card.

It exists because every harness investigation before it went the same way: change
something, build, flash, wait, watch a 1280-byte log ring wrap, guess. The
`web.search` bug that broke every news briefing had been shipping for weeks and
was found in minutes here - the response is 7–8 KB and the old adapter's buffer
stopped at 6000.

## Why the code under test is the real code

Every provider call, tool call and web search already goes through one injected
seam, `agent::HttpTransport`. The device implements it over TLS sockets; the lab
implements it over libcurl. Nothing above that seam knows the difference, so a
bug reproduced here is a bug on the board.

```
  scenarios / REPL
        |
   agent::TurnEngine          <-- lib/harness   (identical on device)
   tool registry, memory      <-- lib/core      (identical on device)
   provider adapters          <-- lib/harness   (identical on device)
        |
   agent::HttpTransport       <-- THE SEAM
        |
   CurlHttpTransport  |  TlsTransport (WiFiClientSecure)
       (this lab)     |      (the device)
```

**Not** simulated: the LEDs, the panel, Telegram, and the ~46 KB internal
heap. `freeHeap` reports a large value so the device's memory gates never fire.
Anything that depends on memory pressure has to be tested on hardware.

## Build

```bash
cd tools/harness-lab && make
```

Needs libcurl (present on macOS) and an ArduinoJson checkout under `.pio/libdeps`
- run `pio test -e native` once if the build cannot find it.

`make check` needs no keys and no network: it builds the lab, runs its offline
self-test (`nimbus-lab selftest`), the Python tool tests, and the model-id lint
(`tools/model_id_lint.py`). It is a battery row, so run it after harness changes
and the lab cannot quietly rot.

## Keys

Read from the environment first, then a dotenv file (default
the secrets `.env` - `NIMBUS_ENV_FILE`, the same one the HIL suite uses). Nothing is committed.

    OPENAI_API_KEY  ANTHROPIC_API_KEY  MISTRAL_API_KEY  Z_AI_TOKEN  TAVILY_API_KEY

A provider with no key is skipped, not faked.

## Use

```bash
./build/nimbus-lab providers                 # do all three actually answer?
./build/nimbus-lab search "world news"       # web.search end to end
./build/nimbus-lab chat                      # interactive REPL
./build/nimbus-lab scenarios                 # the whole suite
./build/nimbus-lab scenarios memory tool-loop
./build/nimbus-lab list-scenarios
./build/nimbus-lab --host=anthropic sub "find the sources for X"   # one sub-agent, end to end
```

`sub` runs one Anthropic sub-agent through the shipping spawn chain
(`providers::antDispatch`: environment, agent, session, first turn) and polls it
with `antPoll` until it finishes, printing the reply and the harness log. Add
`--model=<id>` to pick the agent model, `--nvs-full` to drop every cache write
the way a full NVS partition does (CUM-481 reproduced here first), `--runs=<n>`
to prove a warm spawn re-creates nothing, and `--record=<dir>` to save every
response body (the source of the recorded fixtures under
`test/support/fixtures/anthropic_managed_agents_2026-10/`). Each run is a real
session: a one-line reply costs about $0.02, a short research task about $0.12.

Useful flags: `--host=mistral` pins one provider (no failover, so a failure is
attributable), `--model=<id>` overrides its model, `--verbose` echoes every HTTP
exchange and tool call, `--no-tools` disables the agentic loop, `--no-embed`
turns off embeddings.

## Live legs: model, budget, faults and spend

Models are parameters. With nothing set, each host runs its cheapest current
model (Anthropic `claude-haiku-5-5`, Mistral `mistral-small-latest`, Z.ai
`glm-5.3-flash`, OpenAI `gpt-5.6-luna`) and the priority is
`anthropic,mistral,zai,openai`. Every live run prints one `lab:` line saying
exactly what it ran with.

| Pin | Environment | Flag |
|---|---|---|
| Model per host (`[host/]model`, host inferred from the id; one pinned model also becomes the head) | `NIMBUS_LAB_MODEL` | `--host` + `--model` |
| Anthropic effort (`low` by default; `adapter` sends what the device sends) | `NIMBUS_LAB_EFFORT` | `--effort=` |
| Output cap per request | `NIMBUS_LAB_MAXTOK` | `--max-tokens=` |
| Per-request timeout ceiling | `NIMBUS_LAB_CEILING_MS` | `--ceiling-ms=` |
| Head tool-loop rounds (1..32) | | `--rounds=` |

The device sends no effort and caps Anthropic output at 3000 tokens, and adaptive
thinking counts toward that cap, so live legs pin a low effort until the
adapters carry their own cap table. The pins are applied to the request on its
way out (`src/lab_wire.h`); nothing under `lib/` changes.

LOOP turns run the device's mid-turn failover (the fabric loop, `--no-failover`
turns it off). `--fault=host:range:status[:detail]` fails chosen requests so the
retry and failover paths run against otherwise real traffic, for example
`--fault=anthropic:2:529:overloaded` or `--fault=mistral:1-3:0:noresp` (grammar in
`lib/harness/include/nimbus/harness/fault_transport.h`; repeatable).

`--record=<dir>` appends every provider usage object to `<dir>/usage.jsonl` and
every provider error body to `<dir>/errors.jsonl` (secret-shaped strings
scrubbed). Spend is priced from those provider-reported numbers, never from a
device ledger:

```bash
NIMBUS_LAB_MODEL=claude-haiku-5-5 ./build/nimbus-lab --record=/tmp/rec scenarios smoke
python3 spend_ledger.py price /tmp/rec/usage.jsonl
python3 spend_ledger.py add --ledger ledger.jsonl --lane N --purpose smoke \
    --caps anthropic=0.5,openai=0.05,mistral=0.1,zai=0.1 /tmp/rec/usage.jsonl
python3 spend_ledger.py route mistral --ledger ledger.jsonl   # mistral, or anthropic on overflow
```

`spend_ledger.py` keeps per-provider caps; a provider over its cap, or marked
refused for credit (`spend_ledger.py refuse`), hands its legs to Anthropic.
`record_to_fixtures.py` turns recorded runs into the scrubbed fixtures under
`test/support/fixtures/usage_2026-10/`.

## The scenarios

These are not smoke tests. "The API returned 200" is exactly the bar that let a
permanently-broken `web.search` ship, so each scenario asserts an outcome.

| scenario | what it proves |
|---|---|
| `smoke` | a plain reply and a memory write checked by its effect: the cheapest live-leg wiring check |
| `web-search` | a live search succeeds AND the model uses what came back |
| `search-errors` | a failing search names its cause instead of saying "no results" |
| `multi-tool` | several tool calls inside one turn, and the side effects landed |
| `memory` | written in one chat, recalled in a different one |
| `memory-ops` | write / search / pin / update / episodic, incl. update replacing rather than duplicating |
| `tool-loop` | a multi-round loop where round 2 depends on round 1's result |
| `continuity` | the chat still remembers three turns later |
| `scheduled` | an unattended routine turn - the news-briefing path |
| `dream` | a quiet consolidation turn is allowed to say nothing |
| `parity` | every keyed provider handles a plain turn and a tool turn |

Scenarios needing a Tavily key **skip loudly**. A silent skip reads as a pass,
which is how a broken capability stays broken.

Real API calls cost real money. The suite reports its token totals; a full run is
roughly 100 K input tokens, dominated by the system prompt on every turn.

## Cost of the fakes

Two things here are host stand-ins, not the device's code, so treat their results
as indicative:

- **File tools** (`lab_files.h`) - the device's `files.*`/`artifact.save` are
  SD-backed and cannot be lifted. The tool *shape* matches and the real
  `FileStore::validSegment` traversal gate is reused, but the bytes live in a
  `std::map`.
- **Episodic store** - `InMemoryEpisodicStore`, not the SD append-log. The
  conversation window is rebuilt per turn from it.

Everything else - the turn engine, prompt composition, the tool loop, provider
wire formats, vector memory, RBAC, `web.search` - is the shipping code.

## Gotchas paid for here

- `LabRig` is header-only, so the Makefile **must** track header dependencies
  (`-MMD -MP`). Without it, editing `lab_rig.h` rebuilds only the `.cpp` files
  that changed, the other translation unit keeps its stale inline definitions,
  and the linker picks one - you end up debugging a fix that is in the source but
  not in the binary.
- Without an `ApplyDeps::principalFor` hook, `whoFor` returns a deny-all
  principal and every memory call comes back *"this conversation isn't approved
  to store memories yet"*. That is the correct fail-safe, and it is what the lab
  reported before the hook was wired - not a device bug.
