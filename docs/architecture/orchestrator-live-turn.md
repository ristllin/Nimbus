# Orchestrator Live-Turn Integration

This page explains how the Orchestrator's World memory (the vector store,
scratchpad, episodic history, and the `memory.*`/`session.*` tools) takes part in
one **live model turn**: the Orchestrator sees a composed World prompt, recalls and
writes memories, and drives sub-sessions within a single conversational turn. Read
it when you change the turn path, debug recall, or extend the turn contract.

This is the runtime that binds the portable engines to a real provider round-trip.
For the engines and the context assembler themselves see
[orchestrator-world.md](../orchestrator-world.md); for the field-by-field wire
contract see [../reference/turn-contract.md](../reference/turn-contract.md); for
the tool surface see [../reference/tool-catalog.md](../reference/tool-catalog.md).

## The design: World memory around one turn

A turn is still externally **"one message = one turn"**: one owner message gets
one reply, carried by one terminal `orch_turn` JSON object. By default that object
ends the bounded **head tool loop** (NVS `orchLoop`, on by default): the model may
call registry tools such as `memory.*`, `session.*` and `web.search` over several
rounds, see each result, and then finish with `orch_turn`. The loop is capped at
`ORCH_LOOP_MAX_ROUNDS` (8) rounds and an `ORCH_LOOP_DEADLINE_MS` (10 minute) wall
clock, both owner-tunable (`orchLoopRnds`, `orchLoopDlS`). With the loop off, or on
a host whose wire has no tool loop (Z.ai, a custom endpoint), the LLM is called once
and returns one `orch_turn`. The per-provider wire for both paths is in
[provider-wire.md](../provider-wire.md); what the model sees is in
[turn-anatomy.md](../turn-anatomy.md).

Memory is folded around the turn on two sides, whichever path runs:

- **Read side (automatic injection).** Before the provider call, `runTurn`
  pre-composes the World system prompt via the host-tested `assembleContext()`:
  the immutable rules, directive, capability manifest, running-sessions digest,
  scratchpad, and top-K associative recall from **one** embed of the user text.
  No LLM round-trip is spent to obtain this context.
- **Write side (optional turn arrays).** The turn contract carries optional
  `mem_write[]` / `mem_query[]` / `session_ops[]` arrays. After the terminal
  `orch_turn` is parsed, `applyTurn` dispatches them **in-process** through the
  existing `ToolRegistry`, with no extra LLM round-trip. `mem_query` results are
  not returned inline; they are parked and echoed into the *next* turn's inputs.

Both sides run on the `tg_poll` turn task, which is off the main-loop watchdog, so
a multi-second turn does not restart the device.

## Read side - recall then assemble

`runTurn(inputs, chatId, userText)` composes context before choosing a host:

1. **Associative recall (one embed, fail-open).** When `userText` is non-empty and
   free heap is at or above `ORCH_RECALL_MIN_HEAP` (12000 bytes, the same value as
   the turn hard floor `ORCH_TURN_HARD_FLOOR`: the recall embed is a second TLS
   handshake, but mbedTLS buffers live in PSRAM, so its only internal-heap cost is a
   small HTTP/JSON transient; it stays a named floor so a heavier build can raise
   it), `memory::recall(userText)` embeds the message once and runs a top-K cosine
   `search` over the VDB under the retrieval config. Synthesis turns
   (empty `userText`) skip recall. Any embed failure returns empty content and the
   turn proceeds - recall never blocks a reply.
