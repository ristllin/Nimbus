# Provider wire - structured outputs, tool loops, and failure handling per provider

The technical companion to [turn-anatomy.md](turn-anatomy.md) and
[harness.md](harness.md): exactly how the turn contract is enforced on each
provider's API, how the tool loop rides each wire, and what happens when a
provider misbehaves. Adapters live in `lib/harness/src/providers/`; the shared
schema in `lib/core/include/nimbus/orch/orch_schema.h`.

## The contract has one source

Every field of the `orch_turn` response (reply, ask, memory, device[],
mem_write[], mem_query[], session_ops[]) is defined once, as `ORCH_D_*`
description macros in `orch_schema.h`. The same macros generate both:

- **the JSON Schema** sent to the provider (`ORCH_SCHEMA_BODY`), and
- **the prompt's field documentation** (`ORCH_FIELD_DOCS`, the numbered list at
  the top of the system prompt).

So the prose the model reads and the schema the wire enforces cannot drift -
editing the macro updates both, and a schema golden (`test/golden/`) pins the
result. This is why the prompt template contains field *descriptions* but no
JSON example: the response format is not free-form prose guidance, it is
carried by each provider's structured-output channel (below).

## How each provider enforces the response shape

The `orch_turn` contract reaches the model two ways. The **single-shot** turn
(tool loop off, or no tools supplied) carries the schema on the provider's
structured-output channel. The **tool-loop** round offers `orch_turn` as one
function tool the model calls to end the turn. Wire enforcement differs by
channel:

| provider | single-shot channel | tool-loop channel | schema enforced at the wire? |
|---|---|---|---|
| **OpenAI** (Responses API) | `text.format` `json_schema`, `strict:true`. The compaction fold is plain text: the fold prompt as `instructions`, the conversation as `input`, no format, no tools or connectors, `store:false`, reasoning effort low | `orch_turn` function tool, `strict:true` parameters | **Yes**, both channels - the model cannot emit an out-of-schema `orch_turn` |
| **Anthropic** (Messages API) | the `orch_turn` tool with `tool_choice:auto` - never forced (Sonnet 5.5, Opus 5.5 and Fable 5.1 answer a forced `tool_choice` with a 400); a prose answer is delivered as the reply. The compaction fold offers no tool: its reply is the summary | `orch_turn` offered alongside the registry tools (`tool_choice:auto`); a round that ends normally (`end_turn`, `stop_sequence`) with text and no call is the reply on any round, so a turn costs its tool rounds plus one request (`HeadLoopConfig::proseIsReply`; the nightly dream keeps the old stall path, since its product is the `orch_turn` fields) | **No** - the strict-grammar budget rejects a schema this size, so descriptions are stripped and the schema is advisory; its top-level `required` list is just `reply` and the tool description says to OMIT fields the turn is not using (the device parser treats every other field as optional), so the head stops re-emitting unchanged fields as empty values. The device parser + validator are the enforcement |
| **Mistral** | Conversations API, `response_format.json_schema`, `strict:true`. The compaction fold is a plain chat completion with no format: its reply is the summary | chat/completions, `orch_turn` function tool (no `strict` flag; the schema's shapes only - the field descriptions ride the prompt, so 10 KB of them no longer ride every round); on a person's turn `tool_choice:"auto"` and a text answer that stopped normally is the reply (RB-Q4b), on an unattended turn `"any"` | single-shot **yes**; loop **advisory** - the device parser + validator enforce it |
| **Z.ai** (direct, or Cumulo with the zai upstream) | chat/completions, `response_format:{type:"json_object"}` + a JSON-only instruction | chat/completions, `orch_turn` function tool with `tool_choice:"required"`; the forced final round offers only `orch_turn` | **No** - Z.ai answers `json_schema` requests with plain Markdown, so a single shot asks for `json_object`; the loop's schema is advisory. A reply that is still prose is delivered as the reply (nothing else rides it) |
| **Custom** (LAN / proxy) | chat/completions, `response_format.json_schema`, `strict:true` (schema-less retry on 400) | none (single-shot head) | when the backend supports it |

**"Strict at the wire, lenient at the parser."** Regardless of provider,
`parseTurn` (`lib/core/src/orch_turn.cpp`) is tolerant - unknown fields are
ignored, malformed items are dropped individually, string caps are applied on
copy. The engine reads every head reply through `parseHeadReply`: a reply that
IS one JSON object (bare, or the whole reply in one fence) is the turn; a reply
that opens like JSON but is not a turn (or is wholly a JSON array, or an array
of objects cut off before it closes) goes to the salvage path, so raw wire JSON never reaches a person; any other reply is prose and is delivered as the
reply alone - prose (even prose quoting JSON) never becomes device actions,
memory writes or spawns. The one prose exception is a turn envelope carried
inside the text (a fenced or bare JSON object with a string `reply` and at least
two other orch_turn fields): only the envelope's reply is delivered, its other
fields are dropped with a note, unless the owner's message asked for JSON, in
which case the shown example is content and is delivered whole (see
[turn contract](reference/turn-contract.md)). The portable device-action validator
(`orch_device_actions.cpp`) re-checks every device action, including the
protected-key refusal (API keys, provider routing), no matter what the wire
promised. A provider bug can degrade a turn; it cannot make the device execute
an action the validator refuses.

Per-provider strict-schema requirements (each a hard API constraint):

- OpenAI strict mode: a `const` needs an explicit sibling `type`.
- Anthropic: a nullable enum must be `anyOf:[{enum},{type:null}]`, not a union
  type.
- The schema's nesting depth exceeds ArduinoJson's default limit - every parse
  of it uses `NestingLimit(16)`.
- The `device[]` array is a flat discriminated union (`{type:"lights",...}`)
  because strict schemas reject heterogeneous keyed objects.

## The tool loop rides one canonical transcript

Every tool-loop turn is recorded into one device-owned `Transcript`
(`lib/core/src/orch_transcript.cpp`, host-tested): the seeded user input, then
per round the model's prose, its tool calls, and their results. The portable
controller (`runHeadLoop`, `lib/core/src/orch_head_loop.cpp`) owns rounds/
deadline/heap/byte caps and records into the transcript through
`hooks.transcript`; each provider adapter supplies a thin `step` closure that
re-renders its wire shape from that transcript every round. The loops are
therefore **stateless** - no provider-side conversation is retained between
rounds, and turn continuity lives on the device, not on a provider's server.

This is the invariant that makes a mid-turn provider switch possible (below):
executed tool results ride the transcript as data, so re-rendering a round
against a different provider replays nothing - the tools already ran.

**Calls sent with the final answer run before any reply.** A model may batch
tool calls (typically `memory_write`) in the same response as its final
`orch_turn`. The controller runs those calls once, before any reply is
delivered, then discards that premature reply - it was written before the
results existed - and the next round hands the results back, so the answer the
owner gets can only claim saves (or use data) it actually saw. On the forced
final round (tools withdrawn) such calls are not run. The OpenAI, Mistral and
Anthropic steps surface every sibling call, before or after `orch_turn`. An
Anthropic response cut by `max_tokens` inside its last tool call fails the
round instead (a half-written call is never run).

Each renderer speaks its provider's dialect from the same record:

- **Anthropic** (`renderAntMessages`, `anthropic.cpp`) rebuilds `messages[]`:
  the pinned user seed, then for each tool-carrying round an assistant message
  of `tool_use` blocks answered by a user message of `tool_result` blocks. The
  **prefill rule**: an assistant message is emitted ONLY for rounds that carried
  a tool call - a prose-only (stalled) round is never echoed, because a trailing
  assistant message plus forced `tool_choice` is a hard 400. With `proseIsReply` on (every
  turn but the dream) a prose-only round that ended normally is not a stall: it is the reply. The replay is the
  same on every loop wire: append-only while the request fits its body budget
  (every replayed byte counts: arguments, prose, results), so each round reads
  the previous request from the provider cache. A round over the body budget is
  refit before it is sent (lane RB-BGb): the oldest whole rounds fold into one
  line each in the seeded message, only as many as the overshoot needs, each
  naming its `results.get` tag; then the newest round's result is cut to a part
  that names the read of the rest; only then the prompt's context (the
  `tool_use`↔`tool_result` pairing an unanswered `tool_use` would 400 on is never
  broken). See "The cacheable prefix" in
  [turn-anatomy.md](turn-anatomy.md).
