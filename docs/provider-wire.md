# Provider wire - structured outputs, tool loops, and failure handling per provider

The technical companion to [turn-anatomy.md](turn-anatomy.md) and
[harness.md](harness.md): exactly how the turn contract is enforced on each
provider's API, how the tool loop rides each wire, and what happens when a
provider misbehaves. Adapters live in `lib/harness/src/providers/`; the shared
schema in `lib/core/include/nimbus/orch/orch_schema.h`.

## The contract has one source

Every field of the `orch_turn` response (reply, ask, memory, device[],
mem_write[], mem_query[], session_ops[]) is defined ONCE, as `ORCH_D_*`
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
| **OpenAI** (Responses API) | `text.format` `json_schema`, `strict:true` | `orch_turn` function tool, `strict:true` parameters | **Yes**, both channels - the model cannot emit an out-of-schema `orch_turn` |
| **Anthropic** (Messages API) | forced tool-use, `tool_choice:{type:"tool",name:"orch_turn"}` | `orch_turn` offered alongside the registry tools (`tool_choice:auto`) | **No** - the strict-grammar budget rejects a schema this size, so descriptions are stripped and the schema is advisory; the device parser + validator are the enforcement |
| **Mistral** | Conversations API, `response_format.json_schema`, `strict:true` | chat/completions, `orch_turn` function tool (no `strict` flag) | single-shot **yes**; loop **advisory** - the device parser + validator enforce it |

**"Strict at the wire, lenient at the parser."** Regardless of provider,
`parseTurn` (`lib/core/src/orch_turn.cpp`) is tolerant - unknown fields are
ignored, malformed items are dropped individually, string caps are applied on
copy. And the portable device-action validator
(`orch_device_actions.cpp`) re-checks EVERY device action - including the
protected-key refusal (API keys, provider routing) - no matter what the wire
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

Every tool-loop turn is recorded into ONE device-owned `Transcript`
(`lib/core/src/orch_transcript.cpp`, host-tested): the seeded user input, then
per round the model's prose, its tool calls, and their results. The portable
controller (`runHeadLoop`, `lib/core/src/orch_head_loop.cpp`) owns rounds/
deadline/heap/byte caps and records into the transcript through
`hooks.transcript`; each provider adapter supplies a thin `step` closure that
RE-RENDERS its wire shape from that transcript every round. The loops are
therefore **stateless** - no provider-side conversation is retained between
rounds, and turn continuity lives on the device, not on a provider's server.

This is the invariant that makes a mid-turn provider switch possible (below):
executed tool results ride the transcript as DATA, so re-rendering a round
against a different provider replays nothing - the tools already ran.

Each renderer speaks its provider's dialect from the same record:

- **Anthropic** (`renderAntMessages`, `anthropic.cpp`) rebuilds `messages[]`:
  the pinned user seed, then for each tool-carrying round an assistant message
  of `tool_use` blocks answered by a user message of `tool_result` blocks. The
  **prefill rule**: an assistant message is emitted ONLY for rounds that carried
  a tool call - a prose-only (stalled) round is never echoed, because a trailing
  assistant message plus forced `tool_choice` is a hard 400. Messages is the one
  wire whose body grows round over round, so it also carries an in-turn gradient
  fold (older rounds collapse to one line each past a byte trigger; the newest
  round stays verbatim, and folding never breaks the `tool_use`↔`tool_result`
  pairing an unanswered `tool_use` would 400 on).
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
  serialized into one PSRAM buffer and written once.
- **Tool choice per round**: OpenAI and Mistral FORCE a tool on tool-allowed
  rounds - OpenAI `tool_choice:"required"`, Mistral chat/completions
  `tool_choice:"any"` - so the model always calls a registry tool or the
  terminal `orch_turn`, and a prose-only stall round cannot occur. Anthropic uses
  `tool_choice:auto` (the model may call a tool OR answer via `orch_turn`); a
  genuine text-only stall there is caught by the controller's forced final round.
  The forced FINAL round (a cap tripped) pins `orch_turn` directly on every
  provider - OpenAI/Mistral via the named-function object, Anthropic via
  `tool_choice:{type:"tool",name:"orch_turn"}`.
- **Mistral built-ins**: chat/completions forbids tool_choice forcing alongside
  the Studio built-in connectors, so loop rounds drop the built-ins and the
  registry's `web.search` covers the gap. (The single-shot Conversations path
  keeps them.)
