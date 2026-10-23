# Orchestrator "World" - memory, capability & session architecture

The cognitive architecture for Nimbus's Orchestrator mode: what the agent
*has* (memory tiers), what it *knows about itself* (capability manifest,
running sessions), and how it *acts* (tools over an on-device MCP server).

This document describes each subsystem - memory record + tiers, vector memory
semantics, working/context memory, the audit stream, the scratchpad goal tiers,
recall query + injection, the maintenance cadence, the memory tools, the tool
registry/dispatch, sessions (sub-agents), prompt assembly, the dashboard API,
and the provider config schema - on its own terms, adapted for ESP32-S3
constraints. Where a design choice is driven by the hardware's RAM/flash limits,
the constraint and its reason are called out explicitly.

## Locked decisions (2026-07-02)

- **Tool interface = on-device MCP server** (`memory.*` / `session.*` /
  `device.*`), bridged to the LLM via native function-calling; also reachable
  by external MCP clients over the LAN (Ph4, with auth).
- **Episodic store = append-only JSONL day-streams on the SD card** + blob
  sidecars (SQLite was evaluated and retired, see §1.3).
- **Embeddings configurable but SET-ONCE** - once vectors exist, changing
  provider/model/dims erases the vector memory (nothing is re-embedded); the web
  UI warns before it does.
- **SD card is the primary store** for tiers 2-4 (episodic + VDB). Without a card
  both run in a bounded degraded mode on internal storage (see
  [orchestrator-storage.md](orchestrator-storage.md) section 5); media, the file
  store and the memory archive need a card. Directive, scratchpad and sessions
  live in NVS and work without SD.

## 1. Cognitive tiers

The model-facing prompt (the `[HOW YOU RUN]` section, `lib/harness/src/compose.cpp`)
names **four memory tiers the model itself reads and writes**, in escalating
durability: **running memory** (the `memory` response field - short-lived working
notes for the NEXT turn only), the **scratchpad** (§1.2 - persistent goal tiers that
survive across turns and restarts), **long-term vector memory** (§1.4 - durable
embedded facts), and the **episodic log** (§1.3 - auto-captured conversation history).
The **directive** (§1.1) sits above all four: human-owned, the model cannot write it.

### 1.1 Directive (exists)
User-owned, immutable to the model. `OrchMemory::directive` from
`agent::store::sysPrompt` (1500 B cap). Ships with an owner-authored default
(`kOwnerDirectiveDefault`, the single compiled-in source); an empty stored value
uses that default, so a fresh device still carries the owner's baseline persona.
Set it during setup (the wizard Directive step) or any time on the web Directive
box, with a "Revert to default" affordance. Injected below the platform rules and
identity, delimited as owner preferences (it shapes style, never overrides
safety/moderation/access). Editable only by the human (web UI / provisioning).

### 1.1b Standing rules

The owner's short always/never rules ("never use em dashes", "meat weights I
give are raw"), stored in one small file on the internal flash
(`/data/prefs.txt`, at most 12 rules, 240 characters each, 1,200 characters of
rule text together, zero NVS keys) and rendered into the never-dropped part of
every owner-facing prompt as `[STANDING RULES]`, right under the directive.
A rule stated once is applied in every later reply without a reminder: memory
is recalled by topic, so a rule that applies to every topic almost never
surfaces from it.

The assistant adds one only when the owner asks in their own words
(`prefs.edit`, or a `mem_write` item marked `standing:true`), never from an
unattended turn, and a turn that read outside content can only add one that
waits for the owner's approval. The owner manages the list with `/prefs`
(list, add, rm, approve, clear) or the web page under Memory, beside the
directive; a first boot after the update seeds it from the always/never rules
already in memory and lists them once. A rule that forbids em dashes also
switches on a device-enforced filter that removes them from every delivered
reply, so the guarantee holds even when the model slips.

### 1.2 Scratchpad - model-owned working goals
A set of goal tiers (short-term / mid-term / long-term goals plus an
active-task line), which the model rewrites as it works:

```
scratchpad = {
  active: string                 // what I'm doing right now
  short: [string]                // this-session tasks
  mid:   [string]                // this-week intentions
  long:  [string]                // standing projects/aspirations
}
```

