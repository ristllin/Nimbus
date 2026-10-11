<!-- audience: dev -->
# Orchestrator Turn Contract

The **turn contract** is the JSON shape every provider-hosted model must return
for one Orchestrator turn. Use this page when you write or debug a provider
adapter, a prompt, or a test that feeds `parseTurn()`. The portable, Arduino-free
`parseTurn()` parses and validates a provider's raw response string into a typed
`Turn`, so the device turn loop never reads a raw `JsonDocument`.

- Authoritative doc-comment: `lib/core/include/nimbus/orch/turn.h`
- Wire schema advertised to providers: `lib/core/include/nimbus/orch/orch_schema.h` (`ORCH_SCHEMA_BODY`)
- Parser: `lib/core/src/orch_turn.cpp` (`parseTurn`)
- Behavior pinned by tests: `test/test_orch_turn/main.cpp`
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
  `session_ops`) is **tolerant**: a present-but-not-array value reads as
  **empty**, and a malformed *item* is **dropped silently**, never a whole-turn
  error.

The wire schema still lists `memory` and `ask` under `required`: OpenAI strict
mode demands every property be listed there. Only the parser is lenient.

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
  "scratchpad":  { "active","short":[...],"mid":[...],"long":[...] } /* nullable object; replace-per-tier; persists across turns and restarts */,

  "spawn":   [ { "provider","model","skill","task","category","note","name" } ],
  "await":   [ "job-tag string", ... ]
}
```

Every sub-field the wire schema lists is `required` there but **nullable**
(strict json_schema has no optional keys - an omitted value rides as `null`); the
parser then defaults each one, so at the parser they are all optional. `spawn[]`
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

Every array key is optional and tolerant: absent reads as an empty vector, and
present-but-not-an-array also reads as empty (`as<JsonArrayConst>()` on a
non-array iterates zero times; it is not an error). A bare `{"reply":"hi"}` is
therefore a fully valid turn (all arrays empty, `memory`/`ask` `""`), and a new
turn can omit `spawn`/`await`/`device` and drive the run through the
live-integration arrays instead.

### `device[]` (tolerant per-item)

A non-object element is **dropped** (never a whole-turn error). Each accepted
element is carried as the **raw serialized JSON slice** of the element (compact, no spaces) into
`DeviceAction.json`; the portable device-action validator
(`nimbus/orch/device_actions.*`, a sibling module) turns it into a typed+clamped
action, and the executor lives on the device. Keeping the raw slice here
decouples the turn parser from that module.

### `spawn[]` (tolerant per-item, retired from the wire)

Retired from the wire schema (spawning moved to `session_ops`) but kept fully
parseable, so an old client still works. Per item:

- A **non-object** element, or one whose `task` is **missing / non-string /
  empty**, is **dropped** silently - `task` is the only semantically-required
  field (matches the device `enqueueSpawn` `if (!task[0]) return;`). Every other
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

Retired from the wire (`session.poll` superseded it) but still parsed. A
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

A `mem_write` applies **after this turn's reply is sent**, so a reply that says
"I saved that" is a prediction, not a confirmed fact - phrase it as intent, or
confirm it on a later turn.

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
  they are **no-ops** on this fabric. A non-object item, a non-string `op`, or an
  **unknown `op` is dropped** (turn stays valid).
- Every other field is used per-op, defaulted-empty, and byte-capped. All nine
  keys (`op`, `id`, `task`, `provider`, `model`, `skill`, `name`, `project`,
  `attach`) are `required` in the strict wire schema as **nullable** types; the
  parser defaults each:

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

Some heads (seen on Mistral) call a reply field such as `session_ops`, or a
connector such as `gcal`, as if it were a live tool. The registry
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
(`memory::scratchpad()`, serialized to the `orchmem` NVS namespace via
`persistScratchpad()`). It is a **free write**: returning it costs no tool round.

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
can produce a parse error: a present-but-not-array value reads as empty, and a
malformed item is dropped. The `scratchpad` object is equally tolerant: a
present-but-non-object value reads as **no change**, never an error.

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
   optional strings (absent / `null` / mistyped ⇒ `""`); every array key
   optional and tolerant (absent OR present-but-not-array ⇒ empty).
3. Copy string fields: `reply`/`ask` uncapped; `memory` capped to `kMemModelMax`.
4. `spawn[]` tolerant per-item: non-object / missing-or-empty-`task` items
   dropped; other fields defaulted + provider lowercased + per-field byte caps;
   vector truncated to `kAgentMaxJobs` (6).
5. `await[]` tolerant: non-string items dropped; blank/whitespace-only trimmed
   away.
6. `device[]` tolerant: non-object items dropped; each kept item carried raw.
7. `mem_write[]` tolerant; `importance` int-or-float clamped `[0,1]`; `ttl`
   accepted only when it names a known class; capped to `kAgentMaxJobs`.
8. `mem_query[]` tolerant; trimmed; capped to `kAgentMaxJobs`.
9. `session_ops[]` tolerant; `op` in the known set; spawn fields
   (`skill`/`name`/`project`/`attach`) defaulted + capped; capped to
   `kAgentMaxJobs`.
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