- **OpenAI** (`renderOaiInput`, `openai.cpp`) sends the full Responses `input[]`
  with `store:false` every round - the user seed, then per round the reasoning
  items, `function_call` items, and their `function_call_output` answers. There
  is **no `previous_response_id` and no `store:true`**: the server-side chain is
  gone, and with it the chain-poisoning failure class (an unanswered
  `function_call` left on a stored chain used to 400 every later turn). Reasoning
  models require their reasoning items replayed before each `function_call` under
  `store:false`, so `reasoning.encrypted_content` is captured into the transcript
  item's `meta` and re-emitted by the OpenAI renderer only.
- **Mistral** (`renderMistralMessages`, `mistral.cpp`) runs the loop on the
  stateless `/v1/chat/completions` wire (assistant `tool_calls[]` answered by
  `role:"tool"` messages) - NOT the Conversations API, which only the single-shot
  path keeps. Chat/completions requires tool-call ids of exactly nine
  alphanumerics, so any foreign id carried in by a failover (an Anthropic
  `toolu_…` or OpenAI `call_…`) is normalized to a deterministic 9-char digest
  (`mistralCallId`), applied to both the assistant `tool_calls[]` and their
  paired `tool` messages.

Wire rules shared across the renderers - each a hard requirement of the API it
serves:

- **Tool names**: providers require `^[a-zA-Z0-9_-]{1,64}$`, so dotted registry
  names (`memory.search`) are sanitized `.`→`_` with a reverse map on dispatch.
  Mistral additionally reserves its built-in connector names (`web_search`,
  `code_interpreter`, …); a registry tool that collides gets a `reg_` prefix,
  inverted before dispatch.
- **Serialization**: never `serializeJson` straight into the TLS client -
  per-chunk TLS records fragment the internal heap. Every request body is
  serialized into one PSRAM buffer and written as 4,096 B slices of it (the record
  size the TLS client uses anyway), with the photos base64-encoded into each slice
  at send time. While the DMA-capable pool is under 6 KB the transport waits between
  slices for the previous slice's buffers to come back (the pacing).
- **Each request carries its own budget**: the host and model that send a request
  decide its body budget (`wireBudgetFor`, `nimbus/orch/request_budget.h`), read every
  round, never a turn-global figure. So a round the fabric moves to a host with a
  smaller budget, or Large 4's step down to Medium, is refused locally, before
  anything is sent. No budget is under 85 KB (`providers.h kWireBodyMax`), the one
  every head had before v5.1.2; a head routed through Cumulo keeps it. A body over its
  budget is refused before the send (fail closed, named), so a component that escapes
  its cap costs one honest turn instead of a stall. Inside a tool loop the refusal does
  not end the turn: nothing was sent, so the loop (`runHeadLoop`) shrinks the
  arguments of calls that already ran to a short stub (each call keeps its id and
  name, so it stays paired with its result on every wire), folds the older tool rounds
  to one-line stubs, clips the newest round only if nothing else frees room, and runs
  the same round again (`nimbus/orch/body_budget.h` holds the one wording both sides
  read). The tool results a round replays are kept under the replay figure derived
  from the same budget, never under the wire's earlier figure (24 KB, Anthropic 16 KB).
  A flat single shot's one prompt string (the Mistral Conversations head, the custom
  endpoint) is held under 55,904 B (`flatPromptBudget`) with the same size wording, so the
  turn fits its history into it and builds the request again instead of failing on the
  string ceiling. A head turn's newest message (its per-turn block and the input) is held
  under the same figure with its own wording, and the loop gives up what that message holds
  (recalled memories first, then the middle of the input) before anything else.