Device caps (enforced because of NVS/RAM limits): 1 active line
(240 B), 8 items/tier, 160 B/item, UTF-8-safe truncation
(`caps.h`: `kScratchActiveMax` / `kScratchItemMax` / `kScratchTierItems`, via
`capUtf8`). The model proposes, the device enforces - the same contract as the
memory caps. Persisted in NVS, so it **survives across turns AND restarts** and
works without SD.

**Write path (v4.1.0): the scratchpad is a first-class `orch_turn` RESPONSE
field, not a tool.** It rides the turn contract
(`orch_schema.h::ORCH_D_SCRATCH`) as a nullable object
`{active, short, mid, long}` - `active` a string, the three tiers string arrays.
The model returns it to update: a non-null tier REPLACES that tier's items
(≤8 each), a non-null `active` sets the active line, a null field leaves that part
unchanged, and returning the whole field null means no change
(`orch_turn.cpp` parse step 10). It is a **free write** - no tool round, returned
every turn by every provider alongside `reply`/`memory`. The device applies it
under the memory Lock, replacing only the returned tiers, then persists
(`orchestrator.cpp::applyScratch` → `memory::scratchpad()`; the harness path is
`lib/harness/src/apply.cpp`). Before v4.1.0 the scratchpad had no write trigger
the model could reach on its own, so it went unused.

It is rendered back into every prompt as a `## SCRATCHPAD` block **with its write
instruction printed directly beneath the data** (`orch_world.cpp`) - the fix for
"rendered but never written to." The `memory` field is a SEPARATE tier
(running memory, next-turn-only working notes), not a fallback for the scratchpad.
The admin-only `memory.scratchpad` MCP tool (actions
`view`/`set_active`/`add`/`replace`/`clear`) remains as a secondary read/edit
surface over the same single scratchpad; it is admin-gated because the scratchpad
is injected into every turn's prompt, so a non-admin writing it is a
prompt-injection channel aimed at the owner (`orch_memory_tools.cpp`).

### 1.3 Episodic store (SD) - everything that ever happened

> **Shipped (v4.0.0):** the SQLite index sketched below was evaluated and
> **retired**. The episodic store is an **append-only JSONL day-stream store with
> an in-RAM offset index** (`AppendLogEpisodicStore`) - the system of record.
> History is uncapped on SD and **reaches months back**: the boot scan is
> deliberately budget-bounded (so a busy month cannot make the device unbootable),
> and the rest of the card is indexed in the background right after boot. Until
> that finishes (and past a 20,000-row index cap) a deep query (`memory.episodic`
> cold-scan) pages down to the older rows with a resume cursor and reports an
> honest floor. The storage tiers, the boot-scan bound, the background indexing and
> the deep-history search are the canonical record in
> [`orchestrator-storage.md`](orchestrator-storage.md) §3a. The original
> design below is kept for reference.

**How a history search matches words.** `memory.episodic` (and the web
history search, `GET /api/mem/episodic?text=`) finds a row when **any** word of
the search occurs in it, so a natural question works even when the owner used
only one of the words: searching "advisor supervisor PhD" finds a row that says
only "my advisor reviewed the draft". The same rule runs every text search on the device: the
keyword fallback of `memory.search` and `memory.archive`, `files.search`, and
`docs.search`.

- Punctuation and the case of English letters never matter: "PhD-advisor?" is
  the words "phd" and "advisor". Accented and non-Latin letters match as typed.
- Common words do not count: "the", "is", "my", "what", "who", "again" and
  other everyday words (and any one-letter word) are ignored. A search made only
  of common words finds nothing and says so.
- Plural and verb endings are trimmed, and a trimmed word also finds its longer
  forms: "boats" finds "boat", "advised" finds "advisor".
- A word of four or more letters matches inside longer words ("boat" finds
  "lifeboat"); a shorter word must start a word ("phd" finds "PhDs", but "ai"
  never matches "said").
- With two or more counted words the results are **ranked**: rows holding more
  of the different words come first, newest first among equals, up to the
  requested limit (at most 200). When more rows matched than fit, the answer says
  how many were left out. With one counted word, results are newest first, as
  before.

Before this rule, every word had to appear, so adding a synonym guaranteed an
empty answer and the assistant concluded it had never been told.