- **Per-round socket budget** (F25): each round's exchange timeout is clamped to
  the turn's REMAINING wall-clock budget (8 s floor), so N slow rounds cannot
  stack past the 600 s turn deadline.
- **Thinking**: Anthropic non-tool text blocks and OpenAI reasoning SUMMARIES
  (requested via `reasoning:{summary:"auto"}`, gated off the `-chat` variants
  that reject the parameter) are observed into the glass box, never replayed into
  the conversation. Only OpenAI's ENCRYPTED reasoning items are replayed, and
  only because a reasoning model requires them under `store:false`.

## Context overflow (reactive)

A provider rejecting a request for size is recognized by wording
(`isContextOverflowError`: OpenAI `context_length_exceeded` / "maximum context
length", Anthropic "prompt is too long", Mistral "exceeds the maximum", plus a
generic "context window" guard). The turn itself recovers through the normal
zero-tools fresh-thread retry below; additionally the chat is marked fold-due,
so the next pump pass compacts it ([memory.md](memory.md)) instead of
letting the failure recur.

## Mid-turn provider failover

Because the transcript is device-owned, a provider that fails PART-WAY through a
tool loop no longer loses the turn. `runFabricLoop`
(`lib/harness/src/providers/loop_common.cpp`) drives the loop over an ordered
host list against one shared `Transcript`, wrapping each controller step so that
retry and failover happen INSIDE a single logical round:

1. Run the round against the current host.
2. On a transport failure, retry the SAME host once.
3. Still failing → walk to the next keyed host and re-run the SAME round on the
   shared transcript. Up to two switches; the host list is the current provider
   plus up to two keyed, in-budget alternates from `providerPriority`.

Nothing re-dispatches on a switch: every tool the loop already ran is in the
transcript as a result, so the next provider's renderer just reads it. A
**schema-parse failure does NOT fail over** - that is a device-side contract
error, identical on every host, so it returns immediately rather than burning a
switch. The owner is notified on each switch ("… hit trouble mid-task -
switching to …; your progress this turn carries over"), and turn-end bookkeeping
attributes to the host that finished.

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
   `providerPriority`, notifying the owner at each switch.
2. **Both steps are skipped entirely if ANY tool executed during the failed
   attempt** - replaying a turn re-runs its side effects (a memory write, a
   spawn, a message). The owner gets an honest error instead.
3. A provider over its monthly token budget is skipped at turn entry (the
   budget gate picks the first keyed, under-budget alternate).
4. Provider switches always start a fresh provider-side thread; continuity
   comes from the device's own layers (recent-conversation window, per-chat
   running memory, vector recall) - see
   [turn-anatomy.md](turn-anatomy.md) §3.

## Graceful degradation ladder

From least to most degraded, all fail-open and none reboot:

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
custom LAN backend.

## Local model as the custom endpoint (Ollama and friends)

The device can run against any OpenAI-compatible server on your own network -
no cloud key, no internet. Verified setup (2026-08-12, Ollama on a Mac):

1. On the server machine: install [Ollama](https://ollama.com), pull a small
   model (`ollama pull qwen2.5:3b`), and bind it to your LAN by running
   `ollama serve` with `OLLAMA_HOST=0.0.0.0:11434`. ⚠ On macOS,
   `brew services restart ollama` regenerates its launchd plist and silently
   drops the env - use your own LaunchAgent (or Ollama.app's settings) if you
   want the binding to persist.
2. On the device (Capabilities → Models → Custom endpoint, or
   `POST /api/orch`): Base URL `http://<server-ip>:11434/v1`, conversation
   style `openai`, Model ID `qwen2.5:3b`. Restart the device - the custom
   backend registers at boot. Any URL path (the `/v1`) is ignored; only the
   host and port matter. ⚠ `<server-ip>` is a LAN address: a plain DHCP lease
   moves when the server machine reconnects or changes networks, and the device
   then silently can't reach it (a stuck/failed sub-agent, not an error you'll
   see on the panel). Give the server a reserved/static LAN IP - or point the
   Base URL at a `.local` mDNS name the server advertises - so the endpoint
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
  the custom backend, and requests time out at 30 s.
- Firmware before v4.1.6 needs "Mid-task provider switch" (midFail) turned OFF
  while the HEAD runs on custom - older builds routed loop turns into the
  cloud-only fabric and failed before sending anything.
- A small model is honest but limited: expect terse replies and the occasional
  odd spawn. It is a great sub-agent workhorse and a private fallback head.
