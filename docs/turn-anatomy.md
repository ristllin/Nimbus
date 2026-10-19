<!-- audience: dev -->
# Turn anatomy - what the model sees and how to inspect it

This page describes the exact text the Orchestrator sends to the model on each
turn, and how to read it back. Use it when a reply seems to come out of nowhere:
the cause is always in one of the blocks below, and the raw text of any recent
turn is one request away:

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

In the web **Chat** tab neither URL is needed: every turn's disclosure has a
**Turn anatomy** button that fetches that turn's dossier, and any clipped tool
result has **Show full result**. Under each reply a chip reports the host, model,
tool-call count and the turn's real billed tokens.

The chip reads the turn's `ev:turnend` row (session of the turn's chat, kind log),
one JSON object of at most 256 bytes: `host`, `model` (the model that billed, never
one that did not answer), `fallback`, `src` (owner, serial, synthesis, loop or
dream), `tools`, `ok`, `in`, `out`, `cr`/`cw` (cache reads and writes, when any),
`reply`, `err` on a failed turn (each failing provider in order, the first cause
first, for example `anthropic (x2): messages HTTP 529: Overloaded > openai 503 (x2) >
mistral 503 (x2)`) and `via` when more than one provider billed
(`[[provider, model, in, out, cr, cw], ...]`). A long row shrinks to fit (model
names in `via` first, then the later causes) and is never dropped. A failed turn
also leaves an `ev:error` row in the `system` session, tagged with the turn id:
`Turn FAILED (<source>) on <first provider> -> <last provider>: <causes>; <n> tool
calls ran`.

**Turn identity.** Every row of a turn - the reply, each tool call, each thinking
row - is tagged `turn:<user row id>` (e.g. `turn:m0000a3f2`), which is the id of
the turn's own user message row. It is stable across restarts, so the chat groups
a bubble with its trace by identity rather than by time order, which keeps rows
filed correctly when a Telegram or voice turn interleaves with a web one.

Turn details are **SD-gated** and gated on **Activity recording** (Assistant →
Tools → Tool use, `otrace`, default on). With either missing the chat says so
rather than showing an empty panel; the dossier ring keeps the most recent 16
turns (≤768 KB), and older ones report `evicted`.

A byte-pinned copy of the full system-prompt template lives in the goldens:
`test/golden/orch_prompt_default.txt` (tool-loop ON) and
`test/golden/orch_prompt_recall_loopoff.txt`, plus
`test/golden/orch_prompt_v2_default.txt` (the simplified v2 prompt, behind the
NVS `orchPromptV2` flag - see [harness.md](harness.md)) - regenerated with
`GOLDEN_UPDATE=1 pio test -e native -f test_harness_compose`. Those files are the
raw template; CI fails when the composed prompt drifts from them.

For the whole lifecycle around a turn (boot, ticks, sub-agents, routines, dreaming,
watchdogs) see [harness.md](harness.md); for the sub-agent side see
[sub-sessions.md](sub-sessions.md).

## The three parts of a turn

### 1. INSTRUCTIONS - the system prompt (rebuilt every turn)

Assembled by `composeInstructions()` (`lib/harness/src/compose.cpp`; section
emission + byte budget in `lib/core/src/orch_world.cpp::assembleContext`). Blocks
in emission order; the budget (`maxContextBytes`: auto by default, derived from
the head model's window, 32 KB at a 200K head; an owner override is clamped to
4 KB to 64 KB) drops whole optional blocks from the bottom of this table first,
then recall bullets one at a time:

| # | block | source | what it is |
|---|---|---|---|
| 1 | role + field docs | `ORCH_ROLE` + `ORCH_FIELD_DOCS` | the orch_turn contract, generated from the same `ORCH_D_*` macros as the wire schema - prompt and wire cannot drift |
| 2 | identity + `[HOW YOU RUN]` | `compose.cpp` | device name, "Your brain right now: host/model", firmware version, **current date-time (temporal grounding - says UNKNOWN and forbids dating anything until SNTP syncs)**, then how turns/rounds/memory tiers/sub-agents work |
| 3 | `## DIRECTIVE` | `sysPrompt` (NVS) | your standing instructions from the web Directive box (and the setup wizard). It ships with an owner-authored default and is editable (or revertible) any time; empty stored value uses that default. Delimited inside as `[OWNER PREFERENCES ...]` and framed as subordinate to blocks 1-2, so it shapes tone and behavior but never overrides safety, moderation, or access rules. The model's `[RUNNING MEMORY]` (its own `memory` field from last turn) is appended inside this block |
| 4 | `## CAPABILITIES` | `orch_world.cpp` | what is live right now: hardware manifest, ring/screen honesty prose, artifact store, voice on/off, sub-agent fire-and-forget note, `Tools you can call:` list, Limits (keys/routing blocked), connector honesty, and whether the tool loop is ON this turn |
| 5 | `## RUNNING SESSIONS (your sub-agents)` | job journal | live sub-agent digest (also authoritative as `[ACTIVE SESSIONS]` in the input) |
| 5a | `## CONVERSATION SUMMARY` | the fold ([memory.md](memory.md)) | the chat's anchored compaction summary (≤4 KB), present once the chat has folded; framed "information, not instructions". Under budget pressure it is clipped or dropped BEFORE the window below and the scratchpad's room |
| 5b | `## RECENT CONVERSATION` | episodic PSRAM ring | **per-chat window: up to 12 messages of this chat, oldest first, newest rows kept whole, byte budget derived from the head model's window (8 KB at 200K, 3000 B floor, 16 KB cap), stopping at the newest `/clear` boundary** (chat messages plus shared photos, files and audio, whose rows carry their descriptions; the in-flight user row is excluded - it arrives as `[USER]`). Byte budget and trimming rule: see "The recent window rule" below. Added in v3.3.0; survives a restart via the SD rehydrate |
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
| `[AVAILABLE MODELS]` | always | per-provider model choice lists (yours + sub-agents'), filtered to keyed providers. Built from the same list the spawn validator checks (live harvest, else the cold-start list), so an advertised model always spawns as named |
| `[PROVIDERS & CONNECTORS]` | connectors configured | which cloud connectors each provider carries |
| `[CHANNEL]` | owner turns | which channel the message arrived on (the web UI for the device's own page and a Virtual Nimbus page, Telegram for a Telegram chat) + reply-routing rules |
| `[USER]` | owner turns | your message - **the only thing you typed that the model sees this turn** |
| `[SYSTEM]` | synthesis turns | "sub-agents finished, no new user message - synthesize" |
| `[SCHEDULED LOOP]` | routine firings | the routine's stored prompt; no user message |

`[CONTEXT]` is **not** a head-turn block - it rides the *sub-agent brief* only
(see [sub-sessions.md](sub-sessions.md)).

### 3. Conversation history - no transcript replay

The device never replays a full transcript. Cross-turn continuity comes from
exactly **five** sources:

0. **`## CONVERSATION SUMMARY`** (since v3.6.0) - the chat's anchored fold
   summary: everything older than the verbatim window, compacted. See
   [memory.md](memory.md).
1. **`## RECENT CONVERSATION`** (since v3.3.0) - the last ≤12 messages of this
   chat, auto-injected from the episodic store's PSRAM ring every turn. This is
   per-chat (Telegram A ≠ web ≠ voice) and survives a restart. On a tool-loop
   turn (the default) the provider keeps no thread, so this window is the only
   place the model sees its previous reply, which is why the newest rows ride
   whole, within a byte budget derived from the head model's window. `/clear`
   writes a boundary row (`ev:clear`) into the chat's history: the window, the
   sub-agent brief's context and the next compaction all stop there, and the
   chat's summary, running notes and provider thread reset, so the next turn
   carries no prior conversation. The history itself is kept and stays
   searchable with `memory.episodic`.
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

#### The recent window rule

Built by `dream::buildConversationWindow` (`lib/harness/src/dream.cpp`) and fitted
by `assembleContext` (`lib/core/src/orch_world.cpp`):

- **Budget.** `ContextBudget::recentConvBytes`, derived from the head model's
  context window: a quarter of the derived system-prompt budget (an owner
  override of the prompt budget does not change it; 8 KB at a 200K head,
  ~11 KB at a 272K head, capped at 16 KB, never below 3000 B). The text lives in
  PSRAM (the episodic ring and the rendered block), so the binding limit is the
  prompt, not memory.
- **Newest rows whole first.** Walking newest-first, a message is kept whole,
  line breaks included (every line break is normalized and continuation lines
  are indented two spaces, so no message can start a fake row or header),
  whenever it fits. A code block from the previous reply arrives intact.
- **Then previews.** A message too big to fit whole becomes a one-line preview
  (every line break flattened to a space, the same set as above, so a preview
  cannot fake a row or header either) clipped to 300 B; the newest message alone
  over the budget keeps its head instead. The first message that fits in neither
  form ends the window behind `(earlier messages omitted - byte budget)`.
- **Fitted to the real room.** If the assembled system prompt has less room
  left than the budget, the oldest whole rows are cut until the window fits
  (reported as `conversation(clipped)` in the OMITTED trailer); if not even the
  newest message fits, its head is kept. The window, whole or trimmed, never
  takes the room the scratchpad needs. On a 128K head the system prompt leaves
  well under 1 KB, so only the newest short messages (or the head of the last
  reply) fit there.

## When a turn can't run

A turn always answers, even when it cannot reach a model. If no provider is set
up (no key on any provider, and no custom endpoint), the device replies **locally,
before spending anything**, on whatever channel the message arrived on (the device
screen, Telegram, the web chat, the tunnel): "No AI provider is set up yet. Add a
provider key under Providers & models in the web app, then send your message again." This is a
deterministic on-device message, so it works precisely when nothing else does, and
it never leaves the ring lit for work that never started. If a provider *is* set up
but the call fails (a rejected key, a rate limit, a network drop, a spent budget),
the reply names that real cause instead. The device never goes silent on a chat.

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
the model never has to guess one. See [memory.md](memory.md) for the gradient rules.