The original design split this into (a) an **SQLite DB** for structured rows
and (b) an **append-only JSONL day-file stream** (`/data/blob/YYYY-MM-DD.jsonl`)
for the raw audit trail, plus adapter transcripts; it was proposed to serve both
SQL queries and grepping raw files. Only (b) shipped. The proposed layout, for
reference:

```
/sd/mem/nimbus.db          SQLite index
  sessions(id, started_at, agent_type, provider, model, title, status,
           turns, total_tokens)
  messages(id, session_id, ts, role, kind, text, blob_path, tags)
    role: creator | assistant | system | tool
    kind: message | tool_output | llm_response | file | image | audio |
          transcript | log            // event type + media
/sd/mem/blob/YYYY-MM-DD.jsonl   append-only raw event stream (grep-able)
/sd/mem/blobs/<sha1>.<ext>      binary artifacts (voice notes, images, files)
                                referenced by messages.blob_path
```

Retention: day-files older than 30 days move to `/sd/mem/blob/archive/`
(`retention_days=30`). (The SQLite index was never shipped; see the callout
above.)

### 1.4 Associative memory (VDB, SD)
Record:

```
{ id, content, importance_score (0-1, default 0.5),
  ttl_hours (default 720 = 30 d; -1 = none),
  created_at, source, creator_flag, permanent_flag, metadata }
```

**Semantics** (constants included):
- **Dedup on write:** nearest neighbor at cosine distance < **0.05** ⇒ skip
  the write but bump the existing entry's importance to max(old, new).
- **Decay:** each maintenance cycle multiplies non-permanent importance by
  **decay_factor (default 0.95)**, floor 0.01.
- **Prune:** delete when importance < **0.05** or age > ttl_hours;
  `permanent_flag` and `creator_flag` entries are exempt.
- **Browse/flush/dedupe-scan:** `get_all` (importance-sorted, paged),
  `delete(id)`, `flush_all`, `flush_non_permanent`, pairwise `deduplicate`
  keeping the higher-importance twin - all exposed to the dashboard.

**ESP32 delta - the embedding engine.** An ESP32 cannot run a local
embedder. Nimbus embeds via provider API
(default **OpenAI `text-embedding-3-small`, `dimensions=256`**, int8-quantized
≈ 256 B/vector + text) through a new adapter method on the existing HTTPS
path. Storage: vectors + metadata in one serialized blob (`/mem/vectors.bin` on
SD, `/data/orchvec.bin` without a card), recall =
brute-force cosine over int8 (thousands of entries ≈ ms on the S3, PSRAM).
The embed config `{provider, model, dims}` is stored beside the table;
mixed-config vectors are refused; changing the config once vectors exist erases
the vector memory (`POST /api/mem/embedcfg` with `reset=1`; nothing is
re-embedded), behind the loud UI warning.

Each embeddings provider gets its own request shape (see the Memory
embeddings table in the [capability matrix](reference/capabilities-matrix.md)).
OpenAI and Cumulo Nimbus carry the width as `dimensions`. Mistral has no width
field, so the model's own width comes back (`mistral-embed`: 1024) and the
saved Dimensions must equal it. A vector whose width doesn't match the memory
setting is refused with that reason, never truncated or padded. Saving
Dimensions as `0` takes the model's own width: the device reads it with one
live call and stores that number.

**When embeddings are unavailable.** An embedding round-trip can fail for many
reasons, and the memory tools name the real one instead of a single fixed guess.
The device seam reports the cause (no key for the configured provider, key
rejected, connection busy, provider unreachable, timeout, an HTTP status, a
malformed response, an embedding width that doesn't match the memory setting,
or a router refusal such as out of credit / rate limited / route not allowed)
and each maps to its own honest, secret-safe line (a key or Authorization header
is never echoed; only a known provider name, the numeric HTTP status, or the two
widths is shown). A billing refusal (out of credit or quota at the provider, or a
router budget) reads "out of embedding credit at <provider>" with the next step;
it uses the same billing table as the chat head, so a new provider token is
added once for both.