2. **Prompt assembly.** `composeInstructions(recalled)` builds a `ContextInputs`
   and calls `assembleContext(ci, kContextBudgetMax)` (`kContextBudgetMax = 32768`).
   The composed prompt, in the assembler's priority order, is:

   | Section | Source |
   |---|---|
   | Immutable rules + output contract | `ORCH_ROLE` (the `orch_turn` field spec) |
   | Identity (device name, time, speaker) | composed in `compose.cpp` |
   | Directive (+ `[RUNNING MEMORY]`) | `g_mem.directive()` + `g_mem.model()` |
   | `## CAPABILITIES` | `renderCapabilities(hw, memory::registry().manifest())` |
   | `## RUNNING SESSIONS` | `renderSessions(sessionInfos())` |
   | Chat summary | the chat's folded summary |
   | Recent conversation | the per-chat window |
   | Scratchpad | `memory::scratchpad()` |
   | `## RELEVANT MEMORIES` | the recalled top-K bullets |
   | `## HOW YOUR MEMORY WORKS` | the static `MEMORY_EXPLAINER` (v1 prompt only; the v2 prompt, NVS `orchPromptV2`, drops this block) |

   The capability manifest lists tools **live from the MCP registry manifest**, so
   it never drifts from what actually dispatches. The `hw` descriptor is read live
   each turn: the ring and touchscreen from HAL health and the fault mask, mic and
   speaker from the fault mask, battery from the board's battery-ADC build flag (set
   on every ESP32-S3 build), SD from the live mount state, the file store from
   `files::available()`, Telegram from `telegram::enabled()`, and the Voice replies
   toggle from `store::ttsEnabled()`. Wi-Fi is always reported present (a turn only
   runs with Wi-Fi up) and Bluetooth always absent (it runs only in Notifier mode).
   The composed prompt is also stashed in the engine's `lastInstructions_` (read
   through `agent::orchestrator::lastInstructions()`) so the `PROMPT?` console
   command can dump it without a provider round-trip.

The user/conversation text lives in `inputs` (separate from the system-prompt
budget), so message length is preserved independently of the budget.

## Write side - the optional turn arrays

`parseTurn` accepts a turn whose only required field is `reply`; `memory` and
`ask` are optional strings (absent reads as `""`, so a turn that leaves them out
still applies its side effects). Every array is optional, and the live-integration arrays
are parsed **tolerantly** - a malformed item is dropped, never a whole-turn error,
so one slightly-off item can't lose a good reply. Each array is capped to
`kAgentMaxJobs` (6). `applyTurn` dispatches them after the model-memory update:

- **`mem_write[]`** - `{content, importance (clamped [0,1], default 0.5),
  permanent (default false)}`. Dispatched to `memory.write` (embed + store).
  `content` is byte-capped to `kMemModelMax` (1200). This and the `memory.write`
  tool (mid-turn on the tool loop, or over `/mcp`) are the only paths that create a
  durable associative memory - the model must ask for it explicitly.
- **`mem_query[]`** - search strings. Dispatched to `memory.search`; the hits are
  accumulated into the engine's `pendingMemResults_` and injected into the next turn's inputs
  as a `[MEMORY RESULTS]` block. This is the deferred-result pattern the sub-agent
  `[FRESH RESULTS]` channel already uses (`takeFreshResults`): the model asks in
  turn N and sees the answer in turn N+1, with no mid-turn network hop.
- **`session_ops[]`** - `{op, id, task, provider, model, skill, name, project,
  attach}` with `op` in
  `spawn|tell|poll|terminate|list`. `spawn` enqueues a sub-agent (the same enqueue
  path as the deprecated `spawn[]` array); `terminate` cancels the job named by
  `id`; `list` is a no-op because the running-sessions digest is already injected
  every turn. `tell`/`poll` are not supported on this fire-and-forget fabric - the
  adapter's `answer()` only resumes a `NeedsInput` job, it cannot inject a
  follow-up user message into a live sub-agent conversation. Rather than fail
  silently, `applyTurn` feeds that boundary back through the `[MEMORY RESULTS]`
  channel so the model learns to use `spawn` + `[FRESH RESULTS]` instead.

If any `mem_write` ran, vectors and the scratchpad are persisted once at the end of
the dispatch. Writes run **before** `reply` is delivered, so a "remember X, ok?"
turn stores the fact and then confirms it.

