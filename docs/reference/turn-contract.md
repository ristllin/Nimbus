<!-- audience: dev -->
# Orchestrator Turn Contract

The **turn contract** is the JSON shape every provider-hosted model must return
for one Orchestrator turn. Use this page when you write or debug a provider
adapter, a prompt, or a test that feeds `parseTurn()`. The portable, Arduino-free
`parseTurn()` parses and validates a provider's raw response string into a typed
`Turn`, so the device turn loop never reads a raw `JsonDocument`.

- Authoritative doc-comment: `lib/core/include/nimbus/orch/turn.h`
- Wire schema advertised to providers: `lib/core/include/nimbus/orch/orch_schema.h` (`ORCH_SCHEMA_BODY`)
- Parser: `lib/core/src/orch_turn.cpp` (`parseTurn`, `normalizeTurnDoc`, `salvageTurnText`)
- Leaked-markup repair and the reply cleaner: `lib/core/src/orch_reply_markup.cpp`
- Behavior pinned by tests: `test/test_orch_turn`, `test/test_turn_shape`, `test/test_reply_markup`
- Byte/concurrency caps: `lib/core/include/nimbus/orch/caps.h`, `lib/core/include/nimbus/mem_cap.h`

Related: the tools these fields drive - [tool-catalog.md](tool-catalog.md) -
and how a turn flows through the live turn loop -
[../architecture/orchestrator-live-turn.md](../architecture/orchestrator-live-turn.md).

## The two-layer design: strict at the wire, defensive at the parser

`ORCH_SCHEMA_BODY` marks every field it advertises `required` at the API layer
(each provider wraps this bare schema in its own envelope: Mistral
`completion_args.response_format.json_schema.schema`, OpenAI `text.format.schema`,
Anthropic `tool_use`). But `parseTurn()` is defensive: on Anthropic the wire
schema is advisory (its strict-grammar budget rejects a contract this size), so
the parser is the real enforcement layer, and it never throws away a good reply
over one slightly-off item. Keep the schema string and the parser in lockstep.

The split that matters at the parser is between `reply` and everything else:

- **`reply` is the one required field** and it is **strict**: absent (or
  `null`) ⇒ `MissingField`, present-but-not-a-string ⇒ `WrongType`. Without it
  there is no usable turn.
- **`memory` and `ask` are optional strings**: absent, `null`, or a non-string
  value reads as `""`, which already means "memory unchanged" / "no question".
  A model that leaves one out (Anthropic's schema is advisory, so this
  happens in the field) keeps the turn and its side effects: scratchpad,
  `mem_write`, device actions, and `session_ops` still apply.