- **A turn is sized for the form it goes out in**: the body budget above is filled only by
  a split prompt, where the history rides as messages: every Anthropic and OpenAI turn
  (their single shot is split too), and a Mistral or Z.ai turn that runs the tool loop. With
  the tool loop off, a Mistral head (the Conversations single shot) and a Z.ai head (the
  chat-completions single shot) send one prompt string, as a custom endpoint and a router
  upstream with no tool loop always do. The head's turn budget names that form
  (`turnBudgetFor(host, model, loopOn)` in `request_budget.h`: `flatTurn`, and
  `contextBytes()`, the flat prompt budget of 55,904 B instead of the body budget), and
  every reader of the head's caps and history marks derives from it, never from the body
  budget (`headContextBudget`, `test_orch_budget`). The one formula then leaves a flat turn
  no room for derived history, so it keeps the v5.1.1 history window and the 48 KB summary
  trigger, and recalled memories ride the request as in v5.1.1; the
  body budget of its wire does not change. `loopOn` is the owner's Tool use setting. A
  single turn that the memory gate sends as a single shot (entry heap under the loop's
  floor), or a bounded background turn on a fallback head, keeps the head's standing marks:
  when its history is over the string it is refused locally and fitted by the give-up
  order, with the note that names what was left out.
- **Photos inside the budget and the time fit**: each photo's window cost comes from
  its pixel size (read from its own header: JPEG, PNG, WebP, GIF), by the host's rule
  (Anthropic about width x height / 750 after its resize, OpenAI's 512 px tiles at
  high detail, Mistral's 16 px patches), and comes off the text budget (never under
  85 KB). The photo bytes count in the round's time fit. A photo round that cannot
  leave room for the host's minimum final answer inside its read window is not sent:
  older history goes first (the turn's give-up order, with its note), and only when
  none is left are the photos read as text on that host for the rest of the turn (the
  log line `transcribed: did not fit the <host> time window`).
- **Valid UTF-8, whole strings**: every provider answers a body that is not
  valid UTF-8 with a 400 that is the same on every host, and ArduinoJson stores a
  string of 64 KB or more as null. So every registry tool's text is repaired
  (invalid sequences become U+FFFD) and clipped to 60,000 bytes with a visible
  `[cut: kept K of N bytes]` note where it leaves the registry; each renderer
  repairs what it adds, and clips tool results, call arguments and replayed prose
  under the same 60,000-byte guard; and the checked serializer refuses, before
  the send, any body with a dropped string, a string over the guard, or invalid
  UTF-8, naming the tool the text came from. The system prompt and the user seed
  are never clipped silently - the caps that build them sit below the guard.
- **Tool choice per round**: OpenAI FORCES a tool on tool-allowed rounds
  (`tool_choice:"required"`), and so does Mistral chat/completions
  (`tool_choice:"any"`) on an unattended turn, so the model calls a registry tool
  or the terminal `orch_turn`. Their FINAL round (a cap tripped) pins `orch_turn`
  through the named-function object. On a person's turn Mistral asks with
  `tool_choice:"auto"` (RB-Q4b): under "any" mistral-medium wrote its answer into
  a tool call's name or made needless calls. Anthropic uses `tool_choice:auto` on
  EVERY round, the final round and the single shot included: Sonnet 5.5, Opus 5.5
  and Fable 5.1 answer a forced `tool_choice` with a 400.
- **A text-only round** (`HeadLoopConfig::proseIsReply`, set on a person's turn):
  on Anthropic (`end_turn`, `stop_sequence`) and Mistral (`finish_reason:"stop"`)
  a round that answers in text that is not only white space, with no call, is the
  reply on any round, so a turn costs its tool rounds plus one request. Z.ai has
  taken text as the answer on every round since 1 Nov. Otherwise (an unattended
  turn, the dream, an empty round) it is a stall: on Anthropic the final round
  after it gets the stalled answer back plus a user nudge, so the final turn keeps
  it. A prose answer is delivered as the reply only (no device action, memory write
  or spawn rides prose).
- **Output budget and completeness**: one table (`output_caps.h`) gives each
  adapter and round kind (tool round, final round, single shot, fold) its largest
  `max_tokens` and its reasoning effort: Anthropic 8,192 (12,000 on the retry) with
  `output_config.effort` pinned (medium; low for the fold) on the families that accept
  it; Mistral 6,000 (12,000) on Large 4, Medium, Small and Magistral, 4,096 (6,144) on
  older models, with the lowest `reasoning_effort` each takes (`"low"` on Large 4 and the
  hosted GLM, which refuse `"none"`; `"none"` on the others, which refuse `"low"`; a
  refused value is retried once without the field, HTTP 400 code 3051 or 422); Z.ai
  2,400 (3,000). OpenAI sends no output cap (reasoning would starve the answer), direct
  or through the router; the only OpenAI body that carries `max_output_tokens` is the
  router's capped resend after a low-balance 402. Every round and fold measures what it
  carries before it is sent (the body, the photo bytes spliced in, the prompt's tokens,
  routed or not, and whether the DMA pool was short at round start so the slices will be
  paced: `RoundLoad`) and sends the cap that fits that load inside the host's read window
  (`outputCapFor(..., RoundLoad)`: the window less the connect, the upload, the pacing and
  the prefill, at the model's slowest decode rate), replacing the table's `max_tokens` in
  place. The first attempt takes three quarters of that fit (all of it when three quarters
  is under the host's minimum final answer), never more than the table, and never under
  that minimum (3,000 tokens; 1,500 on Large 4 and GLM through Mistral, 1,600 on Z.ai);
  the retry after a cut is sent only when it gives at least 400 tokens more or a lower
  effort. The read windows are Mistral 60 s (the gateway closes a non-streamed
  request there; its rounds time out at 65 s), Z.ai 50 s, the others 120 s, and at most
  90 s through the router; `docs/reference/request-budgets.md` lists every model's caps.
  A text round over 85,000 B whose fit cannot carry the minimum final is not sent: it is
  refused with the body-budget wording at the size whose fit does, so the turn gives up
  what it can and builds it again. On the device, a round that starts with the DMA pool
  short (the pacing measure above), or any round after an upload of the turn failed for
  memory, has v5.1.1's 85,000 B body budget whatever its head's own: each TLS record of a
  body in PSRAM is encrypted through a DMA bounce allocation (up to 1,600 B), and a larger
  body died mid-upload on that allocation (bench A10). An upload that fails that way over
  85,000 B is rebuilt at 85,000 B and sent once more, with what was left out named in the
  request; a second failure fails the round. When the heap stayed low through the wait after
  that failure (the memory verdict), there is no resend at the floor: the turn ends with the
  memory reply, as in v5.1.1. A turn that sent a round at the floor defers the chat's
  automatic fold until a turn's rounds all go above the floor (lane RB-BGb, ruling 83 item
  9): a fold at the floor would keep the head's full tail (about 23,600 tokens, 59,040 B, on
  claude-sonnet-5-5), which the floor cuts again. Each floor request names what it leaves
  out (the LEFT OUT note, with the rows to read back), rows a window shift hid still fold
  (only they), a Summarize now is never deferred, and the fold after the deferral keeps the
  head's full keep, never v5.1.1's about 1,600 tokens (4,096 B). The window itself is never
  held to the floor. The fold's own request (summary, digest and prompt, under 60,000 B of input)
  fits the floor. The state JSON's `mem` block carries `rounds` and `floorRounds` (every
  round since boot, and those at the floor). The deferral logs `history(<chat>): a request
  went at the send floor - the automatic fold waits for one above it` when it starts, `a
  request went above the send floor - the automatic fold may run` when it ends, and each
  pass that finds the chat due `fold(<chat>): deferred - the last request went at the send
  floor, next look in 60 s`. Every adapter reads completeness - Anthropic
  `stop_reason:"max_tokens"`, Mistral `finish_reason:"length"` (or tool arguments that
  do not parse), OpenAI `status:"incomplete"`, the custom head `finish_reason:"length"`
  - and a cut answer is never run or delivered as complete. A cut round is retried once
  in the same step with a body that differs (more room, effort one step lower), never an
  identical one; still cut, it fails as a Cut, and the fabric takes over (below).
  Thinking is never disabled (a 400 on Opus 5.5 and Fable 5.1); effort is the lever.
- **A long answer in the round's text**: Mistral and Z.ai can write the answer as the
  round's text with only a pointer or a digest of it in `orch_turn`, and only `reply` is
  delivered. Text of at least 1,200 bytes and at least twice the reply replaces it; the
  turn's other fields stay.
- **Mistral built-ins**: chat/completions forbids tool_choice forcing alongside
  the Studio built-in connectors, so loop rounds drop the built-ins and the
  registry's `web.search` covers the gap. (The single-shot Conversations path
  keeps them.)