## Why the turn arrays still exist alongside the tool loop

The tool loop can do the same memory and session work mid-turn as tool calls, so
the arrays are not the only write path. They stay because:

- **One contract on every path.** The arrays ride the `orch_turn` object itself,
  so the single-shot path (loop off, or a Z.ai or custom host with no tool loop)
  can still write memory, query it, and manage sub-sessions without native tool
  calls. On the loop path, a final answer can still carry its side effects with
  it.
- **Recall stays up front.** `embeddings::embed()` is a blocking TLS round-trip
  (1-3 s typical, up to ~20 s worst case), and every loop round is another
  sequential network hop through the single TLS slot. The read side spends its
  one embed before the first provider call, deterministically, instead of making
  the model ask for context it always needs.
- **Backward compatibility.** Adding optional arrays is transparent - an old
  six-field turn still validates.

## Episodic auto-capture

Memory has two capture paths with different triggers:

- **Episodic history is automatic.** Before a turn runs, the owner's message is
  captured (`captureSession` + `captureMessage("user", …)`) so the history is
  durable and queryable even if the turn later fails; after the turn, the
  assistant's `reply` and `ask` are captured. With an SD card mounted the store is
  an append-log of day streams under `/mem/episodic` with uncapped history; with
  no card it falls back to an `InMemoryEpisodicStore` bounded to 500 messages and
  persisted whole-blob to LittleFS. Either way it is queryable via
  `memory.episodic`.
- **Associative (VDB) memory is explicit.** The vector store is written only by an
  explicit `mem_write[]` item or `memory.write` call - conversation is never
  auto-mirrored into the VDB.

## Concurrency - a single writer on the turn task

All memory state is mutated from exactly one task: the `tg_poll` turn task. The web
and MCP surfaces never mutate `g_mem` directly. Instead they **stage** their intent
via flags (`g_memClearReq`, `g_cfgReloadReq`), which the turn task drains through
`drainStaged()` at the top of `handleMessage` and `pollJobs` - i.e. before the next
turn uses them. So directive edits and memory clears take effect at the next turn
boundary, on the writer task.

Cross-task **reads** are made safe without a second lock: the model-memory string
is mirrored into a fixed `g_memEcho` buffer (`strlcpy`, always NUL-terminated) that
the AsyncTCP web task reads, so the worst case is torn *text*, never a dangling
pointer across a realloc. Shared `Config` reads on the web/router side are
serialized by the `ConfigLockGuard` spinlock (`portMUX_TYPE s_cfgMux`). The lock
order, where both are held, is **config → memory**, matching the `ConfigLockGuard`
precedent.

Total concurrent TLS is bounded by the `tls_arbiter` work slot: Telegram holds its
own persistent session and every other TLS user (turn, dispatch, poll, STT, TTS,
media upload) must `acquireWork` before opening a `WiFiClientSecure` and release it
immediately after close - so the single mbedTLS arena holds at most the Telegram
session plus one work session (two work sessions when **Concurrent connections**
is set to 2, applied at boot). The cloud relay keeps its WebSocket open between
requests, but its dial (connect, upgrade, hello) takes the slot, and so does every
response whose body is over 8 KB, its frame one 8 KB chunk at a time with the slot
released between chunks, so a turn waiting for the slot gets it within one chunk.
Heartbeats, acks and small responses, including the dashboard's 6-7 KB status polls,
take no slot, so the cloud dashboard stays live during a turn. The rule is on the
body, not the frame: base64 makes the frame about a third larger, and deciding on
the frame put every poll in the slot. `tests/hil/test_l41_tls_slot_guard.py` (a source
check, no board) fails on any new TLS connect outside the slot.

## PSRAM-backed mbedTLS - the unblock for every live turn