**Writes during an outage are queued, never lost.** On the device (and on a
hosted Virtual Nimbus) a `memory.write` (or `memory.update`) the embedder cannot serve is kept as text in
a small pending queue and stored automatically once embeddings work again, with
the same rules as a direct write (namespace, memory limit, time to live, pins),
under the writer's role at that time. Every write ends in exactly one outcome,
and the tool result says which:

| Outcome | Meaning |
|---|---|
| `stored` | Embedded and in long-term memory. |
| `duplicate` | The same fact is already stored. |
| `queued` | Kept on the device, not stored yet; stored automatically when embeddings work again. Keyword search already finds it (marked `pending, not stored yet`). |
| `refused` | Not stored, with the reason: nothing to embed, the conversation's memory limit, the queue is full, or the device could not write the queue to storage. |

The queue holds at most 64 facts or 48 KB, and 16 per member conversation; when
full it refuses new writes and never drops a queued fact. It lives on the SD card
(`/mem/pending_embed.bin`), or on internal flash (`/data/pendemb.bin`) with no
card or while the card is away; a hosted instance keeps it beside its vectors. "Queued" is claimed only after that file is
written. Facts are kept as text, so a later embedding-model change does not
strand them. While the provider keeps refusing, the device stops calling it:
billing, key and setup failures pause embedding for 30 minutes and then try once;
transient failures back off from 30 seconds up to 30 minutes. Writes made during
the pause queue in milliseconds, automatic recall skips the embed call, and a
success ends the pause. The owner gets one plain notice per billing episode, shared with the
chat notice for the same provider. One queued fact is stored per step: on the
Telegram task between its own work (never inside a turn, not in the first minute
after a restart, at most every 20 seconds, and only with enough free memory for
the call), up to 10 at the start
of the nightly dream, and the queue persists across restarts. A fact that waits
past a role change is stored under the new role, and a member who loses access
loses their queued facts too. Erasing storage clears the queue. Saving a new
embedding model (even with a reset of the stored vectors) keeps it: the facts
are stored with the new model.

The model hears about every outcome. A `mem_write[]` item that was queued or not
stored is reported in the next turn's MEMORY RESULTS, and a queued fact that was
stored later is reported the same way ("now stored"), so "I'll remember that" can
be confirmed a turn later instead of being a guess. Without the queue (the
host agent lab), a write the embedder cannot serve is refused with the reason, and update embeds the new text before
removing the old fact, so a failed update never loses the prior memory.

`memory.search` and `memory.archive` search **degrade** instead of failing: they
run a bounded, case-insensitive keyword scan over the same rows (and the queued
facts), inside the same namespace boundary the semantic path enforces, and label
the result `keyword match, embeddings unavailable: <reason>` so the model does
not mistake it for semantic recall. A transient embed outage therefore reads as
"keyword match" plus the real cause, not as empty memory.

## 2. Per-turn context assembly

The system prompt is assembled in this order (each section byte-capped; total
default **32 KB**, tunable - the budget is in bytes rather than tokens because
the constraint is device RAM):

1. **Immutable rules** - safety + device rails (model cannot touch keys/
   priority/host; see §6).
2. **Identity/version** - "You are Nimbus vX on an ESP32-S3 desk device…", naming
   the board the firmware was built for (Solide S3, or the Freenove display board
   and its screen size), and the `hardware` skill describes that same board.
3. **DIRECTIVE** - user text, verbatim.
4. **Capability/world manifest** (§3) - hardware + tool list generated from
   the registry into the `## CAPABILITIES` section
   (`orch_world.cpp::renderCapabilities`).
5. **Running-sessions digest** (§4) - a summary of the active sessions.
6. **Recent conversation** - the chat's folded summary, then the per-chat window
   of recent messages, trimmed oldest-first when over budget (see
   [turn anatomy](turn-anatomy.md)).
7. **Scratchpad** (§1.2) - active line + three goal tiers, rendered as a
   `## SCRATCHPAD` block with the write instruction printed directly beneath the
   data (`orch_world.cpp`) so the model always sees how to update it via the
   `scratchpad` response field.
8. **RELEVANT MEMORIES** - top-K recall (§2.1).
9. **Memory explainer** - a `## HOW YOUR MEMORY WORKS` block explaining automatic
   recall, `mem_write[]`, `mem_query[]` and the `memory` field (v1 prompt, the
   default; the v2 prompt drops it because `[HOW YOU RUN]` already carries the
   memory tiers, see §1).