- **Per-round socket budget**: each round's exchange timeout is clamped to
  the turn's remaining wall-clock budget (8 s floor), so N slow rounds cannot
  stack past the 600 s turn deadline.
- **Upload timing**: every exchange's trace line ends with `wms=` (the body's upload,
  first slice to last byte) and `pms=` (the part of it spent pacing), and a body of
  100 KB or more writes one durable `transport: up <B> B in <ms> ms, paced <ms>` line
  (at most one every 30 s), the two terms a round's time fit predicts.
- **Thinking**: Anthropic non-tool text blocks and OpenAI reasoning summaries
  (requested via `reasoning:{summary:"auto"}`, gated off the `-chat` variants
  that reject the parameter) are observed into the glass box, never replayed into
  the conversation. Only OpenAI's encrypted reasoning items are replayed, and
  only because a reasoning model requires them under `store:false`.

## Context overflow

A provider rejecting a request for size is recognized by its wording
(`isContextOverflowError`, recorded from each API: Anthropic "prompt is too long: N tokens
> M maximum", OpenAI `context_length_exceeded` and "exceeds the context window", Mistral "Prompt
N > M maximum context length" with type `invalid_request_prompt_too_long`, Z.ai code 1261
"Prompt exceeds max length"). A refused `max_tokens` names a limit too and is not one. The
turn trims and retries once (below), and the chat is marked fold-due, so the next pump pass
compacts it ([memory.md](memory.md)) instead of letting it recur. Mistral answers a request far
over a small tier's limit with a 429, which is handled as a rate limit.

## Mid-turn provider failover

Because the transcript is device-owned, a provider that fails PART-WAY through a
tool loop no longer loses the turn. `runFabricLoop`
(`lib/harness/src/providers/loop_common.cpp`) drives the loop over an ordered
host list against one shared `Transcript`, wrapping each controller step so that
retry and failover happen INSIDE a single logical round:

1. Run the round against the current host.
2. A failed round is typed (`StepErr`, set by the adapter that saw the answer)
   and the fabric acts on the kind:

   | kind | same-host retry | switch host | park host |
   |---|---|---|---|
   | Transport (connect, short write, fast cut, cut body), ServerError (5xx, 529), RateLimited | once | then yes | no |
   | Timeout (sent, no answer within about the whole 120 s round) | no | yes | no |
   | Rejected (another 4xx: unknown model, a parameter the model refuses, a refusal), Cut (still cut after the in-step retry) | no | yes | no |
   | Auth (401/403), Billing (out of credit or quota) | no | yes | yes |
   | BadRequest (the body is unreadable, or refused before the send), LowMemory | no | no | never |
   | Unsent (the device's first write left 0 bytes: the request head, or the first body slice) | once, only after the largest free block recovers (bounded wait) | no | never |
   | Paused (every host the turn could use is held by the pause switch) | no request at all | no | never |

3. A switch walks to the next keyed host that is not parked and re-runs the SAME
   round on the shared transcript. Up to two switches; the host list is the
   current provider plus up to two keyed, in-budget alternates from
   `providerPriority`.

A **Cut** round gets a note in its seeded message: it ran out of room, what it wrote
first, and to answer in far fewer words. It then switches host, or, with nowhere to
go, runs once more on the same host. If that is cut too, the owner gets what it wrote
with the plain cut marker. Tools that ran before the cut are never run again. Once the round is
served the note is gone: later rounds are not told to be brief, and a later failure is told as
itself, not as the recovered cut. An overflow that a trim recovered is not told either.

A provider's **context-window refusal** (`isContextOverflowError`, each provider's
recorded wording; a refused `max_tokens` is not one) is trimmed and retried once on
the same host, then follows the Rejected row above with the trimmed request. It is
never a raw provider line to the owner.