- **Every array** (`device`, `spawn`, `await`, `mem_write`, `mem_query`,
  `session_ops`) and the `scratchpad` object is **repair-or-report**: a value
  in another shape is repaired when it can be (see "The parse-boundary
  normalizer" below), and what cannot be repaired is named in
  `Turn::parseNotes`, never read as empty in silence. A malformed *item* is
  still never a whole-turn error, and every spawn intent the parser cannot
  carry lands in `Turn::spawnDrops` with its reason.

The wire schema still lists `memory` and `ask` under `required`: OpenAI strict
mode demands every property be listed there. Only the parser is lenient.

## The parse-boundary normalizer

`normalizeTurnDoc()` runs inside `parseTurn()` before the field reads. It exists
because a non-strict wire (Anthropic, the Mistral loop) sends fields in shapes
the old parser read as "nothing there": on 16-18 Oct six turns promised 34
sub-agents and the parser read zero spawns, with no trace.

1. **Leaked tool markup** (`repairLeakedParams`). A model that slips from JSON
   tool input into its XML tool-call format ends `reply` with `</reply>` or
   `</parameter>` and writes the later fields as `<parameter name="memory">`
   blocks inside that string. When the tail holds nothing but such markers,
   whitespace and parameter blocks, it is cut, and each field it carried is
   recovered where the document had none (a present value always wins; a
   container value that does not parse to its type is dropped and counted).
   Prose that mentions a tag and goes on, and markers inside code, are left
   alone. The fields recovered this way are flagged (`Turn::repair`) so the
   engine can refuse their actions on an unattended or tainted turn; a spawn
   refused this way is reported to the owner and the head like any other.
2. **Container shapes.** For every container field (the list comes from the
   same table a test pins to `ORCH_SCHEMA_BODY`): a JSON string that parses to
   the expected type is used; one object where a list of objects belongs is
   wrapped; one search string becomes a list of one. Anything else is reported.
3. **`session_ops` items.** A list hidden in an item's `op`, an item sent as a
   JSON string, a missing `op` on an item with a task, an op synonym (`start`,
   `launch`, `run`, ...) and a brief under another name (`prompt`,
   `instructions`, `brief`, ...) are all read as spawns.

A well-formed turn is untouched and byte-identical (a counter-test pins it).

## Reasoning, wrappers and markup in the reply

`parseHeadReply()` drops a leading `<think>` / `<thinking>` / `<reasoning>`
block and a whole-reply wrapper (`<orch_turn>`, `<reply>`, ...) before it reads
the shape, so `<think>...</think>{json}` is a turn, never prose with raw JSON in
it. The engine then runs `cleanReplyMarkup()` on the reply and the ask, the
salvaged text and the fold summary, and Telegram runs it again before sending:
reasoning, tool-call markup and wrapper tags never reach a channel, while
unknown `<word>` content and code stay byte-exact.

## Cut and unreadable answers

When the head's answer cannot be applied, `salvageTurnText()` reads only the
top-level `reply` and `ask` of its leading object (bare or fenced, possibly cut
short; escapes and surrogate pairs decoded). Nothing nested is ever salvaged.
The owner gets that text, plus one fixed line exactly when the answer was cut:
"(My answer was cut off before I finished, and nothing else in it was carried
out. Ask me to continue.)". With nothing to salvage, one plain line says the
answer was cut off or could not be read and that nothing in it was carried
out. Nothing in such an answer runs, so salvaged text that says sub-agents
started also gets the spawn correction line, cut or not, and the turn logs
`spawn ledger spawned=0 claimed=1` and leaves the head a note. The delivered text
is captured to the chat history, and the turn-end row carries `err` `cut: ...`
or `parse: ...`. A failed turn takes the cut path when
its error is a cut by the typed step rule (`StepErr::Cut`: still cut after the
one retry with more room, or an answer too large for the device), or an untyped
line that names the output limit; a request refused for its `max_tokens` value
is not a cut.

## The final check

On a tool-loop turn, a terminal `orch_turn` passes `validateFinal` before it is
accepted (`HeadLoopConfig::validateFinal`, built with `chainFinalChecks`). An
answer whose spawn list cannot be read, whose spawn items cannot start, or whose
reply says sub-agents started while no spawn item is present is sent back once,
with the problems named and the array form shown. The claim check joins the same
chain: an answer whose reply claims an effect that neither this turn's tools nor
its own items back (a file written, a memory saved, a pin, a check, a figure from
a source) is sent back with the claims and the turn's record; sub-agent claims are
left to the shape check. At most one correction round runs per turn, shared by both
checks, never on the forced final round, never for a cut answer and never for a
prose answer (which carries no spawn).

## Contract shape

```json
{
  "reply":   "string (required)",
  "memory":  "string (optional; absent = unchanged)",
  "ask":     "string (optional; absent = no question)",

  "device":  [ /* discriminated-union objects, validated + executed device-side */ ],
  "mem_write":   [ { "content","importance","permanent","ttl" } ],
  "mem_query":   [ "search string", ... ],
  "session_ops": [ { "op","id","task","provider","model","skill","name","project","attach" } ],
  "scratchpad":  { "active","short":[...],"mid":[...],"long":[...] } /* nullable object; replace-per-tier; carries across turns, saved to NVS */,

  "spawn":   [ { "provider","model","skill","task","category","note","name" } ],
  "await":   [ "job-tag string", ... ]
}
```

