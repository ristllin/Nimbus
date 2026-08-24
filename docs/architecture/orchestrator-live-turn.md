# Orchestrator Live-Turn Integration

How the Part B "World" memory - the vector store, scratchpad, episodic history,
and the `memory.*`/`session.*` tools - is wired into the **live LLM turn**, so the
orchestrator sees a composed World prompt, recalls and writes memories, and drives
sub-sessions within a single conversational turn.

This is the runtime that binds the portable engines to a real provider round-trip.
For the engines and the context assembler themselves see
[orchestrator-world.md](../orchestrator-world.md); for the field-by-field wire
contract see [../reference/turn-contract.md](../reference/turn-contract.md); for
the tool surface see [../reference/tool-catalog.md](../reference/tool-catalog.md).

## The design: a hybrid turn contract, not a function-calling loop

A turn is still externally **"one message = one turn"** - the LLM is called once
and returns one `orch_turn` JSON object. Memory is folded into that single call on
two sides:

- **Read side (automatic injection).** Before the provider call, `runTurn`
  pre-composes the World system prompt via the host-tested `assembleContext()`:
  the immutable rules, directive, capability manifest, running-sessions digest,
  scratchpad, and top-K associative recall from **one** embed of the user text.
  No LLM round-trip is spent to obtain this context.
- **Write side (optional turn arrays).** The turn contract carries optional
  `mem_write[]` / `mem_query[]` / `session_ops[]` arrays. After the response is
  parsed, `applyTurn` dispatches them **in-process** through the existing
  `ToolRegistry` - again with no extra LLM round-trip. `mem_query` results are not
  returned inline; they are parked and echoed into the *next* turn's inputs.

Both sides run on the `tg_poll` turn task, which is off the main-loop watchdog, so
a multi-second turn does not reboot the board.

## Read side - recall then assemble

`runTurn(inputs, chatId, userText)` composes context before choosing a host:

1. **Associative recall (one embed, fail-open).** When `userText` is non-empty and
   free heap is at or above `ORCH_RECALL_MIN_HEAP` (52000 bytes, equal to the turn
   hard floor - the recall embed is a *second* TLS handshake that must fit ahead of
   the LLM call), `memory::recall(userText)` embeds the message once and runs a
   top-K cosine `search` over the VDB under the retrieval config. Synthesis turns
   (empty `userText`) skip recall. Any embed failure returns empty content and the
   turn proceeds - recall never blocks a reply.
2. **Prompt assembly.** `composeInstructions(recalled)` builds a `ContextInputs`
   and calls `assembleContext(ci, kContextBudgetMax)` (`kContextBudgetMax = 32768`).
   The composed prompt, in the assembler's priority order, is:

   | Section | Source |
   |---|---|
   | Immutable rules + output contract | `ORCH_ROLE` (the `orch_turn` field spec) |
   | Directive (+ `[RUNNING MEMORY]`) | `g_mem.directive()` + `g_mem.model()` |
   | `## CAPABILITIES` | `renderCapabilities(hw, memory::registry().manifest())` |
   | `## RUNNING SESSIONS` | `renderSessions(sessionInfos())` |
   | Scratchpad | `memory::scratchpad()` |
   | `## RELEVANT MEMORIES` | the recalled top-K bullets |
   | `## HOW YOUR MEMORY WORKS` | the static `MEMORY_EXPLAINER` |

   The capability manifest lists tools **live from the MCP registry manifest**, so
   it never drifts from what actually dispatches. The `hw` descriptor is mostly
   static (ring/screen/mic/speaker present; no battery/SD/BLE) with the
   Telegram capability read live from `telegram::enabled()`. The composed prompt is
   also stashed in `g_lastInstructions` so the `PROMPT?` console command can dump it
   without a provider round-trip.

The user/conversation text lives in `inputs` (separate from the system-prompt
budget), so message length is preserved independently of the budget.

## Write side - the optional turn arrays

`parseTurn` accepts a turn whose only required fields are the three strings
(`reply`/`memory`/`ask`). Every array is optional, and the live-integration arrays
are parsed **tolerantly** - a malformed item is dropped, never a whole-turn error,
so one slightly-off item can't lose a good reply. Each array is capped to
`kAgentMaxJobs` (6). `applyTurn` dispatches them after the model-memory update:

- **`mem_write[]`** - `{content, importance (clamped [0,1], default 0.5),
  permanent (default false)}`. Dispatched to `memory.write` (embed + store).
  `content` is byte-capped to `kMemModelMax` (1200). This is the **only** path that
  creates a durable associative memory - the model must ask for it explicitly.
- **`mem_query[]`** - search strings. Dispatched to `memory.search`; the hits are
  accumulated into `g_pendingMemResults` and injected into the next turn's inputs
  as a `[MEMORY RESULTS]` block. This is the deferred-result pattern the sub-agent
  `[FRESH RESULTS]` channel already uses (`takeFreshResults`): the model asks in
  turn N and sees the answer in turn N+1, with no mid-turn network hop.
- **`session_ops[]`** - `{op, id, task, provider, model}` with `op` in
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

## Why not a provider function-calling loop

The hybrid is a deliberate alternative to native tool-calling:

- **Single-shot adapters.** `openai_responses::orchTurn` and `orchTurnAnthropic`
  are both `(convId, instructions, inputs) -> outJson`; neither sends a `tools[]`
  array nor handles `tool_calls`. A tool loop would be a large stateful rewrite of
  the one proven turn path, duplicated across two providers.
- **Blocking-TLS embed cost.** `embeddings::embed()` is a blocking TLS round-trip
  (1–3 s typical, up to ~20 s worst case). A mid-turn tool loop would add N
  sequential network hops per turn and stall the Telegram `getUpdates` long-poll
  the whole time. The hybrid does the one embed we control, up front,
  deterministically.
- **Backward compatibility.** Adding optional arrays is transparent - an old
  six-field turn still validates. Native tool-calling would change the request
  envelope on both providers.

## Episodic auto-capture

Memory has two capture paths with different triggers:

- **Episodic history is automatic.** Before a turn runs, the owner's message is
  captured (`captureSession` + `captureMessage("user", …)`) so the history is
  durable and queryable even if the turn later fails; after the turn, the
  assistant's `reply` and `ask` are captured. The store is an
  `InMemoryEpisodicStore` bounded to 500 messages and persisted whole-blob to
  LittleFS, queryable via `memory.episodic`.
- **Associative (VDB) memory is explicit.** The vector store is written **only** by
  an `mem_write[]` tool call - conversation is never auto-mirrored into the VDB.

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
immediately after close - so the single mbedTLS arena never holds more than two
resident sessions.

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
selectable (`api.openai.com`, `gpt-4o-mini-transcribe`). Both expose
`POST /v1/audio/transcriptions` as multipart and return `{"text": …}`. Because
`solide::audio::recordToFile` emits **headerless** 16-bit mono PCM, `stt::transcribePcm`
wraps it in a canonical 44-byte-header WAV before upload. Transcription is
fail-open: any failure returns `""` and the caller nudges the owner to send text.

**TTS (`tts::synthesizeToFile`).** The provider is `store::ttsProvider`, defaulting
to **Mistral** (`voxtral-mini-tts-latest`), with OpenAI selectable
(`gpt-4o-mini-tts`); both use `POST /v1/audio/speech`, and the audio is streamed
straight to a LittleFS file so a clip never sits in internal heap. The response
shapes differ: OpenAI honors `response_format` (`wav`/`mp3`) and returns raw binary;
Mistral ignores it and always returns MP3 as base64 inside `{"audio_data": …}`,
which is stream-decoded to the file. Since the device speaker plays WAV/PCM only,
MP3 output is the **Telegram-audio** path, not an on-device readout.

**Outbound media on the poll task (`sendMedia`).** A send publishes a single
pending-media slot under the `g_mediaMux` spinlock; the `tg_poll` task drains it via
`drainMedia()` **after** closing the poll socket, so only one TLS session is ever
resident (the single-TLS invariant). Kinds `document`/`photo`/`voice`/`audio` map to
the Bot API `sendDocument`/`sendPhoto`/`sendVoice`/`sendAudio` methods.

**Spoken reply path.** In `applyTurn`, when a reply exists, `store::ttsEnabled()` is
on, `tts::available()`, and the `chatId` is a real numeric Telegram chat (synthetic
`"serial"`/`"web"` ids are skipped), the reply is synthesized to `/reply.mp3` and
handed to `sendMedia(chatId, "audio", …)` - Telegram audio is the audible path
because the bench speaker is disconnected.
