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

**Not** simulated: the LEDs, the e-ink panel, Telegram, and the ~46 KB internal
heap. `freeHeap` reports a large value so the device's memory gates never fire.
Anything that depends on memory pressure has to be tested on hardware.

## Build

```bash
cd tools/harness-lab && make
```

Needs libcurl (present on macOS) and an ArduinoJson checkout under `.pio/libdeps`
- run `pio test -e native` once if the build cannot find it.

`make check` compiles without touching the network: run it after harness changes
so the lab cannot quietly rot.

## Keys

Read from the environment first, then a dotenv file (default
the secrets `.env` - `NIMBUS_ENV_FILE`, the same one the HIL suite uses). Nothing is committed.

    OPENAI_API_KEY  ANTHROPIC_API_KEY  MISTRAL_API_KEY  TAVILY_API_KEY

A provider with no key is skipped, not faked.

## Use

```bash
./build/nimbus-lab providers                 # do all three actually answer?
./build/nimbus-lab search "world news"       # web.search end to end
./build/nimbus-lab chat                      # interactive REPL
./build/nimbus-lab scenarios                 # the whole suite
./build/nimbus-lab scenarios memory tool-loop
./build/nimbus-lab list-scenarios
```

Useful flags: `--host=mistral` pins one provider (no failover, so a failure is
attributable), `--model=<id>` overrides its model, `--verbose` echoes every HTTP
exchange and tool call, `--no-tools` disables the agentic loop, `--no-embed`
turns off embeddings.

## The scenarios

These are not smoke tests. "The API returned 200" is exactly the bar that let a
permanently-broken `web.search` ship, so each scenario asserts an outcome.

| scenario | what it proves |
|---|---|
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