`ESP.getFreeHeap()` reports **internal** RAM only: ~218 KB at boot but ~63 KB by
turn time in Orchestrator mode once Wi-Fi, the web server, the World-memory
subsystem, and the always-on turn task are up. A single mbedTLS handshake wants
~40 KB contiguous and was failing with `-0x7F00` (`MBEDTLS_ERR_..._ALLOC_FAILED`)
at ~50 KB.

The S3's 8 MB PSRAM is not in the default malloc pool, so at startup - guarded by
`ESP.getPsramSize() > 0` - the firmware installs a PSRAM-backed mbedTLS allocator:

```c
mbedtls_platform_set_calloc_free(
    [](size_t n, size_t sz) -> void* { return heap_caps_calloc(n, sz, MALLOC_CAP_SPIRAM); },
    free);
```

TLS record/session buffers are CPU-processed, never DMA, so external RAM is correct
here; every driver DMA buffer keeps using the default internal allocator, untouched.
This lifts the whole handshake off the scarce internal heap and unblocks every live
turn - and recall's second TLS handshake within a turn.

## Inbound Telegram: albums, bursts and the storm guards

Every inbound Telegram page goes through one receive cycle on the `tg_poll` task
(`nimbus/net/tg_receive.h`, host-tested end to end in `test_tg_burst`; the device seam
is `src/agent/telegram.cpp`). One cycle:

1. **Fetch** one `getUpdates` page at the RAM offset cursor (the archive sees it as
   read, before anything else; an update already saved this boot costs no card read).
2. **Settle** (`nimbus/net/tg_settle.h`). A phone uploads an album one photo at a time,
   so the first poll often holds only part of it. The page is held (re-polled with
   timeout 0 after the 1 s pause, on the same keep-alive socket) until it has not grown
   for 3 s with media or 1.5 s for text only, or it is full, or its newest Telegram date
   is older than that window plus 2 s, or 15 s have passed since its first item was
   seen. A hold classifies, acks and stores nothing; local turns, job polling and reply
   sends keep running between re-polls. Keeping the poll socket open is safe only
   because every other TLS session of the task opens through `workConnect`, which
   closes it first.
3. **Break duplicates and classify** (`nimbus/net/tg_dup_guard.h`). A message already
   answered (same chat, message id and date), or the same content in the same chat
   within 2 minutes and before its answer went out, is acked and skipped, with one
   owner notice per episode. Text joins its chat's turn; a photo or document becomes a
   media segment of that turn (its caption with it, at full length) and takes one of
   ten download slots (a lazily allocated PSRAM array); a voice note is a turn of its
   own; a command is a turn of its own. An eleventh media item waits for the next page.
4. **Run each turn in order.** Past the hourly ceiling (60 inbound head turns in any
   rolling hour, 12 of them reserved for the owner's chat, so the other allowed chats
   stop at 48; commands do not count) a turn is skipped, with one owner notice per
   trip. Otherwise every photo of the turn is fetched, hashed, handed to the archive
   (one download per file), described and stored, in album order, with a typing
   indicator at most once per photo and only 4 s or more after the last; then the turn
   runs with every item of its bucket (`MessageBatcher::render`, the one label
   `nimbus/orch/media_label.h`). A failed item is part of the turn, never a separate
   reply, unless the turn has nothing else (no text, no caption, nothing fetched): then
   one plain reply says why, instead of a paid turn.
5. **Commit.** Only after the page's turns returned does the cursor move past it (a
   page holding a command checkpoints after each bucket), so a reset anywhere re-serves
   the page and the duplicate breaker skips what was already answered.

**The poison guard.** Media are acked only after processing, so a page whose processing
crashes the device would be served again on every boot. The work in flight is marked in
a record in RTC no-init memory (magic, build id, CRC; no settings write; a power-on
clears it): the page while it is classified, each photo while it is fetched and
described, each turn while it runs. A boot after a panic, watchdog or brownout counts a
try against the mark (`main.cpp` hands the reset reason to the poll task); after two
such resets on the same work it is skipped and only the owner's chat is told to send it
again: a crashing photo becomes "could not be fetched: it made the device restart twice"
in a turn that still answers everything else, a crashing turn is acked without running,
and a page that crashes while it is classified is acked unprocessed. A page served
again keeps a mark that lies inside it, so the count survives a command page's
per-bucket checkpoints. The duplicate breaker and the hourly ceiling keep their state
in the same record, so a restart in the middle of a storm keeps it at one paid turn.