### 2.1 Recall
When embeddings cannot serve recall (paused, no key, a failed call), the turn
gets a keyword match over the same memories and the queued facts instead, and
RELEVANT MEMORIES opens with one line saying recall is keyword-only and why, so
an outage never reads as "nothing relevant". The device, a hosted instance and
the lab share this rule.

The keyword scan copies nothing from the store: it picks at most the 2,000 most
important memories by importance alone, scores each one where it is stored, reads
the queued facts in place, and builds text only for the hits it returns, so its
memory use depends on the number of hits, not on the size of the store. On the
device it also holds to the memory floors of the embedding call (12 KB free and a
5 KB contiguous block): below them it skips the scan, and the line says that
keyword recall was skipped because the device is low on memory.
```
query = active_task + " " + latest_user_messages + " " + all_scratchpad_goals
hits  = vdb.search(query, k = retrieval_count)          # default 10
hits  = [h for h in hits if (1 - h.distance) >= relevance_threshold]  # default 0
inject as "## RELEVANT MEMORIES" bullets
```
`retrieval_count` (1–100), `relevance_threshold` (0–1), `decay_factor`
(0.5–1), `max_context_bytes` are the **memory config** - adjustable by BOTH
the model (`memory.config` tool, clamped to those ranges) and the user
(dashboard PUT `/api/mem/config`).

### 2.2 Maintenance
Periodically (each N turns / daily): `vdb.decay_importance(decay_factor)` then
`vdb.prune_expired()`; blob day-file archival monthly. Tool results worth
keeping are written back via `memory.write` (the model decides what to keep).

On a device **with an SD card**, `prune_expired()` does not delete: an expired
memory is **moved to the archive** (`/mem/archive.bin`, embedding preserved) instead,
so it can be found or restored later without paying to re-embed it. The model reaches
it through `memory.archive` (`search` / `restore` / `list`), which is offered only
when a card is present; `restore` returns the fact to the live store, counting against
the normal memory limit. With no card, expiry deletes exactly as before. See
[orchestrator-storage.md §2a](orchestrator-storage.md).

### 2.3 Rapid-message batching (one turn per burst)

When several messages arrive in one chat in quick succession (a thought split
across a few quick sends, or messages that pile up while a turn is running), the
device drains them into **one turn with one reply** instead of answering each
separately. The model still sees them as distinct, ordered messages (each rendered
as its own delimited block that keeps its sender), so nothing is lost or merged
into a run-on; it is simply one coherent answer rather than several disjoint ones,
at one turn's cost instead of N.

This applies uniformly across channels (Telegram, and the web/serial/voice inject
queue). Key properties:

- **Grouped by chat.** Messages from different chats that arrive together stay
  separate: one turn per chat, never merged across principals.
- **Bounded.** The accumulator is hard-capped by messages per chat, bytes per
  chat, distinct chats, and total bytes (`orch::kBatch*` in `caps.h`), and lives
  in PSRAM, so it can never overflow. If a burst exceeds a cap, the device runs
  what it has as one turn and handles the rest on the next cycle; a message is
  never silently dropped, and the turn's context notes that more are still waiting.
- **Drains a page at a time.** Each poll cycle handles one fetch of messages: it
  batches them, runs the turns, and records how far it got before fetching the next
  page on the next cycle. A rapid burst fits one fetch, so it is still one turn; a
  larger backlog drains one page per cycle.
- **Ordering preserved; no reentrancy.** A message that arrives while a turn is
  running joins the next batch, never the one in flight.
- **Still fully serial.** Batching only groups already-fetched messages; it adds
  no background task and no second network session (the single-task, single-TLS
  rule in `AGENTS.md` §4 is unchanged).

Crash-safety: nothing is lost. The device advances its saved position only after a
page's turns have run and that position is written down, and it never confirms a
page with the server until the next fetch. So a restart (brownout, watchdog, panic,
update) mid-turn re-serves the whole page it was working on, and a message can at
worst be answered again, never dropped. Voice notes, photos, and documents are not
batched: each still runs its own turn.

## 3. Capability & world manifest

