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
emission + byte budget in `lib/core/src/orch_world.cpp::assembleContext`) in five
parts, in this order (see "The cacheable prefix" below for why):

- the **static** part, which changes only with the device config, the date, a key
  verdict, or a provider pausing or returning;
- the **shelf** part (the owner's standing preferences and the skills index),
  which changes only on an owner or skill edit;
- the **history** part: the conversation as real user and assistant messages,
  the fold summary's pair first - the only thing that rewrites it is a
  compaction, a `/clear` or an overflow shift;
- the **per-turn** part, which changes every turn;
- the **cache settings** (the Anthropic TTL choice and the per-chat cache key),
  which the providers read and never send as text.

The budget (`maxContextBytes`: auto by default, derived from the head model's
window, 32 KB at a 200K head; an owner override is clamped to 4 KB to 64 KB) is
split by size alone: the per-turn part keeps a quarter of it (at least 3328 B),
and the static part is fitted to the rest, its shelf and catalogs counted against
it (its capability list clips first). The history rides its own budget (derived
from the head model's window), so a growing conversation never moves the static
part's clip point. Inside the per-turn part optional blocks drop from the bottom
of the table first, then recall bullets one at a time:

| # | part | block | source | what it is |
|---|---|---|---|---|
| 1 | static | role + field docs | `ORCH_ROLE` + `ORCH_FIELD_DOCS` | the orch_turn contract, generated from the same `ORCH_D_*` macros as the wire schema - prompt and wire cannot drift |
| 2 | static | identity + `[HOW YOU RUN]` | `compose.cpp` | device name (never the model: the static block is cached and outlives a failover, so the model is named per request in `[THIS TURN RUNS ON]`), firmware version, **the current date (temporal grounding - UNKNOWN, and no dating anything, until SNTP syncs)**, then how turns/rounds/memory tiers/sub-agents work |
| 3 | shelf | `[OWNER PREFERENCES ...]` | `sysPrompt` (NVS) | your standing instructions from the web Directive box (and the setup wizard), with an owner-authored default when nothing is stored. Subordinate to blocks 1-2: it shapes tone and behavior but never overrides safety, moderation, or access rules. It rides the shelf, so an owner edit re-caches only the shelf. Below it, `[STANDING RULES]`: your always/never rules from `/data/prefs.txt` (up to 12, added with `/prefs` or `prefs.edit`), then `[OPEN QUESTIONS]`: open follow-ups from `/data/openq.txt` (up to 5), each raised at most once |
| 4 | static | `## CAPABILITIES` | `orch_world.cpp` | hardware manifest, ring/screen honesty prose, artifact store, voice on/off, sub-agent fire-and-forget note, Limits (keys/routing blocked), connector honesty. The `Tools you can call:` list appears only when the mid-turn tool loop is disabled in the settings - with the loop on, every tool rides the request itself with its full description and the manifest points there (about 8 KB less per turn) |
| 5 | static | `## HOW YOUR MEMORY WORKS` | `compose.cpp` | the memory-tier usage instructions (v1 prompt only) |
| 6 | static | catalogs | `engine.cpp::buildStableCatalogs` | `[AVAILABLE PROVIDERS]` (sub-session priority, then "(paused now: ...)" for paused providers), `[AVAILABLE MODELS]` (one line naming the keyed providers, pointing to the `models.list` tool when the tool loop is on - a config-keyed shape, never this turn's heap gate, so the cached block cannot flip per turn; heads with inline per-provider short lists show each list's age and "may miss newer models"; every listed model passes the spawn and model-switch checks; when the loop is off for a turn, the loop-off note says the pointer tool cannot run either), `[PROVIDERS & CONNECTORS]` (a head that refuses its model shows as paused for your own turns; a paused provider's row says why and when it returns and names its connectors as waiting - no spawn, PDF or sandbox route; a `PUBLIC WEB FACTS` line names the open web search routes). Counted against the static part's budget since the shelf split |
| 7 | shelf | the skills index | `engine.cpp` | the playbook list (`[SKILLS]`), the same index the skill.get tool serves from; an edit re-caches only the shelf |
| 8 | history | the conversation messages | `conversation.cpp::renderHistory` | the fold summary's message pair first, then **per-chat history: one user and one assistant message per turn, oldest first, append-only between boundaries, every turn after the fold anchor up to the head's history budget (derived from its request budget: 206 KB on Mistral Large 4, 118 KB on Claude Sonnet; on a head at v5.1.1's room, and on a Mistral or Z.ai head with the tool loop off, v5.1.1's window with the owner's local day on top), stopping at the newest `/clear` boundary or the fold anchor** (shared photos, files and voice notes appear once, in the turn that carried them; the in-flight turn arrives as `[USER]`). Providers send these as real history messages; a flat head reads them as `## RECENT CONVERSATION`. Rules: see below |
| 9 | per-turn | clock, speaker, `[RUNNING MEMORY]`, tool-loop note | `compose.cpp` | the time of day, who sent this message (human turns only), the model's own `memory` field from last turn, and whether the tool loop is ON this turn. On a "remember" message, a `[MEMORY ROUTING]` line names where it lands: an always/never rule to the standing rules via `prefs.edit` (a full list retires the oldest rule), a lasting fact to `mem_write`, a recall question to a search with nothing saved. It rides this part only, so cached blocks stay identical, and never a routine's prompt or a sub-agent's results. On a turn whose history holds an earlier unreplied owner message, a `[REPLY SCOPE]` line follows: answer this message; an earlier question or request with `[no reply]` is still owed its answer; other earlier messages are context. Absent otherwise |
| 10 | per-turn | `[RECENT ACTIONS]` | the chat's effects ring (`nimbus/orch/effect_ring.h`, through `compose.h` `RecentActionsSource`) | this chat's newest 12 effect receipts, oldest first, one line each (local time with the year, tool, receipt), at most 2,048 B (the oldest lines go whole), as data, so a correction can name what it changes without a read; only effects that happened (a failed call never joins the ring); another chat's receipts never appear; absent when the ring is empty. Since v5.2.0 no data block or playbook of one use case rides any turn. Never dropped, and never takes the rest of the per-turn part's room |
| 11 | per-turn | `## RUNNING SESSIONS (your sub-agents)` | job journal | live sub-agent digest (also authoritative as `[ACTIVE SESSIONS]` in the input) |
| 12 | per-turn | `## SCRATCHPAD` | flash file `/data/scratchpad.bin` | the model's own goal tiers (now/short/long) |
| 13 | per-turn | `## RELEVANT MEMORIES` | vector-DB recall on the user text, then the stored conversation's word search | top-k long-term memories semantically similar to your message, prefixed `[NN%]`; then up to 3 of your own earlier lines of this chat (`[said earlier in this chat, <date>]`, verbatim, at most 320 B each) that the window and the summary no longer carry |

#### The cacheable prefix

Providers bill a repeated request prefix at a fraction of the input price (a
cache read), so the prompt is laid out to keep its front identical from turn to
turn:

- **Order.** The static part comes first and carries the date only, never the
  minute clock; the shelf, then the history, then everything that changes per
  turn come after it. The tools array is built in registry order with fixed
  descriptions and rides every round of a turn unchanged, so it is
  byte-identical across turns and rounds.
- **On the wire.** `lib/harness/src/prompt_layout.cpp` splits the composed string
  again for each provider:
  - Anthropic: system blocks for the static part and the shelf, each with a
    cache point; the history as real user and assistant messages with the third
    point on its last message; the moving fourth point on the request's last
    message (four in all, the API maximum, counted by a test). The points carry
    the TTL the chat's turn gaps picked (1 hour for a chat whose median gap is
    5 to 60 minutes, else the default 5 minutes; the moving point is always
    5 minutes), and a round's thinking blocks replay unchanged before the tool
    calls they preceded;
  - OpenAI: the static part and the shelf as developer messages with explicit
    cache breakpoints, the history as message items with the third breakpoint on
    the last user item, then the user message led by the per-turn part, and the
    per-chat `prompt_cache_key`. Only gpt-5.6 and later take the breakpoints; every
    other model gets the same messages without them, and a model that still refuses
    one is sent the round again without it;
  - Mistral: one system message (static plus shelf), the history messages, then
    the user message led by the per-turn part, and the per-chat
    `prompt_cache_key`;
  - the custom, Z.ai and Cumulo heads and the Mistral single shot send the parts
    flat, in the same order (the history renders as the `## RECENT CONVERSATION`
    block).
- **Within a turn.** Each tool round's request starts with the previous round's
  request: tool results already sent are never rewritten, so a round reads the
  last one from the cache. Over the replay budget (16 KB on Anthropic, 24 KB on
  OpenAI and Mistral) the newest result is cut with a visible `[cut: ...]` note,
  or withheld with a `[not shown: ...]` note when no room is left.
- **Checked by tests.** `test/test_prompt_layout` composes many turns that differ
  only in per-turn content and asserts the static and shelf parts, and each
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
| `[FRESH RESULTS]` | sub-agents just finished | drives the automatic synthesis turn; past the 6-slot cap a result parks as a one-line stub carrying `results.get("sub:<tag>")` - nothing is ever silently dropped. Each result's sources stay under its header; a time-sensitive fact credited only to a non-primary site is labeled unverified, naming the primary source to check; no site named, no label |
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
naming its ref and before the text (the head model reads the pixels). Whether a request
shows it is decided by the host serving that request: a host that cannot see images
reads the photo's machine transcription instead (`(transcribed by openai) Receipt
total ...`, made only then), and a photo nobody could read says why (`(not read:
anthropic out of credit (HTTP 429))`). The label is one format everywhere (the turn, the stored row, the
history): `k/N` is the item's place among the turn's media, `ref` is its content hash
(16 hex characters, absent when nothing was fetched; `image.look` takes it to look at
an earlier photo again), and the text after the bracket is the class
(`(could not be fetched: too large)`, a file's name). A late or
redelivered item carries its original send time: `(sent 09:58)` before a text,
`, sent 09:58` inside a label. A voice note on its own is its transcript; in a turn with
other messages its block reads `(voice note) <transcript>` (one voice note per turn); a
command is always its own turn. A turn sent before its chat's previous answer went out
ends with one more line, "[sent while you were still answering the previous message;
that answer is already in the chat above]"; the history row does not keep it.

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
  no answer shows `[no reply]` (a later reply does not take up a remark of it unless the
  later message brings it up: the reply-scope check below). A routine, wakeup or sub-agent synthesis turn
  shows its trigger as a `- system:` line (`[routine "commute" ran]`), so its
  reply never sits under the previous turn's reply.
- **Each item once.** A photo's record lives in its media row; the user
  row keeps what the sender sent with a bare label (`[image]`) where the photo
  sat, joined to the media row by its blob ref (the 16-hex content hash), and
  the history puts the record back in place (an album's labels each get
  their own). A photo the head saw itself keeps the head's own note
  (`image.note`, written with its answer): its row reads `(seen; note: ...)`, or
  `(seen in its turn; image.look shows it again)` with no note, settled when its
  turn ends and never rewritten once sent (only a photo that a blind host alone met and
  nobody could read is read again later). `image.look` puts the photo back in
  front of the head as an image; on Anthropic a recent photo also rides its own
  history message as a file reference (the same bytes every turn). A voice note's transcript is the user row. Rows captured before
  this rule render once too (a repeated description or a `ref=` label is
  recognized).
- **Captured once.** A Telegram turn's user row carries its source (the newest
  update id of its page and the message count), so a page served again after a
  restart is not stored or answered a second time.
- **What it did.** A reply row's `fx:` tag renders as `[did: memory, file]` or
  `[did: nothing]`, so a promise reads differently from a deliverable.
- **When.** A day marker (`(Thu 22 Oct)`) opens each new local day, and an item
  sent before the previous reply (a late or redelivered message) shows its send
  time (`[image, sent 09:58]`). A day is the owner's local day, told for each turn
  from the second the device stored its first row (the row's `at:` tag) with the UTC
  offset that was in force at that moment. A change of clocks (summer time) never
  moves an earlier turn to another day, so the history stays append-only, and a day
  starts at the exact local midnight in half-hour and 45-minute zones too. Changing
  the device's time zone re-tells every earlier turn in the new zone, once. A row
  stored before this tag existed keeps only its hour: its day is told at the start
  of that hour, and follows the turn before it by at most the one day an hour can
  be wrong by (so a half-hour zone cannot repeat a day). A turn with a time to the
  second is never moved, so a clock that was set back does not hold later turns up.
  A history that starts after
  a fold or an overflow shift tells its days from the row it starts after, so they
  match the full history. A row stored before the clock was set has no date and
  opens no marker.
- **Repeats collapse.** Consecutive identical owner messages with nothing
  answered in between are one line with a count: `- user (x37): ...`. Only
  messages sent close together on one local day fold: within 2 minutes of the
  message before it when both carry a time to the second, else in the same hour
  (and never when that hour holds a local midnight, as in a half-hour zone). A
  steady run, each message 50 seconds after the last, is one line however long it
  grows, and an earlier line never changes as the run grows. A job's output
  between two of the messages does not break the run, and the message being
  answered never joins it. The same message sent
  yesterday and again today, or a minute either side of local midnight, stays
  two lines under their own days.
- **Append-only between boundaries; fold before shift.** A closed turn's bytes
  depend only on its own rows, so each turn's history continues the previous one
  byte for byte. The history is every turn after the fold anchor. It grows
  append-only to the head's soft mark (`historySoftBytes`), where a fold is due;
  the fold takes the oldest turns beyond the newest `historyKeepBytes`, at most one
  fold step (an owner's own Compact threshold below the soft mark folds earlier and
  keeps a tail in proportion). Only past `historyMaxBytes` (or its row bound) are the oldest whole
  turns hidden, down to the soft mark, never one row per message and never the
  newest turn or the last answered one; the hidden rows are named in the request
  (below) and make the next fold due. Nothing moves at local midnight: today folds
  only when today alone is longer than the keep mark. The shift is kept with the
  fold state (`/data/chatsum.txt`), so a restart renders the same history.
- **A head at v5.1.1's room keeps v5.1.1's window.** When a head's derived history
  is no larger than v5.1.1's (the 85 KB floor: a custom endpoint, a Cumulo head
  until the routed lift), the window is v5.1.1's exactly: past high water (the
  byte budget, or 64 rows) the oldest whole turns drop until half of it is left,
  and the owner's current local day is a floor under it (its turns never shift out
  and never fold, up to 16 KB, the newest first; past that cap the oldest quarter
  of the day leaves at once). Each turn's day is the one its row was stored on, in
  the owner's zone at that time, so a clock-change day is still one day.
- **Nothing leaves before a refusal.** The request goes as composed. Only when the
  wire refuses its body for its size (or a provider refuses it for its window)
  does the turn give anything up, in one order (`turn_fit.h`): the replayed tool
  rounds' arguments and older rounds, then recall (only when its bullets are in the
  body), the attached skills, history before today (oldest
  whole turns first), today's turns beyond the newest exchange, the newest unread
  results, the middle of the input, the tools, the skills list and standing rules.
  The summary pair and the newest exchange are never dropped. Each step is measured
  on the request as rendered, and a step that would not make it smaller gives
  nothing and says nothing. A photo round that cannot carry its answer in time
  gives up older history first and reads its photos as text only when history is
  down to the newest exchange.
- **What is left out is said.** A history cut opens the input with `[LEFT OUT TO FIT
  THIS REQUEST]`, naming how many messages, their local time range, their row ids
  and a ready `memory_episodic {"kind":"message","before":"<first row kept>"}` call.
  While rows after the fold anchor are not in the request (a shift hid them, or the
  history is still being read after a restart), or the summary is more than a day
  old, the input opens with one `[CONVERSATION COVERAGE]` line that names them the
  same way, or the summary's age.
- **One message's cap.** A single message over 6 KB is clipped once, at a fixed
  cap, so its bytes never change as the history grows (12 KB on a head with a
  60k-token request or more).
- **The rows ride a cache in PSRAM.** The chat's rows after the fold anchor are
  kept between turns (`conv_cache.h`: up to 3 chats, each up to its history budget
  and a quarter): a turn reads only the rows newer than the newest it holds, which
  are still in the store's RAM ring, so a steady-state turn reads no card row.
  After a restart the cache fills newest first, a few pages per idle pump pass and
  at most one page of card reads on a turn (the duplicate-delivery check included),
  so the first turn after a restart carries the newest exchanges and names the rest
  as not read yet. Rows the store's index does not reach yet stay named until its
  background indexing reaches them; after three pages that find nothing the idle
  pump tries that chat again at a growing interval, so the fill resumes by itself. A fold trims it, a `/clear` clears
  it, and a photo record that arrives later patches it. A run of identical owner
  rows still folds into one as it is read.

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

The per-turn input opens with one line naming what the request runs on, so the model
can answer "which model are you?" exactly: `[THIS TURN RUNS ON] mistral /
mistral-large-2512 (asked for mistral-large-latest).` The model is the one that provider
last answered with for that alias since the device started (else the alias resolved through
its stored list), and "(asked for ...)" appears only when the two differ. When the device knows
only a `-latest` alias (no list names the model behind it yet), the line adds "That is an
alias: add no version, size or family from memory." When the chosen
head is not the one answering and something holds it off, one sentence says what and when
it comes back: paused by the owner, by the owner's monthly limit or by the overall spending
limit, out of credit, its key refused, a request refused with HTTP 403 (back when it is
checked again), or over its monthly budget ("Anthropic, the chosen head, is out of credit
and returns automatically once credit is added."). The line then ends "When asked
which model you run, name this one and say why the chosen head is not running this turn."
Without that sentence it ends "When asked which model you run, name this one only." The
line is built for each request from the provider that request goes to: each host a
single-shot turn tries, the host a tool-loop turn starts on (past a parked or paused head)
and the host it moves to mid-turn. It rides the input, never the cached system prompt,
which names no model at all. The turn page (`/api/lastturn`) shows it as the first line of the input section.

## Diagnosing a confusing reply

Work through the blocks above in order: open that turn's **Turn anatomy** in the
Chat tab (or `/api/lastturn` for the most recent one). Its header names the
model the answering provider really ran (its answer's `model` field; a `-latest` alias
Mistral echoes is resolved through Mistral's own model list, so `mistral-large-latest`
reads `mistral-large-2512 (asked for mistral-large-latest)`), the provider chain, and
measures the device history sent in the prompt - the summary,
the recent window, the running memory and the recalled items - so a stateless
loop turn no longer reads as a chat with no memory of earlier turns. The legend
under section 1 lists every `##` section the prompt carries (an ambiguous message
with little device history is the most common cause of a non-sequitur). The page
also shows whether a `[MEMORY RESULTS]` or `[FRESH RESULTS]` block led the input,
exactly which memories were recalled, and - in section 4 - every tool call the
model made mid-turn with its arguments and result.

## An answer that is not one

Template filler where an answer belongs ("placeholder", "<reply>", "your reply here", only
dots) gets one more round, on the forced final round too, and is never delivered: a model
that repeats it ends the turn with "That didn't finish". A word the owner's message holds
is an answer.

A reply that promises later work ("I'll finish this next turn", "I'll report back") is not
held to what stands behind it on any path: the claim check that sent it back was deleted in
v5.2.0 (lane RB-Q1b), and the line a cut or unreadable answer got for it went with the reply
guard (lane RB-Q1c).

An earlier owner remark with no reply shows as `[no reply]` in the history; nothing checks
whether a reply takes it up (the reply-scope check and its `[REPLY SCOPE]` line were deleted
in v5.2.0, lane RB-Q1b).

No sentence is removed from a reply, on any delivery path (text or spoken): the reply
guard's drops of a sentence echoing the request's instructions and of a sentence telling the
owner to operate an internal object were deleted in v5.2.0 (lane RB-Q1c). The send seam is
specified in [turn-contract.md](reference/turn-contract.md#the-send-seam-nothing-is-removed-from-a-reply).

A chat line takes the tool rounds it needs within the turn's round budget and wall clock,
like any other turn: the 2-round cap on plain chat turns was deleted in v5.2.0 (lane
RB-Q1c), because it ended work before it was done.

## What the device checks in a reply (since v5.2.0)

The device no longer reads a reply's words against what the turn did. The claim check
(a scanner of ten claim classes, its correction round and the parenthesized Correction and
Note lines it appended), the spawn-claim line, the owner-action and reply-scope checks and the
refusal of a memory write that stated a cause or a daily target were deleted in v5.2.0
(lane RB-Q1b): they read one turn and matched words, so they sent correct answers back or
appended lines to them (39 of 702 replies in the owner's month carried such a line). The
model's reply is delivered once, as written; `tools/release_gate/check_no_harness_lines.py`
fails a build whose image holds such a line. What stays:

- the turn's effects record in RAM (at most 32 entries, never stored): every tool call and
  its result, the memory writes, the scratchpad save, the sub-agents the job queue accepted,
  the sub-agent results the turn read. The history row's `fx:` tag and the empty-answer
  check read it; RB-FX stores the chat's recent effects from it, and the truth check (below)
  reads both;
- the final-answer checks of [the final check](reference/turn-contract.md#the-final-check):
  the turn shape (a field the device cannot use) and a placeholder answer;
- the device's own lines about its own actions, each a plain "Note: ..." line after the reply:
  a `tts` that did not play gets a note that it could not be spoken aloud, with the reason; a
  voiced text cut to the speech bound says so, and
  a chat that may not start sub-agents is told so. A sub-agent intent that did not start
  reaches the head as a `[SPAWN REFUSED]` line on the chat's next turn;
- `/api/state` keeps `claimsCorrected` and `claimsTailed` at 0 for the readers that poll them.

### The truth check

What replaces the claim check does not read the reply for words either. It asks one small
model one question, measured before it was built (`tests/evals/measured/probe.json`): does
this reply claim a stored change (a row in a file, a file, a note, a memory, a routine, a
setting, a message sent) that no listed effect backs? The question carries the owner's
message, the reply, this turn's effects (its tool calls, and what the answer's own fields
will store) and the chat's recent effects (the stored ring), never a photo. With the recent
effects a true "I logged it at 13:02" is backed and is not a hit.

- **Enforce** (a head whose probe model passed the measurement: Anthropic direct, with
  claude-haiku-5-5): a yes holds the answer for the turn's one correction round. The head gets
  one device item that states what the device did and nothing else
  (`[device] effects this turn: ... Recent: ...`), and what it answers then is delivered as
  written. At most one hold a turn.
- **Receipt-only** (every other head, a turn that read outside content, a chat whose ring is
  not loaded from the card): never a probe.
- **The receipt line**: when the last write of a kind failed this turn (and no later one of
  that kind landed), one separate message follows the reply, `Note: the file change in this
  reply did not go through (<the tool's error>)`. On an enforce head it is sent only when the
  probe says the delivered reply reports success, or when the probe gave no answer; on a
  receipt-only head it is always sent.
- The probe fails open: no answer within 8 s, an error or an answer that is not the JSON
  object delivers the reply and is counted. Modes and counters per head live in
  `/data/truth_state.json`; v5.2.0 never moves a head to shadow mode on its own.

A sub-agent result for a live-data task (a status, the weather, prices, news) that
ran no tool is marked `[NOT CHECKED]`, and every `[FRESH RESULTS]` line says how
many tools ran (`[tools: 2, web: 1]` or `[no tools ran]`) and, when the provider's
record names them, which ones (`[ran: web_search x2, notion x0]`: a tool the run was
offered and never used counts 0). The spawn ledger keeps the same names for two
hours, with the sub-agent's task (the head's own words, never the sub-agent's reply) and
a note that why it picked those tools is not recorded, so "why did it use Notion?" is
answered from what the sub-agent was asked and ran. The block opens with one line telling the head to state only
facts the results contain, to keep the sources a result names, and to label anything
from an earlier answer as earlier.

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
  ("already pinned") and when a write was refused; a refused write or update also says
  that nothing keeps or retries it, so the reply never promises a later save;
- `[FRESH RESULTS]`, where each sub-agent result says how many tools it ran, and a
  live-data result with none opens with `[NOT CHECKED]`;
- the `device.log` tool: the device's own durable log (settings saves, boot
  lines, panics, failures that belong to no turn), read in bounded 4 KB chunks.
  `match` filters to lines carrying up to three literals, `lines` caps the
  answer, `scan_kb` bounds the search; when no log file can be read the tool
  answers from the in-memory tail and says so. It is the owner's: a member's or a
  guest's turn is not offered it.

The sub-agent runtime cap (about 60 seconds for a synchronous sub-agent) applies
to spawned sub-agents only, never to the head's own turns or the nightly dream. If
nothing read names the cause, the right answer is that the cause is not recorded,
and which read would show it.

## Fetching what a view left out - `results.get`

Every clip in a turn is a **view**, not a loss. The full text of a clamped tool
result or an overflowed sub-agent result lands in a PSRAM ring, and the model can
widen the view on demand:

- `results.get(tag, offset?)` - returns a bounded page of the stored result whose head
  names the next part (`bytes 0-8128 of 51234; next part: "offset":8128`, or `; end of
  result`). A result over 64 KB keeps its first 64 KB and ends saying how long it was.
  An offset inside a character starts at the next one and the head says so. Unknown tags
  point at `memory.episodic`, which holds the durable copy on SD. The hosted daemon keeps no
  ring: a result it clamps ends "...[truncated]".
- `results.list` - tag / kind / name / size for everything currently fetchable.

Tags are `r<n>` for tool-result spills and `sub:<jobtag>` for sub-agent results.
The truncation marker and the `[FRESH RESULTS]` stubs embed the tag directly, so
the model never has to guess one. See [memory.md](memory.md) for the gradient rules.