Every key the wire schema lists is `required` there (strict json_schema has no
optional keys). The keys a turn may leave unset are typed **nullable**, so an
omitted value rides as `null`: `mem_write` `importance`/`permanent`/`ttl`, every
`session_ops` key except `op`, the `scratchpad` object and its four keys, the `led`
action's `brightness`, and every `config` knob. The rest are not nullable:
`mem_write.content`, `session_ops.op`, and each device action's `type` and core
fields (`led` `mode`/`r`/`g`/`b`, `lights` `value`, `tts` `text`, `orch_model`
`provider`/`model`). The parser defaults every missing nullable value. `spawn[]`
and `await[]` are **retired from the wire** (`session_ops` is the one spawn
surface), but the parser still accepts them so an old client keeps working.

## String fields

`reply` is the one required string: missing (or `null`) rejects with
`MissingField`, mistyped (for example an array) rejects with `WrongType`. `memory`
and `ask` are optional: absent, `null`, or mistyped all read as `""`.

| Field | Meaning | Cap on parse |
|---|---|---|
| `reply` | User-facing text (`""` if none) | **uncapped** - user-facing; the device budgets it elsewhere |
| `memory` | Updated model running memory (`""` if unchanged) | `kMemModelMax` = **1200** bytes, UTF-8-safe |
| `ask` | Question to the user (`""` if none) | **uncapped** - user-facing; the device budgets it elsewhere |

`reply` names an action as done only when a tool result confirms it. Delivery is
itself a tool result: writing "sent to your Telegram" or "reading it aloud now"
does not send or speak anything - the matching tool (file send, voice readout)
does, and only its success result lets the reply claim it happened.

`memory` is clamped to `kMemModelMax` (1200) UTF-8-safe on parse so the parsed
`Turn` is self-consistent with the memory module's cap even before
`OrchMemory::setModel` runs; the definitive cap + truncation flag live there.

## Optional arrays

Every array key is optional and tolerant: absent reads as an empty vector, and a
value in another shape is repaired or reported by the normalizer (never an
error). A bare `{"reply":"hi"}` is
therefore a fully valid turn (all arrays empty, `memory`/`ask` `""`), and a new
turn can omit `spawn`/`await`/`device` and drive the run through the
live-integration arrays instead.

### `device[]` (tolerant per-item)

A non-object element is **dropped** (never a whole-turn error). Each accepted
element is carried as the **raw serialized JSON slice** of the element (compact, no spaces) into
`DeviceAction.json`; the portable device-action validator
(`lib/core/include/nimbus/orch/device_actions.h` and
`lib/core/src/orch_device_actions.cpp`, a sibling module) turns it into a typed+clamped
action, and the executor lives on the device. Keeping the raw slice here
decouples the turn parser from that module.

### `spawn[]` (tolerant per-item, retired from the wire)

Retired from the wire schema (spawning moved to `session_ops`) but kept fully
parseable, so an old client still works. Per item:

- A **non-object** element, or one whose `task` is **missing / non-string /
  empty**, is **dropped and reported** (`Turn::spawnDrops`) - `task` is the only
  semantically-required field. Every other
  field falls back to a default on copy, so a provider that omits `note` or
  sends a numeric `model` loses that item at worst, never the whole turn.
- Defaults, provider-lowercasing, and per-field UTF-8-safe byte caps are applied
  on copy:

| Field | Rule on copy | Byte cap (usable = buffer−1) |
|---|---|---|
| `provider` | **lowercased**; `""` ⇒ device resolves by priority | `kSpawnProviderMax`−1 = **15** |
| `model` | may be empty/invalid; device coerces | `kSpawnModelMax`−1 = **39** |
| `skill` | provider-native hint | `kSpawnSkillMax`−1 = **23** |
| `task` | the only **semantically-required** field | `kSpawnTaskMax`−1 = **4095** |
| `name` | short display name (`""` ⇒ unnamed) | `kSpawnNameMax`−1 = **23** |
| `category` | `""` ⇒ **`"research"`** | `kSpawnCategoryMax`−1 = **15** |
| `note` | `""` ⇒ **`"On it."`** | `kSpawnNoteMax`−1 = **79** |

