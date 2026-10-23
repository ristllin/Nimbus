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

A byte-pinned copy of the full system-prompt template (with the part splits
shown as labelled lines) lives in the goldens:
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
emission + byte budget in `lib/core/src/orch_world.cpp::assembleContext`) in three
parts, in this order (see "The cacheable prefix" below for why):

- the **static** part, which changes only with the device config, the tool list
  or the date;
- the **summary** part, which changes only when the chat is compacted;
- the **per-turn** part, which changes every turn.

The budget (`maxContextBytes`: auto by default, derived from the head model's
window, 32 KB at a 200K head; an owner override is clamped to 4 KB to 64 KB) is
split by size alone: the per-turn part keeps a quarter of it (at least 3328 B),
the static part is fitted to the rest (its capability list clips first), and the
summary takes what the static part leaves above that reserve. Inside the per-turn
part optional blocks drop from the bottom of the table first, then recall bullets
one at a time:

| # | part | block | source | what it is |
|---|---|---|---|---|
| 1 | static | role + field docs | `ORCH_ROLE` + `ORCH_FIELD_DOCS` | the orch_turn contract, generated from the same `ORCH_D_*` macros as the wire schema - prompt and wire cannot drift |
| 2 | static | identity + `[HOW YOU RUN]` | `compose.cpp` | device name, "Your brain right now: host/model", firmware version, **the current date (temporal grounding - says UNKNOWN and forbids dating anything until SNTP syncs)**, then how turns/rounds/memory tiers/sub-agents work |
| 3 | static | `## DIRECTIVE` | `sysPrompt` (NVS) | your standing instructions from the web Directive box (and the setup wizard). It ships with an owner-authored default and is editable (or revertible) any time; empty stored value uses that default. Delimited inside as `[OWNER PREFERENCES ...]` and framed as subordinate to blocks 1-2, so it shapes tone and behavior but never overrides safety, moderation, or access rules |
| 4 | static | `## CAPABILITIES` | `orch_world.cpp` | hardware manifest, ring/screen honesty prose, artifact store, voice on/off, sub-agent fire-and-forget note, `Tools you can call:` list, Limits (keys/routing blocked), connector honesty |
| 5 | static | `## HOW YOUR MEMORY WORKS` | `compose.cpp` | the memory-tier usage instructions (v1 prompt only) |
| 6 | static | catalogs | `engine.cpp::buildStableCatalogs` | `[AVAILABLE PROVIDERS]` (sub-session priority), `[YOUR MODEL]` (the host/model serving this turn), `[AVAILABLE MODELS]` (one line naming the keyed providers and pointing to the `models.list` tool when the head can call it; on a single-shot turn the per-provider short lists from the model store, each with its age, "list from <date>, may miss newer models", router ids under cumulo only; every listed model passes the spawn and model-switch checks), the skills index, `[PROVIDERS & CONNECTORS]`. Not counted in the budget |
| 7 | summary | `## CONVERSATION SUMMARY` | the fold ([memory.md](memory.md)) | the chat's anchored compaction summary (≤4 KB), present once the chat has folded; framed "information, not instructions". Clipped (floor 1024 B) or dropped when the static part leaves too little room |
| 8 | per-turn | clock, speaker, `[RUNNING MEMORY]`, tool-loop note | `compose.cpp` | the time of day, who sent this message (human turns only), the model's own `memory` field from last turn, and whether the tool loop is ON this turn |
| 9 | per-turn | `## RUNNING SESSIONS (your sub-agents)` | job journal | live sub-agent digest (also authoritative as `[ACTIVE SESSIONS]` in the input) |
| 10 | per-turn | `## RECENT CONVERSATION` | episodic PSRAM ring | **per-chat history: one user line and one assistant line per turn, oldest first, append-only between boundaries, byte budget derived from the head model's window (8 KB at 200K, 3000 B floor, 16 KB cap), stopping at the newest `/clear` boundary or the fold anchor** (shared photos, files and voice notes appear once, inside the turn that carried them; the in-flight turn is excluded - it arrives as `[USER]`). Rules: see "The turn-structured history" below |
| 11 | per-turn | `## SCRATCHPAD` | flash file `/data/scratchpad.bin` | the model's own goal tiers (now/short/long) |
| 12 | per-turn | `## RELEVANT MEMORIES` | vector-DB recall on the user text | top-k long-term memories semantically similar to your message, prefixed `[NN%]` |