Generated, never hand-written (so it can't drift): hardware inventory (ring,
screen, mic/speaker, battery state), connectivity (Wi-Fi/Bluetooth/Telegram
up?), SD present?, and the tool list pulled from the MCP registry with
one-line descriptions. The model knows its verbs and its body.

The manifest also states the model's **self-model in plain language**
(`orch_world.cpp::renderCapabilities`) - what it can actually do, so it neither
invents capabilities it lacks nor refuses ones it has:

- **Sub-agents** run in the background and are **fire-and-forget** - the model
  cannot talk to a running one. Each result is delivered to the owner as it
  lands, and once they have all finished the model is automatically given one
  more turn (a `[FRESH RESULTS]` block, with no new owner message) to synthesize
  them and update memory.
- **Skills authoring (v4.0.0):** the model **can write its own reusable skill
  capsules** (`skill.save` / `skill.delete`, admin conversations only). Approval
  is **asynchronous** - a saved capsule is server-stamped `created_by: agent`,
  `approved: false`, and stays **inert** (never injected into a spawn brief)
  until the owner approves it in the web UI or via Telegram `/skill approve`. So
  saving a skill is an investment for FUTURE runs, never a step of the current
  task; the model may delete only capsules it authored. Rails in §6.
- **Honesty:** one rule (the `reply` field description) covers every claim of an
  effect, a durability, live data or a figure from a source, and the device checks
  each claim against the turn's recorded effects before delivery (see the claim
  check in [turn-anatomy.md](turn-anatomy.md)); owner-only knobs (keys, routing)
  are named as owner-only so it can explain the boundary instead of silently
  trying and failing. A pin on an already pinned memory answers "already pinned,
  no change".

## 4. Sessions as conversational sub-agents

A conversation-manager + agent-adapter layer over the existing heavy fabric +
6-slot `Journal`:

- **Adapter seam**: `create_session(title, cfg)`,
  `send_message(id, text)`, `get_status(id)`, `abort(id)`, `cleanup(id)`.
  Provider adapters (openai/anthropic/custom) implement it for remote
  sub-sessions; the Journal keeps re-attaching running jobs after a restart, as
  before.
- **Digest injected each turn**: `[{id, agent_type/provider, model, title,
  turns, tokens, state, pending_reply}]`.