- The accepted `spawn` vector is **truncated to `kAgentMaxJobs` = 6** items;
  extras are dropped (the journal ceiling refuses them device-side).

### `await[]` (tolerant per-item, retired from the wire)

Retired from the wire (sub-agent results arrive on their own in a later turn's
`[FRESH RESULTS]` block; there is no working poll) but still parsed: the first
kept tag still steers the next job poll to that sub-agent. A
**non-string** entry is **dropped**; entries are **trimmed** of surrounding
whitespace and blank / whitespace-only entries are **skipped** (a
whitespace-only tag can never match a real job tag, so keeping it would seed a
dead poll). Non-blank entries are kept in order.

## Live-integration arrays

`mem_write`, `mem_query`, and `session_ops` are the advertised path for a modern
turn. Like every other array they are **tolerant** per item (a malformed item is
dropped, never a whole-turn error), and each vector is capped to `kAgentMaxJobs`
= **6**.

### `mem_write[]` → `memory.write`

Store facts in the associative VDB this turn (explicit, LLM-directed - never
auto-mirrored).

| Sub-field | Type | Rule | Default |
|---|---|---|---|
| `content` | string, **required non-empty** | capped to `kMemModelMax` = **1200** UTF-8-safe; empty/missing ⇒ item dropped | - |
| `importance` | number | accepts **int- or float-encoded**; **clamped to `[0,1]`** | **0.5** |
| `permanent` | bool | pins the fact so it never decays or prunes | **false** |
| `ttl` | enum string | one of `session` / `days` / `weeks` / `months` / `permanent`; **accepted only when it names a known class**, otherwise ignored and the store's default class applies | store default (`weeks`) |

A non-object item, an item with a non-string / missing `content`, or an
empty-after-cap `content` is **dropped** - the turn stays valid.

The device applies `mem_write` while it applies the turn, **before** it delivers
`reply`/`ask` (`applyTurn`, `lib/harness/src/apply.cpp`). The model never sees the
write's result in the same turn, so a reply that says "I saved that" is still
unconfirmed from the model's side: phrase it as intent, or confirm it on a later
turn.

### `mem_query[]` → searches echoed into the NEXT turn

Search strings. Each entry is **trimmed**; blank ⇒ dropped; a non-string entry
is dropped (tolerantly, not an error). Kept entries are capped to
`kSpawnTaskMax − 1` = **4095** UTF-8-safe. Results are echoed into the *next*
turn's context.

### `session_ops[]` → spawn / terminate sub-sessions

The advertised way to start and stop sub-agents (superseding the retired
`spawn[]`/`await[]`).

- `op` (**required**) - the wire schema advertises **`spawn` and `terminate`
  only**. The fabric is fire-and-forget: there is no live back-and-forth with a
  running sub-agent, so `tell`, `poll`, and `list` were retired from the wire.
  The parser's known set still accepts those three names for old clients, but
  they are **no-ops** on this fabric. An item the normalizer cannot make usable
  (not an object, no `op`, an unknown `op`, a spawn with no task, a spawn past
  the per-turn cap of six) is dropped - and when it carried a task it is
  **reported** as a spawn that did not start: the owner gets one correction line
  on the turn's reply and the head a `[SPAWN REFUSED]` line on its next turn.
- Every other field is used per-op, defaulted-empty, and byte-capped. All nine
  keys are `required` in the strict wire schema. `op` is a non-nullable enum
  (`spawn` | `terminate`); the other eight (`id`, `task`, `provider`, `model`,
  `skill`, `name`, `project`, `attach`) are **nullable** types, and the parser
  defaults each:

| Sub-field | Used by op | Rule / cap (usable = buffer−1) |
|---|---|---|
| `id` | `terminate` | target session tag; cap `kSpawnCategoryMax`−1 = **15** |
| `task` | `spawn` | the NL instruction; cap `kSpawnTaskMax`−1 = **4095** |
| `provider` | `spawn` (optional) | **lowercased**; `""` ⇒ device resolves by priority; cap `kSpawnProviderMax`−1 = **15** |
| `model` | `spawn` (optional) | cap `kSpawnModelMax`−1 = **39** |
| `skill` | `spawn` (optional) | approved capsule id injected into the sub-agent's brief; cap `kSpawnSkillMax`−1 = **23** |
| `name` | `spawn` (optional) | display name, also the saved-document name; cap `kSpawnNameMax`−1 = **23** |
| `project` | `spawn` (optional) | run tag; the sub-agent's full result auto-saves to the file store as `<project>/<name>-<tag>.md`; cap `kSpawnProjectMax`−1 = **24** |
| `attach` | `spawn` (optional) | array of up to `kSpawnAttachMax` = **4** `<project>/<name>` docs whose full content the device splices into the instruction; each ref capped to **73** bytes, non-string entries dropped |

The retired `tell` op's `message` field (cap `kSpawnTaskMax`−1 = **4095**) is
still read by the parser but has no effect.

Each turn's `[ACTIVE SESSIONS]` context carries a `[SPAWN CAPACITY]` line
(`buildDynamicContext()`, `lib/harness/src/engine.cpp`) - `<live> running,
<queued> queued. You can start up to <N> sub-agents this turn`, where `N =
min(kAgentMaxJobs, kMaxPendingSpawns − queued)`. Spawns run **sequentially** (the
device does one network call at a time, up to `kMaxActiveInflight` = **4** in
flight), draining as each finishes. This is **not** a hard total limit: a big
fan-out runs over successive waves - start a wave now, and when they finish an
automatic turn lets you spawn the next, so a run can work through dozens. So a
turn emits one spawn op per unit of work **up to what `[SPAWN CAPACITY]` allows**,
and carries a larger fan-out across waves rather than silently dropping work.

## When a model calls a field as a tool

Some heads (seen on Anthropic and Mistral) call a reply field such as
`session_ops`, or a connector such as `gcal`, as if it were a live tool. A call
to a spawn field (`session_ops`, `spawn`, `session_spawn`, `spawn_agent`) is
read with the normalizer above: its usable spawn items are carried into the
turn as its `session_ops` and start when the turn ends, deduped against the
final `orch_turn` (provider plus the task's first 200 characters, case and
spacing folded), and the model is told they were taken. A call with nothing
usable gets the recovery text below, and if the turn then starts no sub-agent
the owner and the head are told. For every other unknown name the registry
(`ToolRegistry::unknownToolText`, `lib/core/src/orch_tool_registry.cpp`) answers
an unknown name with recovery text instead of a bare error, in this order:

1. **A field of this contract** (every `ORCH_SCHEMA_BODY` property, plus the
   retired `spawn` / `await` and the aliases `session_spawn`, `session_tell`,
   `session_poll`). The text says the name is not a tool but a field of the
   orch_turn reply, and how to fill it (for `session_ops`: add a
   `{"op":"spawn","provider":...,"task":...}` item and finish the turn).
   Spawn-type names add the live `[SPAWN CAPACITY]` number ("You can start up to
   6 sub-agents this turn"), computed by the same `spawnRoomFor()` as the prompt
   line, so the two can never disagree. A field with a live tool twin offers it
   (`mem_write` -> `memory_write`).
2. **A configured connector** (device name, catalog type or Mistral workspace id,
   case-folded). The text says it is a connector attached to that provider's
   sub-agent jobs, not a tool, and to spawn a job there with the request.
3. **Anything else**: `unknown tool: <name>.` plus the names this caller can call,
   in the spelling it can call them (`memory.write` for the LAN MCP endpoint,
   `memory_write` for the head loop unless it used a dotted name), bounded to 640
   bytes with a `(+N more)` tail.

The text is built per call from static strings; nothing is kept between calls.
Pinned by `test/test_orch_tools` (`test_unknown_tool_*`, including a walk over
every schema property so a new field without a hint fails).

## Persistent scratchpad (response object)

`scratchpad` is a top-level, **nullable object** response field - the model's own
working memory, rewritten as it works. Unlike `memory` (which lives for the
**next turn only**), the scratchpad **persists across turns and across restarts**
(`memory::scratchpad()`, serialized to the LittleFS file `/data/scratchpad.bin` via
`persistScratchpad()`; before v4.5.14 the `orchmem` NVS namespace). When a save
does not land, the notes live in RAM only until restart: the change result says
"kept until restart only (saving failed)" and the SCRATCHPAD prompt header says
saving is failing, so the model never promises otherwise. It is a **free write**:
returning it costs no tool round.

This response field is the model's normal write path. The admin-only
`memory.scratchpad` tool remains as a read/edit path for operators; see
[tool-catalog.md](tool-catalog.md).

Shape (every key `required`-but-nullable in the strict wire schema):

```json
"scratchpad": {
  "active": "the one thing being done right now (string | null)",
  "short":  [ "this task's steps / a checklist", ... ] | null,
  "mid":    [ "threads to return to over days", ... ]   | null,
  "long":   [ "standing goals", ... ]                   | null
}
```

Update semantics (parser step 10, `orch_turn.cpp`):

- The **whole field null** - or absent, or not an object - leaves the scratchpad
  **unchanged**.
- A **non-null `active`** string sets the active line, capped to
  `kScratchActiveMax` = **240** bytes, UTF-8-safe.
- A **non-null tier array** (`short` / `mid` / `long`) **replaces** that tier; a
  **null** tier leaves it unchanged. Each tier keeps at most `kScratchTierItems`
  = **8** items, each capped to `kScratchItemMax` = **160** bytes UTF-8-safe;
  blank / whitespace-only items are dropped.

Because a non-null tier is a full replace (not an append), the model rewrites the
whole tier each time: write the plan when a multi-step task or fan-out starts,
then tick items off on later turns so the thread survives across turns.

## ParseError codes

`parseTurn` returns `true` only when the turn is contract-valid (`err.code == Ok`). On
failure `out` is left **cleared** (no half-parsed state leaks) and `err` carries
the reason; `err.detail` names the offending field/reason for logs + tests.

| `ParseError::Code` | When |
|---|---|
| `Ok` | valid turn |
| `JsonError` | did not deserialize (garbage / truncated / empty / not JSON) - `detail` is the ArduinoJson error string |
| `NotObject` | valid JSON but the top-level value is not an object (e.g. `[1,2,3]`, `42`) |
| `MissingField` | `reply` is absent or `null`; `detail` is `"reply"` |
| `WrongType` | `reply` is present but not a JSON string |

`MissingField`/`WrongType` fire **only** for `reply`. `memory`/`ask` never fail a
turn (an off value reads as `""`), and no array - `device`, `spawn`, `await`, `mem_write`, `mem_query`, or `session_ops` -
can produce a parse error: a value the normalizer cannot repair reads as empty
and is named in `parseNotes`, and a malformed item is dropped. The `scratchpad`
object is equally tolerant: a value that is not an object (or a JSON string of
one) reads as **no change**, never an error.

## Backward-compatibility guarantee

An **old six-field turn** (`reply`/`memory`/`ask` + `device`/`spawn`/`await`,
no live-integration arrays) still validates and applies exactly as before -
`spawn[]` still parses, and the new `mem_write`/`mem_query`/`session_ops`
vectors simply stay empty. Symmetrically, a new turn may carry only `reply` plus
the live-integration arrays and omit everything else. In all cases, an absent -
or present-but-mistyped - array parses to an **empty vector**, and an absent
`memory`/`ask` parses to `""`.

## Validation order (as implemented)

1. Deserialize → `JsonError`; not an object → `NotObject`.
2. `reply` required string (`MissingField` / `WrongType`); `memory`/`ask`
   optional strings (absent / `null` / mistyped ⇒ `""`).
2a. The normalizer: leaked-markup repair, container shapes, `session_ops` item
   shapes; what it cannot repair goes to `parseNotes` / `spawnDrops`.
3. Copy string fields: `reply`/`ask` uncapped; `memory` capped to `kMemModelMax`.
4. `spawn[]` tolerant per-item: non-object / missing-or-empty-`task` items
   dropped and reported; other fields defaulted + provider lowercased + per-field
   byte caps; items past `kAgentMaxJobs` (6) reported.
5. `await[]` tolerant: non-string items dropped; blank/whitespace-only trimmed
   away.
6. `device[]` tolerant: non-object items dropped; each kept item carried raw.
7. `mem_write[]` tolerant; `importance` int-or-float clamped `[0,1]`; `ttl`
   accepted only when it names a known class; capped to `kAgentMaxJobs`.
8. `mem_query[]` tolerant; trimmed; capped to `kAgentMaxJobs`.
9. `session_ops[]` tolerant; `op` in the known set; a spawn needs a task;
   spawn fields (`skill`/`name`/`project`/`attach`) defaulted + capped; capped to
   `kAgentMaxJobs`; every dropped spawn intent reported.
10. `scratchpad` tolerant: whole field null / absent / non-object ⇒ no change;
    non-null `active` capped to `kScratchActiveMax`; each non-null tier
    (`short`/`mid`/`long`) REPLACES it - blank items dropped, ≤ `kScratchTierItems`
    items, each ≤ `kScratchItemMax`.

## Constant reference

| Constant | Value | Source | Applies to |
|---|---|---|---|
| `kMemModelMax` | 1200 | `caps.h` | `memory`, `mem_write.content` |
| `kSpawnTaskMax` | 4096 (−1 = 4095) | `caps.h` | `spawn.task`, `mem_query`, `session_ops.task`/`message` |
| `kSpawnProviderMax` | 16 (−1 = 15) | `caps.h` | `spawn.provider`, `session_ops.provider` |
| `kSpawnModelMax` | 40 (−1 = 39) | `caps.h` | `spawn.model`, `session_ops.model` |
| `kSpawnSkillMax` | 24 (−1 = 23) | `caps.h` | `spawn.skill`, `session_ops.skill` |
| `kSpawnNameMax` | 24 (−1 = 23) | `caps.h` | `spawn.name`, `session_ops.name` |
| `kSpawnProjectMax` | 25 (−1 = 24) | `caps.h` | `session_ops.project` |
| `kSpawnAttachMax` | 4 | `caps.h` | `session_ops.attach` (max docs; each ref ≤ 73 bytes) |
| `kSpawnCategoryMax` | 16 (−1 = 15) | `caps.h` | `spawn.category`, `session_ops.id` |
| `kSpawnNoteMax` | 80 (−1 = 79) | `caps.h` | `spawn.note` |
| `kAgentMaxJobs` | 6 | `caps.h` | vector caps for `spawn`, `mem_write`, `mem_query`, `session_ops`; `[SPAWN CAPACITY]` per-turn ceiling |
| `kMaxPendingSpawns` | 12 | `caps.h` | pending-spawn queue depth; `[SPAWN CAPACITY]` queue room |
| `kMaxActiveInflight` | 4 | `caps.h` | max sub-agents in flight (device concurrency window) |
| `kScratchActiveMax` | 240 | `caps.h` | `scratchpad.active` |
| `kScratchItemMax` | 160 | `caps.h` | each `scratchpad` tier item |
| `kScratchTierItems` | 8 | `caps.h` | items per `scratchpad` tier (`short`/`mid`/`long`) |
| `utf8CapLen` | - | `mem_cap.h` | UTF-8-safe byte cap (drops a straddling multi-byte char whole) |