#### The cacheable prefix

Providers bill a repeated request prefix at a fraction of the input price (a
cache read), so the prompt is laid out to keep its front identical from turn to
turn:

- **Order.** The static part comes first and carries the date only, never the
  minute clock; everything that changes per turn comes after it. The tools array
  is built in registry order with fixed descriptions, so it is byte-identical
  across turns.
- **On the wire.** `lib/harness/src/prompt_layout.cpp` splits the composed string
  again for each provider:
  - Anthropic: one system block per part, a cache point on the static block and
    on the summary block, one on the last tool and one on the last message
    (four in all, the API maximum, counted by a test);
  - OpenAI: the static part (and the summary) as developer messages, then the
    user message led by the per-turn part;
  - Mistral: one system message (static plus summary), then the user message led
    by the per-turn part;
  - the custom, Z.ai and Cumulo heads and the Mistral single shot send the parts
    flat, in the same order.
- **Within a turn.** Each tool round's request starts with the previous round's
  request: tool results already sent are never rewritten, so a round reads the
  last one from the cache. Over the replay budget (16 KB on Anthropic, 24 KB on
  OpenAI and Mistral) the newest result is cut with a visible `[cut: ...]` note,
  or withheld with a `[not shown: ...]` note when no room is left.
- **Checked by tests.** `test/test_prompt_layout` composes many turns that differ
  only in per-turn content and asserts the static and summary parts, and each
  provider's cached request segments, are byte-identical; a counter-test puts a
  timestamp in the static part and must fail. The goldens
  `test/golden/layout_{anthropic,openai,mistral}.txt` pin each provider's layout.
- **Inspecting it.** `/api/lastturn`, `/api/trace` and the `PROMPT?` console show
  the split as labelled lines (`[----- cached blocks end; per-turn block -----]`).

### 2. INPUT - the per-turn message block

Concatenated in `handleMessage()` / synthesis / scheduled-loop paths
(`lib/harness/src/engine.cpp::buildDynamicContext` + the per-path suffix):