- **Model surface** (`session_ops`, v4.0.0): the turn contract advertises
  exactly **`spawn`** and **`terminate`** - "advertised == callable" is a
  standing rule, so the conversational ops (`tell`/`poll`/
  `list`) are **deliberately not offered**: the fabric is fire-and-forget, and
  output returns through the running-sessions digest plus an automatic synthesis
  turn, not a model-issued poll. Beyond `task`/`provider`/`model`, a `spawn`
  carries `skill` (an approved capsule id injected into the brief), `name`
  (display name, also the sub-agent's saved-document name), `project` (a run tag:
  each sub-agent's full result auto-saves as `<project>/<name>-<tag>`), and
  **`attach`** - up to 4 `<project>/<name>` documents the firmware reads from the
  file store and splices into the brief, so a sub-agent can read files it has no
  tools to open. The full brief format, per-provider wire, and what the head can
  and cannot do with a running or finished session are in
  [`sub-sessions.md`](sub-sessions.md).
- **Hygiene**: stale active sessions time out; idle sessions are cleaned
  after 30 min; every turn is persisted to the
  episodic store (transcript + blob emit).

## 5. On-device MCP server & tool registry

Registry semantics: every tool = name +
description + JSON schema + timeout; dispatch validates against the safety
rails, executes with a timeout, **audits every call to the blob stream**
(tool, params ≤500 B, success, duration, error), and returns a uniform
`{success, output, error, error_type}`.

- **Namespaces:** `memory.*` (write, search, update, pin, config, `scratchpad`
  - the admin-only read/edit surface; the model's normal scratchpad write is the
  `scratchpad` response field, §1.2 - and `episodic`, the deep-history search,
  §1.3), `session.*` (§4), `skill.*`
  (list, get, and **save/delete** - authoring, v4.0.0, admin-only and
  owner-approval-gated), `files.*`/`artifact.*` (the durable SD file store),
  `system.health`, `device.*` (existing validated device actions - led/tts/
  screen; secret-block preserved).
- **Transport:** JSON-RPC 2.0 (MCP). In-process for the orchestrator bridge
  (Ph1); WebSocket on the LAN for external clients (Ph4, token-auth).
- **Bridge:** the orchestrator advertises the registry to the LLM through
  native function-calling (OpenAI tools / Anthropic tool_use) and runs the
  tool-use loop (bounded iterations/turn), feeding results back - planning and
  execution collapse into this one loop on-device.

## 6. Security rails (unchanged, now enumerated in the immutable-rules block)

- The model may NEVER read or set provider keys, `providerPriority`,
  `orchHost`, or the Telegram token - no tool exposes them (redirecting its
  own brain / exfiltrating credentials). The HUMAN sets them via the web UI.
- `device.*` keeps the existing validator (clamps, allowlist).
- **Skill authoring is admin-only and approval-gated** (v4.0.0): `skill.save` /
  `skill.delete` require an admin conversation; a capsule the model writes is
  server-stamped `created_by: agent`, `approved: false`, and stays inert (never
  injected into a spawn) until the owner approves it - an agent-written
  instruction blob is a prompt-injection channel until a human signs off. Both
  ops are **refused in scheduled/unattended turns** (a routine firing, or a
  synthesis turn chewing on untrusted sub-agent output, must never persist a
  skill), the pending-approval queue is capped, and the model may delete only
  what it authored.
- Episodic and memory reads are typed tools (no free-form query language); the
  earlier `memory.sql` SELECT surface was dropped when SQLite was retired (§1.3).
- LAN MCP (Ph4) requires a bearer token minted on the device; off by default.

## 7. Dashboard (web UI) - memory routes

- `GET /api/mem/stats` - vector count and cap, scratchpad items, episodic row
  count, embed config, storage tier (`sdPresent`, `sdMissing`, `flashFull`,
  `store`), `pending` (memory writes waiting for an embedding) and `embedParked`
  (why embeddings are paused, empty when they are not).
- `GET /api/mem/vector?query&limit&offset` - browse (importance-sorted) or
  semantic search.
- `POST /api/mem/vector?op=delete|permanent|temporary|flush|flushnp|dedupe` - row
  actions, flush and dedupe (`op=flush` also needs `confirm=DELETE`).
- `GET /api/mem/episodic?kind&session&text&sinceh&beforeh&before&cold&limit` -
  filtered episodic browse.
- `GET /api/mem/blob?path=/mem/blobs/<name>.<ext>` - the full text behind a
  clipped row.
- `GET /api/lastturn` - the exact system prompt, per-turn input and raw output of
  the last turn, as plain text: the single most useful debug view.
- `GET/PUT /api/mem/config` - retrieval_count / relevance_threshold /
  decay_factor / max_context_bytes / max_vectors / recency_half_life_hours /
  mmr_lambda (same clamps as the model's tool).
- `GET/POST /api/mem/scratchpad`, `GET/POST /api/mem/embedcfg`,
  `POST /api/mem/embedverify`, `GET /api/mem/nsusage`, `GET /api/tools`,
  `POST /api/mem/import`, `GET /api/sdprobe`.
- Live sessions show in `[ACTIVE SESSIONS]` and the Home page's
  **Active sessions** card.
- Embed-config panel with the **set-once destructive-change warning**.
- Scratchpad + directive editors (directive human-editable; scratchpad
  human-viewable, clearable).

## 8. Phasing

- **Ph1 (portable, no SD/network):** scratchpad module · capability manifest ·
  sessions digest · context assembler (§2 ordering + budgets) · tool registry
  + in-process MCP dispatch · memory-config clamps. All host-tested.
- **Ph2 (SD):** SQLite episodic store + JSONL blob stream + artifact sidecars
  + `memory.sql`/`memory.grep` tools + retention/archival (superseded: the
  append-log store shipped instead).
- **Ph3 (SD+net):** embedding adapter (set-once) · int8 VDB + recall +
  dedup/decay/prune · `memory.write/search/config` tools · dashboard memory
  panels.
- **Ph4:** conversational session control (`tell`/`poll` as user) · LAN MCP
  + auth · dashboard session console.