**What a request gives up when it does not fit.** The same ladder answers a body over
the wire budget (`kWireBodyMax`, refused before the send and refitted until it fits)
and a window refusal (one pass, so it walks as far as the refusal's figures ask). In
order:

1. the arguments of calls that already ran, then older tool rounds folded to one line;
2. older history, oldest first (the summary and the newest turn stay);
3. the turn's photos as images, for a window refusal only (photos sit outside the byte
   budget): that host reads their transcriptions;
4. the newest tool results, clipped;
5. recalled memories, then the attached skills (a tool can read both again);
6. the `[FRESH RESULTS]` and `[MEMORY RESULTS]` blocks, clipped, both ends kept;
7. the newest past turns as whole pairs, then the summary. On a turn with nobody's message
   in it (a synthesis, a routine) 7 comes before 6, since the results are what it is for;
8. the middle of the turn input;
9. every registry tool (orch_turn alone rides, and the model is told to answer now);
10. the skills list and standing preferences.

Whatever went is said: the request's input opens with `[LEFT OUT TO FIT THIS REQUEST]`,
the log has a `turn fit:` line, and the owner hears it when it touches what they asked
about (results, the latest messages, part of their message, the tools). The chat's fold
is queued for the next idle moment when at least 4 KB of conversation went, except within two
turns of the last fold: a run of large payloads is no sign that the fold failed, so it neither
pauses folding nor alerts anyone. A failed synthesis turn sends the raw results, not a second
failure message as well; a key or credit notice due that turn still goes out, since nothing else
says what to do.

A tool loop on one host (the Cumulo router head, the hosted Virtual Nimbus, mid-turn failover
off) gets all of this inside its loop, with no host to move to: the ladder, the one trim for a
window refusal, the note and one more run for a cut. A head with no tool loop gets the same
ladder from the engine, on the prompt and the input.

Nothing re-dispatches on a switch: every tool the loop already ran is in the
transcript as a result, so the next provider's renderer just reads it. A
**parked** host is skipped at the start of later turns - no request, retry or
notice spent on it - until its key changes, a request there succeeds, or 30
minutes pass (then one probe request). A host closed by a refusal - out of credit,
or a refused key (401, or a 403 for one model or endpoint) - gets no request outside
that timed probe or a key change: with every host closed the turn ends with no
request (`parked: <host> is out of credit (HTTP <status>); no request sent`, typed
Billing, or `parked: <host> rejected the key (HTTP <status>); no request sent`, typed
Auth) and the owner reply below. A **paused** host is never asked.

**One park per provider, for every paid seam.** The park
(`lib/core/include/nimbus/orch/provider_park.h`) is shared by the chat head, the
fold, sub-agent spawns, embeddings, the photo caption step, speech to text, text
to speech, the download scan and image generation. A billing refusal or a refused
key (401) learned on ANY of them closes that provider for all of them (a 403 only
for the seam that met it, since it can name one model or endpoint); a rate limit,
a server error, a timeout or a network failure closes nothing. Every harness
request feeds it on its way back (`wire::exchange`), so a refused sub-agent
dispatch or poll parks the provider too. The record keeps the refusal, its HTTP
status, the seams it closes, when it parked and when it is checked again; it is
saved to a flash file (`/data/provpark.txt`, never NVS) with the re-check time on
the wall clock when it is set, so a restart neither sends one more doomed request
nor repeats the owner notice. The **pause switch** on the same record holds a
provider (or every provider) off every seam until the cause that set it lifts it -
the spend ceiling uses it; the pause itself is not saved by the park (its owner
re-applies it at boot). The health report's **Providers** row and `GET /api/orch`
(`parks`) list every closed provider, why, and when it is checked again.

**The Cumulo router is read by its reason, never its status.** Its refusal contract
(`lib/core/include/nimbus/orch/router_refusals.h`, a copy of the contract in
`tools/router_contract/`) names what each reason means for a device:

| Router answer | What the device does |
|---|---|
| `insufficient_credits`, `credits_or_subscription_required` (no money), `budget_exceeded` (this device's own limit), `account_suspended`, `missing_router_key`, `invalid_router_key` | The only answers that park: every seam, the owner told once, in words true of that reason ("Add credits" only for the first two) |
| `insufficient_credits_for_call` (the clamp: the wallet has credits, this call's worst case does not fit) | One resend with the output capped at `maxAffordableOutputTokens` when that is at least 256; otherwise this call fails with "this request is larger than the remaining credits allow". A short reply opens with a note saying why, at most once every 6 hours |
| `rate_limited`, `credits_held_in_flight`, `router_internal_error` | One resend of the same request after `retryAfterMs` when that is 6 seconds or less, else the engine's own reply |
| `fail_over` reasons (`upstream_*`, `provider_*`, `fallback_exhausted`, `funding_cap_reached`, `router_disabled`, `ceiling_reached`) | That upstream cools for `retryAfterMs`; the turn answers on the router default, or fails over to the next provider. The wallet is never blamed |
| Any other reason, or a 402 or 403 with none | This call fails. Never a park. A 401 with no reason still reads as the key: the router's own 401 always names one, so what is left is a body that did not survive |

A resend that recovers a refusal ends it: a cut reply or a server error on the capped
resend reads as it would with no clamp, and a refusal of the resend is what the owner hears
about. Only a resend that was capped opens the reply with the note.

A clamp, a held wallet or an upstream's failure also shows the account stands, so any
of them ends a billing or key park outright. The exception is a park for this device's
own limit (`budget_exceeded`): the router reads that budget after the wallet, so only an
upstream's failure says it was passed.

**A re-check is a real request.** A reader (a failover candidate list, the sub-agent
catalog, a status surface) asks `peek`, which never takes the park's one re-check; the
request that leaves claims it (`wire::exchange`, or `available` at a device seam). A seam
that got it and then sends nothing hands it back. A parked provider nothing else calls,
such as Cumulo last in the priority list behind a head that answers every turn, is asked
by the idle re-check (`TurnEngine::recheckParkedHost`, on the fold's pump) with one short
request: a refusal is free and an answer ends the park, so a provider recovers by itself.
It waits for two quiet minutes after the last turn, never runs while a turn does, under
low memory, or past the spend limit or the provider's budget. A provider that hangs holds
the pump until the head's timeout, the same as a fold.
Every re-check logs one outcome: `park: <provider> re-check answered - open again`,
`refused again (...)`, `failed without a verdict`, `got no answer in time`, or `handed
back`. A hosted instance keeps asking its head on every turn, so a client who tops up is
answered on the next message.

**Out of credit or a refused key: told once, answered once, then quiet.** A turn
that fails for billing or on a refused key - the provider refused, or the park ended
it with no request - follows one rule per episode, on every path
(`orch::refusalTurnAnswer`): the first time the episode reaches the owner they get
the notice ("That didn't finish - Anthropic says the account is out of credit. Add
credit on its billing page, or switch providers. Your message was not answered.", or
"That didn't finish - Anthropic rejected its key. Update it under Assistant >
Models. ..."); the owner's next refused message gets one short answer ("Your message
was not answered. Anthropic is checked again in 27 minutes, and replies resume when
it accepts requests.", or "Your message was not answered. Anthropic rejected its
key: update it under Assistant > Models."); every later refused turn of the episode
gets nothing. A routine or wakeup may carry the notice, never the short answer. Both
latches live in the park record (the notice latch is shared with the embedding,
caption and other seams, and a switch notice for either refusal takes it too) and
survive a restart; an answered probe or a new key ends the episode. A quiet
background turn holds its words and takes neither latch, so the next attended turn
tells the owner.

The owner hears **one message per failed turn**: a switch notice raised while the
turn runs is held, and a turn that fails folds it into its final notice (a billing
refusal's next step becomes "Add credit on <provider>'s billing page."), while a
turn that is served sends it just before its reply. The final notice names the
first cause (stepErrCause), offers asking again only for a cause in transit, says
"Your message was not answered." when every host failed before anything ran, and
lists what the turn had already done when tools ran ("Already done this turn:
memory_write, web_search (2)."). A request the device could not send at all reads
"the device could not finish sending its request (memory is tight)", never a
provider's fault. The billing notice is told once per episode across every seam.
Turn-end bookkeeping attributes to the host that finished. Every request's deadline comes from one policy: 120 s for
every head round and every synchronous sub (Z.ai keeps its 50 s, under its
gateway cut), clamped to the turn's remaining budget.

Gated by NVS `midFail` (`store::midTurnFailover()`, default **ON**; web toggle
"Switch providers mid-task if one fails"). The engine
(`src/agent/orchestrator.cpp`) registers the fabric loop as `ProviderHosts.fabric`
and routes a loop turn to it whenever the gate is on and tools are present.
Otherwise the turn runs the single-host loop and relies on the between-turn
ladder below.

## Between-turn retry, failover, and the side-effect rule

This ladder governs single-shot turns and tool-loop turns when mid-turn failover
is off. (With `midFail` on, a loop turn fails over INSIDE the fabric loop above
and never re-enters this ladder - the ladder exists only because the older
per-host loops could not carry a turn across providers.)

1. A failed turn retries ONCE on the same host with a fresh conversation
   (transient TLS/handshake flakes), then walks up to two keyed alternates from
   `providerPriority`, notifying the owner at each switch with the cause. The
   same typed rule decides the retry: a timeout, an unknown model, a refused key,
   a billing refusal or an unreadable request is not retried on the same host
   (the same request fails the same way again).
2. **Both steps are skipped entirely if ANY tool executed during the failed
   attempt** - replaying a turn re-runs its side effects (a memory write, a
   spawn, a message). The owner gets an honest error instead.
3. A provider over its monthly token budget is skipped at turn entry (the
   budget gate picks the first keyed, under-budget alternate).
4. The park above holds here too (the same per-provider park): a billing or auth
   refusal parks the host, a parked head is skipped at turn entry (the turn
   starts on the first alternate that is not parked, and the next turn's
   `[FALLBACK]` note names the head it started past), a parked alternate is
   never a failover target, and the billing notice is told once per episode on
   every path. With no open alternate a head closed by a refusal (out of credit, a
   refused key) or paused gets no request and the turn ends with the park's cause. The context fold skips parked hosts the same way, and its
   answers and failures feed the park.
   Failover notices are held and folded into the one final notice like the
   fabric's.
5. Provider switches always start a fresh provider-side thread; continuity
   comes from the device's own layers (recent-conversation window, per-chat
   running memory, vector recall) - see
   [turn-anatomy.md](turn-anatomy.md) §3.

## Graceful degradation ladder

From least to most degraded, all fail open and none restart the device:

- A tool dispatch fails → the model sees the error text as the tool result and
  is expected to recover (a tool error is never fatal to the loop).
- A cap trips (rounds/deadline/heap/bytes/stall) → one forced tool-less
  "answer now" round; if the model still doesn't terminate, a clean error
  reply.
- The response parses but violates the contract → per-item drops (lenient
  parser) + validator refusals with reason strings the model sees next turn.
- The whole turn fails → retry/failover ladder above.
- Heap below the turn floor → the turn is deferred with a "one moment" reply
  before any provider call is made.

## Sub-agent (managed-agent) wire

Documented separately in [sub-sessions.md](sub-sessions.md) - Anthropic managed
agents (environments/agents/sessions + an events poll), OpenAI background
Responses, Mistral Conversations (synchronous under an async skin), and the
custom LAN endpoint.

## Z.ai (GLM)

Z.ai's GLM models speak the OpenAI chat-completions dialect, but under the base
path `/api/paas/v4` (not `/v1`). The same token is served by two hosts depending
on region: `api.z.ai` and `open.bigmodel.cn`. On the first verify the device
**probes both** (a `GET /api/paas/v4/models` with the token) and pins whichever
answers, so you never pick the wrong region by hand. The model list is harvested
into the catalog (GET /api/models) with GLM ids classified into roles; a GLM
sub-agent runs one synchronous `POST /api/paas/v4/chat/completions` and its reply
is read from `choices[0].message.content`. Paste the key into the Z.ai row's
**API key** field under Assistant > Models > Providers & keys and select
**Save & Verify**; leave the endpoint to the probe.

GLM always reasons before it answers (Z.ai refuses `thinking: disabled`), and
Z.ai's gateway closes a request that has not answered after about 60 s. So every
Z.ai request the device makes (the head, direct or through the Cumulo router's
`zai` upstream, and every Z.ai sub-agent) carries `reasoning_effort: "low"`; a
Virtual Nimbus's Z.ai heads (direct and through the router) do the same through
the same code.
A head request gets 50 s and 2,400 output tokens (3,000 on the retry after a cut),
about 1,800 words, because a request that runs past 60 s gets no answer at all; if
Z.ai has not answered by then, the turn fails over to the next provider straight
away (no same-host retry) and the notice says Z.ai did not answer in time.

A GLM head runs the same tool loop as the other providers, and Z.ai is a host of
the mid-turn failover. A model that refuses `reasoning_effort` gets one retry without
it, remembered for this boot, on the loop and the single shot alike. An answer it sends
as text instead of an `orch_turn` call is delivered as the reply. The compaction fold is a plain completion with no response
format: its reply is the summary.

## Cumulo Nimbus router

Cumulo Nimbus is a router: one key, and the upstream provider is chosen per role.
The device speaks the OpenAI chat-completions dialect to
`/router/<upstream>/v1/...` on the router host (default `app.cumulo-nimbus.ai`,
overridable), and the router forwards to the chosen upstream (Anthropic, OpenAI,
Mistral, Z.ai) and normalizes the reply. The key verifies against
`/router/<upstream>/v1/models`. A model is selected as `<upstream>/<model>` (for
example `anthropic/claude-sonnet-5`); the orchestrator head and its sub-agents
resolve the selector the same way, splitting the upstream off, routing to
`/router/<upstream>/v1/chat/completions`, and sending only the bare model id (the
id the router prices). This
lets one key drive the orchestrator on one upstream and sub-agents on another,
with embeddings/vision/STT/TTS available where the chosen upstream supports them.
A reasoning model may answer with a list of content chunks instead of a string
(Mistral's `mistral-large-4-0` sends a `thinking` chunk, then `text` chunks; an
Anthropic reply can lead with a `thinking` block): every reader on the device and a
hosted instance takes only the `text` chunks, in order, as the answer.

A head round through the router gives up at 90 s (the router's egress gives up at
95 s), or at its host's own ceiling when that is shorter (Mistral 65 s, Z.ai 50 s), and
every routed request tells the router when: `x-cumulo-deadline-ms` is its socket
deadline less 5 s, so the router starts no retry or fallback it cannot finish in time.
Routers that do not know the header ignore it.

Get the key from your Cumulo Nimbus account (see
[Use your Cumulo key](cloud/cumulo-key.md)); paste it into the Cumulo Nimbus row
under Assistant > Models > Providers & keys.

A verified Cumulo key is the default source for the whole assistant: when no other
provider key is set, the orchestrator and its sub-agents run on the Cumulo key with
nothing else to configure (one key, one balance). If you also set your own OpenAI,
Anthropic, or Mistral key, that provider runs on your own key instead; the Cumulo
key stays the source for everything you have not brought your own key for. The head
model defaults to a proven router model until you pick one under Assistant > Models.

## Model catalog, capabilities, and fallbacks

The device builds a live, capability-aware model list per provider from each key's
own `/v1/models` (Anthropic: 100 models a page, following `has_more`) and classifies
every model into roles (orchestrator, sub-agent, embedding, vision, STT, TTS, image),
a size class (S/M/L), and capability flags. It reads capability fields where the API
supplies them (Anthropic `max_input_tokens`/`max_tokens`/`capabilities`, Mistral
`capabilities`) and id-family heuristics otherwise, and keeps what the provider says
about each model: its display name, line (Anthropic), release date, lifecycle and an
announced shutdown date. A model past its shutdown date is left out once the clock
knows the date; one with a future date is kept, marked deprecated and left off the
short list. Model ids are untrusted text: only ids of 1-64 characters from
`[A-Za-z0-9._:/-]` are kept, and control characters are stripped from names.

The Cumulo Nimbus head is special: it reads the router's own `GET /router/models`,
which lists exactly the models the router can route (priced and enabled), as
`<upstream>/<model>` ids with a size class and the billed rates as metadata, so the
Cumulo model list tracks the router's catalog rather than one upstream's
`/v1/models`. The list is about 95 KB; every list is read as a stream, one model at a
time, so it is read whole in a few kilobytes of memory, and a list cut short (a
dropped connection, the 20 s read deadline) is marked incomplete and never replaces
a stored one. Every fetch writes one log line with what it read and kept. The router
keeps one row per model: a dated snapshot whose undated alias is listed is left out,
the dot and dash spellings of one version (`claude-opus-5.5`, `claude-opus-5-5`) are
one model kept under the dash spelling, and the router's `:batch` price rows and ids
whose model part still holds a `/` after the upstream (such as
`mistral/mistral/zai-glm-5-3`, which no upstream runs) are not models the device can
call. The router default comes first, then one model per upstream, then every
upstream's current generation, then the rest. The same model name under two
upstreams is two models; the four upstreams the device knows (OpenAI, Anthropic,
Mistral, Z.ai) are always listed, and at most 8 others. A Virtual Nimbus applies the
same rule to the same list.

**The model store.** Each provider's list lives in one flash file,
`/data/models_<provider>.txt`: a header line with the fetch time (`fetchedAt`, 0 when
the clock was not set yet), the firmware version, how many models the provider
listed, how many did not fit and the short list, then one line per model (about 85 B)
with its price when the router gave one. It holds **up to 160 models and 20 KB per
provider**; the router's whole list fits today, and a list that does not says
"<n> more" in the picker. The short list (the picker's Recommended group and the
head's `[AVAILABLE MODELS]` line) holds up to 8 models for a direct provider and up to
12 (480 B) for the router, which always includes the router default. Nothing about
the model list is kept in NVS. The same verify keeps the billed rates
(`in_micro_per_mtok` / `out_micro_per_mtok`) of the models the device uses - its
Cumulo short list plus the head and sub-agent models, at most 24 - so the Usage page
prices Cumulo spend at what Cumulo bills (shown as "Cumulo price"; a price you enter
for a model still wins). A device with a verified Cumulo key and no stored router
prices (for example right after an update) runs one Cumulo verify shortly after
boot, on the same verify task, when no turn is running.

**One check.** Whether a model may be used is one rule, asked by the head's model
switch, by every sub-agent spawn and by the web picker's save: a model the store
lists that the key can run (not marked unusable by the probe, not a non-chat family,
not past its shutdown date), or an id the provider serves under another name (OpenAI's
`gpt-5.6`, answered as `gpt-5.6-sol`), or an alias a live model lists (Mistral's
`mistral-large-4-0`, answered as `mistral-large-4`), else, before the first fetch, the
built-in starting list; the provider's default models can always be picked again, and
a model you already chose is never refused: if a later list no longer names it, as an
id or an alias, it keeps running and you are told once, with the provider's current
default named as the model to switch to if it ever stops answering. The settings show
a renamed pick under its current name, and a pick whose own row exists is shown under
that row, never under a sibling row that merely lists it as an alias. So every model
the picker or the head's model line offers is accepted, and a model pick the device
cannot run is refused with the models it accepts. Each model in `GET /api/models`
carries its price and where it comes from (`priceSrc`: the router's price, the
built-in estimate, or `unknown`, shown as "price unknown"). A one-shot cheap
usability probe per selected model means a model your key cannot actually use never
appears. The list is served at `GET /api/models` (add `?all=1` to include
probe-hidden models) with `fetchedAt`, `stale` and `refreshPending`, and every verify
refreshes it: a manual **Verify**, or, with **Capability validation** set to Active,
the scheduled re-check (default every 24 hours; OpenAI, Anthropic and Mistral keys
only). A list refresh fetches the model list alone (no model probe, so it costs no
tokens): opening a model dropdown in the web app (`POST /api/models/refresh`; the
dropdown then shows "Updated" with the time), the assistant asking through
`models.list`, a model switch that names an id the store does not hold, and a passive
refresh weekly or after a firmware update (the version in the store's header differs
from the running one). They share one queue: a provider is refreshed at most once an
hour, only between turns, one provider at a time. The generated
[capability matrix](reference/capabilities-matrix.md) shows which role and feature
each provider offers.

Provider failover is a rule engine shared with the cloud (one admin-editable rule
set): predicates match provider, model (with a trailing-`*` glob), size class,
capability, and error class; the ordered targets are tried in turn, skipping the
one that just failed. It ships with size-class defaults that reproduce the classic
priority walk, applies to every turn (the mid-turn switch and the between-turn
ladder both consult it), never falls embeddings back cross-provider, and records
each switch as a context note the assistant can mention if relevant. The active set
is served at `GET /api/fallbacks`. See [harness.md](harness.md) for the turn flow.

When a turn is answered by a fallback provider or model, that substitution is
disclosed honestly rather than hidden: the reply carries a small "served by
&lt;provider&gt; &lt;model&gt;" note so you always know which model actually
answered. A working direct call shows nothing.

## Local model as the custom endpoint (Ollama and friends)

The device can run against any OpenAI-compatible server on your own network,
with no cloud key and no internet. Example with Ollama:

1. On the server machine: install [Ollama](https://ollama.com), pull a small
   model (`ollama pull qwen2.5:3b`), and bind it to your LAN by running
   `ollama serve` with `OLLAMA_HOST=0.0.0.0:11434`. On macOS,
   `brew services restart ollama` regenerates its launchd plist and drops that
   environment variable; use your own LaunchAgent (or Ollama.app's settings) if
   you want the binding to persist.
2. On the device (Assistant > Models > Custom endpoint, or
   `POST /api/orch`): Base URL `http://<server-ip>:11434/v1`, wire
   `openai wire`, Model ID `qwen2.5:3b`. Helpers can use it right away; to
   run the head on it, restart the device (the custom head registers at start).
   Any URL path (the `/v1`) is ignored; only the
   host and port matter. `<server-ip>` is a LAN address: a plain DHCP lease
   moves when the server machine reconnects or changes networks, and the device
   then cannot reach it (you see a stuck or failed sub-agent, not an error on
   the screen). Give the server a reserved or static LAN IP, or point the
   Base URL at a `.local` mDNS name the server advertises, so the endpoint
   survives a reconnect.
3. Route work to it: put `custom` in the sub-agent priority to send sub-agents
   there, or set it as the head to run the whole assistant locally.

Notes, all deliberate:
- Keys are never sent over `http://` - a plain-HTTP LAN endpoint runs keyless.
  Everything the device sends (including its memory recalls) crosses your LAN
  in cleartext, so this is for trusted home networks; use `https://` + a key
  for anything else.
- The head speaks single-shot chat-completions with the strict orch_turn
  JSON schema (schema-less retry on 400); there is no mid-turn tool loop on
  the custom backend. A head request may take up to 120 s (the same ceiling
  as the OpenAI and Anthropic heads; the Z.ai head shares this path with its
  own 50 s budget, see Z.ai above); a sub-agent request times out at 30 s.
- Firmware before v4.1.6 needs "Switch providers mid-task if one fails"
  (`midFail`) turned off while the head runs on the custom endpoint; those
  builds routed loop turns into the cloud-only fabric and failed before sending
  anything.
- A small model is honest but limited: expect terse replies and the occasional
  odd spawn. It is a great sub-agent workhorse and a private fallback head.
- Spend: a keyless endpoint on your own network (a private or loopback address, or
  a `.local` name) counts as free toward the monthly spend ceiling. Any other host,
  or any endpoint with a key, is treated as a paid service and counted at the
  highest built-in price until you set the model's price under Usage > Prices (see
  [Monthly spend ceiling](spend-ceiling.md#a-custom-endpoint)).