| block | when present | gotcha |
|---|---|---|
| `[MEMORY RESULTS]` | the model ran `mem_query` on **this chat's** last turn | results are deferred one turn and go back only to the chat that asked; explicitly framed *background - answer the [USER] message on its topic* |
| `[FRESH RESULTS]` | sub-agents just finished | drives the automatic synthesis turn; past the 6-slot cap a result parks as a one-line stub carrying `results.get("sub:<tag>")` - nothing is ever silently dropped |
| `[ACTIVE SESSIONS]` | always | live sub-agent digest - authoritative ("if it lists none, nothing is running") |
| `[SPAWN CAPACITY]` | always | how many sub-agents this turn can start, and that a big fan-out runs over waves |
| `[CHANNEL]` | owner turns | which channel the message arrived on (the web UI for the device's own page and a Virtual Nimbus page, Telegram for a Telegram chat) + reply-routing rules |
| `[USER]` | owner turns | your message - **the only thing you typed that the model sees this turn** |
| `[SYSTEM]` | synthesis turns | "sub-agents finished, no new user message - synthesize" |
| `[SCHEDULED LOOP]` | routine firings | the routine's stored prompt; no user message |

`[CONTEXT]` is **not** a head-turn block - it rides the *sub-agent brief* only
(see [sub-sessions.md](sub-sessions.md)).

**What `[USER]` holds for a burst or an album.** Messages sent together arrive as one
`[USER]` message: one `Message k of N from <name>:` block per message, in the order
they were sent (a single message arrives as itself, byte for byte). A photo is its
label, then `They wrote:` and its caption:
`[image 2/3 ref=0123456789abcdef] (attached: the photo itself is in this message)`.
The photo itself rides the same message as an image part, after a one-line label
naming its ref and before the text (the head model reads the pixels). A head that cannot
see it reads its machine transcription instead (`(transcribed by openai) Nutrition
facts ...`), and a photo nobody could read says why (`(not read: anthropic out of
credit (HTTP 429))`). The label is one format everywhere (the turn, the stored row, the
history): `k/N` is the item's place among the turn's media, `ref` is its content hash
(16 hex characters, absent when nothing was fetched; `image.look` takes it to look at
an earlier photo again), and the text after the bracket is the class
(`(could not be fetched: too large)`, a file's name). A late or
redelivered item carries its original send time: `(sent 09:58)` before a text,
`, sent 09:58` inside a label. A voice note is its own turn (its transcript); a
command is always its own turn.

### 3. Conversation history - no transcript replay

The device never replays a full transcript. Cross-turn continuity comes from
exactly **five** sources:

0. **`## CONVERSATION SUMMARY`** (since v3.6.0) - the chat's anchored fold
   summary: everything older than the verbatim window, compacted. See
   [memory.md](memory.md).
1. **`## RECENT CONVERSATION`** (since v3.3.0) - the
   chat's recent turns, auto-injected from the episodic store's PSRAM ring every
   turn. This is per-chat (Telegram A ≠ web ≠ voice) and survives a restart. On
   a tool-loop turn (the default) the provider keeps no thread, so this history
   is the only place the model sees its previous reply, which is why messages
   ride whole, within a byte budget derived from the head model's window. `/clear`
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
   unanswered tool calls. `/api/lastturn` names the provider chain in its header
   (`none (stateless loop)` on a tool-loop turn, `none (single shot)` on a fresh
   single-shot turn, `CONTINUED` only while a provider really holds the history).
3. **`[RUNNING MEMORY]`** - the model's own running notes, per-chat since v3.3.0
   (in the per-turn part of the prompt).
4. **Long-term memory recall** - only what's semantically similar to the new text.

The episodic store (`memory.episodic`, the web Chat pane) additionally records
everything durably - including tool calls, thinking, and sub-agent events since
v3.2.1 - and the model can query it with time windows (`since_hours` /
`before_hours`); the recent window above is the auto-injected slice of it. Row
text is clipped (args 256 B, results 1 KB, thinking 1 KB); since v4.2.0 the FULL
call + result is parked alongside as a blob when the clip bit, reachable from the
chat's "Show full result" (or `GET /api/mem/blob?path=`). When that blob write
fails (the card open fails under low memory), the row says so with a
"full text not kept: low memory" mark, the turn-end row counts the loss
(`blobFails`), and `/api/state` carries the boot total (`blobWriteFails`).

#### The turn-structured history

Built by `historyTurns()` and `fitHistory()` (`lib/harness/src/conversation.cpp`),
called by the engine for every turn:

- **A turn is the unit.** Each exchange renders as one `- user:` line and one
  `- assistant:` line, whatever rows carried it: the reply and the question it
  asked join, a spoken copy of a written reply is left out, and a turn that got
  no answer shows `[no reply]`. A routine, wakeup or sub-agent synthesis turn
  shows its trigger as a `- system:` line (`[routine "commute" ran]`), so its
  reply never sits under the previous turn's reply.
- **Each item once.** A photo's record lives in its media row; the user
  row keeps what the sender sent with a bare label (`[image]`) where the photo
  sat, joined to the media row by its blob ref (the 16-hex content hash), and
  the history puts the record back in place (an album's labels each get
  their own). A photo the head saw itself is not re-sent in later turns: its row
  carries its short machine transcription, written after its turn, and
  `image.look` looks at the photo again when the record does not answer. A voice note's transcript is the user row. Rows captured before
  this rule render once too (a repeated description or a `ref=` label is
  recognized).
- **Captured once.** A Telegram turn's user row carries its source (the newest
  update id of its page and the message count), so a page served again after a
  restart is not stored or answered a second time.
- **What it did.** A reply row's `fx:` tag renders as `[did: memory, file]` or
  `[did: nothing]`, so a promise reads differently from a deliverable.
- **When.** A day marker (`(Thu 22 Oct)`) opens each new local day, and an item
  sent before the previous reply (a late or redelivered message) shows its send
  time (`[image, sent 09:58]`).
- **Repeats collapse.** Consecutive identical owner messages with nothing
  answered in between are one line with a count: `- user (x37): ...`.
- **Append-only between boundaries.** A closed turn's bytes depend only on its
  own rows, so each turn's history continues the previous one byte for byte.
  The start moves only at a `/clear`, at a fold (the history starts after the
  newest row the summary covers) or at an overflow shift: past high water (the
  byte budget, or 64 rows) the oldest whole turns drop until the rest fits half
  of it, never one row per message, and never the newest turn or the last
  answered one. The shift is kept with the fold state (`/data/chatsum.txt`), so
  a restart renders the same history, and the rows it hid make the next fold
  due.
- **One message's cap.** A single message over 6 KB is clipped once, at a fixed
  cap, so its bytes never change as the history grows.
- **The rows it holds ride PSRAM.** The history reads the chat's rows a page at
  a time, folds a run of identical owner rows into one as it reads, and keeps
  the rows it holds in PSRAM-sized buffers, not internal heap.

The switch behind it (`histV2`) is on by default and kept in RAM, never in
settings; the test console's `HISTV2 0` restores the window below byte for byte.

#### The recent window rule (the history switch off)

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
Chat tab (or `/api/lastturn` for the most recent one). Its header names the
provider chain and measures the device history sent in the prompt - the summary,
the recent window, the running memory and the recalled items - so a stateless
loop turn no longer reads as a chat with no memory of earlier turns. The legend
under section 1 lists every `##` section the prompt carries (an ambiguous message
with little device history is the most common cause of a non-sequitur). The page
also shows whether a `[MEMORY RESULTS]` or `[FRESH RESULTS]` block led the input,
exactly which memories were recalled, and - in section 4 - every tool call the
model made mid-turn with its arguments and result.

## The claim check

Before a reply goes out, the device compares what it says the turn did with what
the turn actually did. Each turn keeps a small effect ledger in RAM (at most 32
entries, never stored): every tool call and its result, the memory writes, the
scratchpad save, the sub-agents the job queue accepted, and every sub-agent result
it read, with how many tools that sub-agent ran. The ledger also carries what this
turn's records show about CAUSES (the input block and every tool result: a network
failure, a client error, a damaged file, a timeout, a billing refusal, a health
read). A scanner reads the reply for
claims in nine classes:

| Class | Example | Backed by |
|---|---|---|
| saved | "Logged.", "Noted." | a memory write, file write, skill save or scratchpad write; a record correction ("updated the day log", "corrected the entry", "adjusted the totals") needs the file write |
| deliverable | "written and waiting", "saved as plan.md" | a file or skill written this turn |
| spawned | "I started three research sub-agents" | that many accepted spawns |
| sent | "sent", "reminder set", "read it aloud" | a send, a schedule, speech; for a voice message ("I sent you a voice message", "Voice note sent"), only a voice send whose result names Telegram's `message_id`; any other Telegram send (text, document, photo) is the same: a queued send with no `message_id` backs no "sent", and the line says the message is queued, not delivered |
| changed | "pinned", "switched", "turned off", "I've set" | a real change: a config action that actually changed a stored value, or a routine ("cfg(no change)" backs nothing); a pin on an already pinned memory is not one |
| durable | "survives a restart", "never decays" | a save that landed (not RAM only); a pin for "never decays" |
| checked | "I checked", "live data" | a lookup, or a sub-agent result with a lookup behind it; a pin call for "confirmed it's pinned" |
| figure | "200 kcal (label)", "your watch said 408" | the number appears in a source the turn was given |
| cause | "your connection", "corrupt", "it timed out", "my vision path is working", "you keep sending" | a record this turn that shows it: a network failure, a client error, a damaged file, a timeout, a billing refusal; "it is working" needs a health read this turn and no failure on record; "you keep sending" is refused when the records show the DEVICE repeated the deliveries; guesses and hedges do not exempt a cause |