## Voice and Telegram-media adapters

Speech and media share one uploader and a provider-configurable STT/TTS pair.

**Shared multipart uploader (`http_multipart`).** A tiny RFC-2388
`multipart/form-data` POST over `WiFiClientSecure` that streams **one** file
straight from LittleFS so a multi-KB audio/image upload never sits in internal heap.
It is HTTP/1.0 `Connection: close` with the `Content-Length` computed up front from
the field framing plus the on-disk file size. Its TLS buffers come from the PSRAM
allocator above. It backs both the STT upload and Telegram `sendVoice`/`sendPhoto`/
`sendDocument`.

**STT (`stt::transcribe`).** The provider is `store::sttProvider`, defaulting to
**Mistral Voxtral** (`api.mistral.ai`, `voxtral-mini-latest`), with OpenAI
selectable (`api.openai.com`, `gpt-4o-mini-transcribe`). A Cumulo-keyed device
with no provider key of its own routes through the Cumulo router instead
(`/router/openai/v1/audio/transcriptions`, `gpt-4o-mini-transcribe`). All of them
expose `POST .../audio/transcriptions` as multipart and return `{"text": …}`. Because
`solide::audio::recordToFile` emits **headerless** 16-bit mono PCM, `stt::transcribePcm`
wraps it in a canonical 44-byte-header WAV before upload. Transcription is
fail-open: any failure returns `""` and the caller nudges the owner to send text.

**TTS (`tts::synthesizeToFile`).** The provider is `store::ttsProvider`, defaulting
to **Mistral** (`voxtral-mini-tts-latest`), with OpenAI selectable
(`gpt-4o-mini-tts`) and the Cumulo router used by a Cumulo-keyed device
(`/router/openai/v1/audio/speech`, `gpt-4o-mini-tts`); all use the `audio/speech`
endpoint, and the audio is streamed
straight to a LittleFS file so a clip never sits in internal heap. The response
shapes differ: OpenAI honors `response_format` (`wav`/`mp3`) and returns raw binary;
Mistral ignores it and always returns MP3 as base64 inside `{"audio_data": …}`,
which is stream-decoded to the file. The device speaker plays both: the speaker
path (`speakOnDevice`) asks for the format the effective provider actually emits
(`core::speakerTtsFormat`: WAV from OpenAI and the Cumulo router, MP3 from Mistral)
and plays WAV directly or decodes MP3 with the vendored minimp3, so a Mistral-only
device speaks on its own speaker too.

**Outbound media on the poll task (`sendMedia`).** A send publishes a single
pending-media slot under the `g_mediaMux` spinlock; the `tg_poll` task drains it via
`drainMedia()` **after** closing the poll socket, so the Telegram poll session is
not resident while the upload holds a work slot. Kinds `document`/`photo`/`voice`/`audio` map to
the Bot API `sendDocument`/`sendPhoto`/`sendVoice`/`sendAudio` methods.

**Spoken reply path.** Speaking is the model's choice, gated by the owner's
**Voice replies** toggle (`store::ttsEnabled()`, on by default). `reply.speak`
calls `speakOnDevice` and plays on the device speaker, refusing with a reason when
voice replies are off or the speaker volume is 0. The `tts` device action and
`reply.telegram` with `voice:true` go through the speak sink (`orchSpeakSink` in
`main.cpp`), which plays on the speaker first and falls back to a Telegram audio
message (`/tts.mp3` via `sendMedia(…, "audio", …)`) only when on-device speech is
not possible.
