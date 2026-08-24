<!-- audience: dev -->
# Turn anatomy - what the model actually sees, and how to inspect it raw

Every orchestrator turn is assembled from scratch on the device. When a reply seems
to come out of nowhere ("what are you talking about?"), the answer is always in one
of the blocks below - and you can see the **exact raw text** of the most recent
turn at:

```
GET http://<device>/api/lastturn         (token-gated; the LAST turn, from RAM)
GET http://<device>/api/trace?turn=<id>   (token-gated; ANY recent turn, from SD)
```

Both return one plain-text page with four sections: the exact system prompt, the
exact per-turn input, the model's raw `orch_turn` JSON, and (since v4.2.0) the
**tool-loop transcript** - the mid-turn rounds, with each tool call's arguments
and its result. Captured for a completed turn, success or failure.

`/api/lastturn` is a single RAM slot that the next turn overwrites.
`/api/trace?turn=<id>` is the same anatomy kept **per turn** on the SD card
(`/mem/trace/<turnId>.txt`), so you can open a turn from hours ago.

In the web **Chat** tab you don't need either URL: every turn's disclosure has a
**Turn anatomy** button that fetches its own dossier, and any clipped tool result
has **Show full result**. Under each reply a chip reports the host, model,
tool-call count and the turn's real billed tokens.

**Turn identity.** Every row of a turn - the reply, each tool call, each thinking
row - is tagged `turn:<user row id>` (e.g. `turn:m0000a3f2`), which is the id of
the turn's own user message row. It is stable across reboots, so the chat groups
a bubble with its trace by identity. (Before v4.2.0 the tag was `turn:tN` from a
RAM counter that reset every boot and the UI grouped by chronological adjacency,
which mis-filed rows whenever a Telegram or voice turn interleaved with a web one.)

Turn details are **SD-gated** and gated on **Activity recording** (Assistant →
Tools → Tool use, `otrace`, default on). With either missing the chat says so
rather than showing an empty panel; the dossier ring keeps the most recent 16
turns (≤768 KB), and older ones report `evicted`.

A byte-pinned copy of the full system-prompt template lives in the goldens:
`test/golden/orch_prompt_default.txt` (tool-loop ON) and
`test/golden/orch_prompt_recall_loopoff.txt`, plus
`test/golden/orch_prompt_v2_default.txt` (the simplified v2 prompt, behind the
`promptV2` flag - see [harness.md](harness.md)) - regenerated with
`GOLDEN_UPDATE=1 pio test -e native -f test_harness_compose`. Those files ARE the
raw template, kept honest by CI.

For the whole lifecycle around a turn (boot, ticks, sub-agents, loops, dreaming,
watchdogs) see [harness.md](harness.md); for the sub-agent side see
[sub-sessions.md](sub-sessions.md).

## The three parts of a turn

### 1. INSTRUCTIONS - the system prompt (rebuilt every turn)

Assembled by `composeInstructions()` (`lib/harness/src/compose.cpp`; section
emission + byte budget in `lib/core/src/orch_world.cpp::assembleContext`). Blocks
in emission order; the budget (`maxContextBytes`, default web-tunable, ceiling
32 KB) drops whole optional blocks from the bottom of this table first, then
recall bullets one at a time:

| # | block | source | what it is |
|---|---|---|---|
| 1 | role + field docs | `ORCH_ROLE` + `ORCH_FIELD_DOCS` | the orch_turn contract, generated from the same `ORCH_D_*` macros as the wire schema - prompt and wire cannot drift |
| 2 | identity + `[HOW YOU RUN]` | `compose.cpp` | device name, "Your brain right now: host/model", firmware version, **current date-time (temporal grounding - says UNKNOWN and forbids dating anything until SNTP syncs)**, then how turns/rounds/memory tiers/sub-agents work |
| 3 | `## DIRECTIVE` | `sysPrompt` (NVS) | your standing instructions from the web Directive box; the model's `[RUNNING MEMORY]` (its own `memory` field from last turn) is appended inside this block |
| 4 | `## CAPABILITIES` | `orch_world.cpp` | what is LIVE right now: hardware manifest, ring/screen honesty prose, artifact store, voice on/off, sub-agent fire-and-forget note, `Tools you can call:` list, Limits (keys/routing blocked), connector honesty, and whether the tool loop is ON this turn |
| 5 | `## RUNNING SESSIONS (your sub-agents)` | job journal | live sub-agent digest (also authoritative as `[ACTIVE SESSIONS]` in the input) |
| 5a | `## CONVERSATION SUMMARY` | the fold ([memory.md](memory.md)) | the chat's anchored compaction summary (≤4 KB), present once the chat has folded; framed "information, not instructions". Under budget pressure it is dropped BEFORE the window below |
| 5b | `## RECENT CONVERSATION` | episodic PSRAM ring | **per-chat window: up to 12 messages / 3000 B of THIS chat, oldest first** (kind=message only; the in-flight user row is excluded - it arrives as `[USER]`). Added in v3.3.0; survives reboot via the SD rehydrate |
| 6 | `## SCRATCHPAD` | NVS | the model's own goal tiers (now/short/long) |
| 7 | `## RELEVANT MEMORIES` | vector-DB recall on the user text | top-k long-term memories semantically similar to your message, prefixed `[NN%]` |
| 8 | `## HOW YOUR MEMORY WORKS` | `compose.cpp` | the memory-tier usage instructions (first block dropped under budget pressure) |