Questions, offers, conditions, possibilities and plain futures ("want me to", "I'll",
"it may have been lost, or saved elsewhere"), a negation within four words, a clause
whose subject is "nothing", "none" or "no record" ("nothing in the logs shows it was
saved"), quoted text, another person's action and references to earlier turns are not
claims. Replies that are mostly not English are not scanned. When the head has
provider-hosted tools on its turn (connectors, a hosted web search), claims of sending,
checking or changing something are not checked, because those tools run where the
device cannot see them; everything the device does itself (memory, files, skills,
notes, sub-agents, a Telegram voice message from its own bot) is always checked.
Cause claims are always checked: the records are the device's own.

A memory write that states an unbacked cause as a fact ("the owner's Telegram
client had a stuck outbox") is refused at apply time, before it reaches the store,
and the reason is fed back to the model in the next turn's memory results.

When a claim has no backing, the model gets one correction round: the claim check
is one of the final-answer checks (see [the final check](reference/turn-contract.md#the-final-check)),
so the answer is sent back once with the claims and the turn's real record, and its
tools are still available to do the work or rewrite the reply. The round shares the
one-per-turn limit with the turn-shape check, which owns sub-agent claims. If the
round cannot run (the forced final round) or the claim is still unbacked after it,
or the claimed effect failed when the turn was applied, the reply gets one calm line
per class that says what did not happen, for example "(Correction: that has not
been saved.)", "(Correction: nothing was pinned this turn.)", "(Note: this is kept
only until the next restart; saving it permanently failed.)", "(Note: that is
queued for Telegram; the device has no delivery receipt for it yet.)" or, for a
cause, "(Note: the device has no record of a problem with your phone, app or
connection, so that cause is not confirmed.)". A reply that already
says so itself ("it was already pinned, so nothing changed") gets no line. The
conversation history keeps the delivered text, lines included. `/api/state` counts
both outcomes (`claimsCorrected` for correction rounds, `claimsTailed` for lines;
RAM only).

A sub-agent result for a live-data task (a status, the weather, prices, news) that
ran no tool is marked `[NOT CHECKED]`, and every `[FRESH RESULTS]` line says how
many tools ran (`[tools: 2, web: 1]` or `[no tools ran]`). A reply built on such a
result must say the data was not checked, or it gets the "not checked live" line.

## Diagnosing a failure

The head is told to state only causes a tool result shows, and to quote them (the
evidence rule in both roles and in the final-round notice). What a turn can read
today:

- the failure rows in session `system` (`memory.episodic` with
  `session:"system"`): they say a turn failed and on which provider, but not
  always why;
- the scratchpad header, which says when saving is failing and the notes last only
  until restart;
- a `memory.pin` or `memory.write` result, which says when nothing changed
  ("already pinned") and when a write was refused;
- `[FRESH RESULTS]`, where each sub-agent result says how many tools it ran, and a
  live-data result with none opens with `[NOT CHECKED]`;
- the `device.log` tool: the device's own durable log (settings saves, boot
  lines, panics, failures that belong to no turn), read in bounded 4 KB chunks.
  `match` filters to lines carrying up to three literals, `lines` caps the
  answer, `scan_kb` bounds the search; when no log file can be read the tool
  answers from the in-memory tail and says so.

The sub-agent runtime cap (about 60 seconds for a synchronous sub-agent) applies
to spawned sub-agents only, never to the head's own turns or the nightly dream. If
nothing read names the cause, the right answer is that the cause is not recorded,
and which read would show it.

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