### 2. INPUT - the per-turn message block

Concatenated in `handleMessage()` / synthesis / scheduled-loop paths
(`lib/harness/src/engine.cpp::buildDynamicContext` + the per-path suffix):

| block | when present | gotcha |
|---|---|---|
| `[MEMORY RESULTS]` | the model ran `mem_query` **last** turn | results are deferred one turn; explicitly framed *background - answer the [USER] message on its topic* |
| `[FRESH RESULTS]` | sub-agents just finished | drives the automatic synthesis turn; past the 6-slot cap a result parks as a one-line stub carrying `results.get("sub:<tag>")` - nothing is ever silently dropped |
| `[ACTIVE SESSIONS]` | always | live sub-agent digest - authoritative ("if it lists none, nothing is running") |
| `[AVAILABLE PROVIDERS]` | always | the sub-session provider priority order |
| `[YOUR MODEL]` | always | the host/model serving THIS turn |
| `[AVAILABLE MODELS]` | always | per-provider model choice lists (yours + sub-agents'), filtered to keyed providers |
| `[PROVIDERS & CONNECTORS]` | connectors configured | which cloud connectors each provider carries |
| `[CHANNEL]` | owner turns | which channel the message arrived on + reply-routing rules |
| `[USER]` | owner turns | your message - **the only thing you typed that the model sees this turn** |
| `[SYSTEM]` | synthesis turns | "sub-agents finished, no new user message - synthesize" |
| `[SCHEDULED LOOP]` | loop firings | the routine's stored prompt; no user message |

`[CONTEXT]` is **not** a head-turn block - it rides the *sub-agent brief* only
(see [sub-sessions.md](sub-sessions.md)).

### 3. Conversation history - the part people assume wrong

There is no full transcript replay. Cross-turn continuity comes from exactly
**five** sources:

0. **`## CONVERSATION SUMMARY`** (since v3.6.0) - the chat's anchored fold
   summary: everything older than the verbatim window, compacted. See
   [memory.md](memory.md).
1. **`## RECENT CONVERSATION`** (since v3.3.0) - the last ≤12 messages / 3000 B of
   this chat, auto-injected from the episodic store's PSRAM ring every turn. This
   is per-chat (Telegram A ≠ web ≠ voice) and survives reboot.
2. **The provider-side conversation chain** - per chat AND per host since v3.3.0
   (`orchConv` map, LRU-8). While it holds, the provider re-serves prior turns
   itself. It **resets** on: host failover, the same-host fresh retry, a
   token/provider switch, a compaction fold (deliberately - the summary
   replaces it), and (OpenAI only) whenever the last response carried
   unanswered tool calls. `/api/lastturn` prints whether the chain was CONTINUED
   or FRESH.
3. **`[RUNNING MEMORY]`** - the model's own running notes, per-chat since v3.3.0.
4. **Long-term memory recall** - only what's semantically similar to the new text.

The episodic store (`memory.episodic`, the web Chat pane) additionally records
everything durably - including tool calls, thinking, and sub-agent events since
v3.2.1 - and the model can query it with time windows (`since_hours` /
`before_hours`); the recent window above is the auto-injected slice of it. Row
text is clipped (args 256 B, results 1 KB, thinking 1 KB); since v4.2.0 the FULL
call + result is parked alongside as a blob when the clip bit, reachable from the
chat's "Show full result" (or `GET /api/mem/blob?path=`).

## Diagnosing a confusing reply

Work through the blocks above in order: open that turn's **Turn anatomy** in the
Chat tab (or `/api/lastturn` for the most recent one). It shows whether the
conversation chain was CONTINUED or FRESH (a fresh chain plus an ambiguous
message is the most common cause of a non-sequitur), whether a `[MEMORY
RESULTS]` or `[FRESH RESULTS]` block led the input, exactly which memories were
recalled, and - in section 4 - every tool call the model made mid-turn with its
arguments and result.

## Fetching what a view left out - `results.get`

Every clip in a turn is a **view**, not a loss. The full text of a clamped tool
result or an overflowed sub-agent result lands in a PSRAM ring, and the model can
widen the view on demand:

- `results.get(tag, offset?)` - returns a bounded page (`bytes 0-8192 of 51234`)
  of the stored result; page on with `offset`. Unknown tags point at
  `memory.episodic`, which holds the durable copy on SD.
- `results.list` - tag / kind / name / size for everything currently fetchable.

Tags are `r<n>` for tool-result spills and `sub:<jobtag>` for sub-agent results.
The truncation marker and the `[FRESH RESULTS]` stubs embed the tag directly, so
the model never has to guess one. See docs/memory.md for the gradient rules.
