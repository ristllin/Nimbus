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
- **Episodic index = SQLite on the SD card** + raw JSONL day-files + blob
  sidecars (a SQLite index paired with a `/data/blob/*.jsonl` raw stream).
- **Embeddings configurable but SET-ONCE** - changing provider/model/dims
  wipes + re-embeds the VDB; the web UI must warn loudly (destructive).
- **SD card REQUIRED** for tiers 2–4 (episodic + VDB). Directive + scratchpad
  + sessions work without SD.

## 1. Cognitive tiers

The model-facing prompt (the `[HOW YOU RUN]` section, `lib/harness/src/compose.cpp`)
names **four memory tiers the model itself reads and writes**, in escalating
durability: **running memory** (the `memory` response field - short-lived working
notes for the NEXT turn only), the **scratchpad** (§1.2 - persistent goal tiers that
survive across turns and reboots), **long-term vector memory** (§1.4 - durable
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
memory caps. Persisted in NVS, so it **survives across turns AND reboots** and
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
> which leaves older rows on the card *below* the index; a deep query
> (`memory.episodic` cold-scan) pages down to them with a resume cursor and reports
> an honest floor. The storage tiers, the boot-scan bound, and the deep-history
> search are the canonical record in
> [`orchestrator-storage.md`](orchestrator-storage.md) §3a. The original
> design below is kept for reference.

The original design splits this into (a) an **SQLite DB** for structured rows
and (b) an **append-only JSONL day-file stream** (`/data/blob/YYYY-MM-DD.jsonl`)
for the raw audit trail, plus adapter transcripts. Nimbus keeps that exact split
because it satisfies both requested query modes - SQL queries AND grepping
raw files:

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
(`retention_days=30`); the SQLite index is permanent (it is small relative to
the SD).

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
path. Storage: vectors + metadata in SQLite (`vdb` table), recall =
brute-force cosine over int8 (thousands of entries ≈ ms on the S3, PSRAM).
The embed config `{provider, model, dims}` is stored beside the table;
mixed-config vectors are refused; changing config = wipe + re-embed from the
episodic store (background job) behind the loud UI warning.

## 2. Per-turn context assembly

The system prompt is assembled in this order (each section byte-capped; total
default **32 KB**, tunable - the budget is in bytes rather than tokens because
the constraint is device RAM):

1. **Immutable rules** - safety + device rails (model cannot touch keys/
   priority/host; see §6).
2. **Identity/version** - "You are Nimbus vX on an ESP32-S3 desk device…"
3. **DIRECTIVE** - user text, verbatim.
4. **Capability/world manifest** (§3) - hardware + tool list generated from
   the registry into a `## TOOLS` section.
5. **Scratchpad** (§1.2) - active line + three goal tiers, rendered as a
   `## SCRATCHPAD` block with the write instruction printed directly beneath the
   data (`orch_world.cpp`) so the model always sees how to update it via the
   `scratchpad` response field.
6. **Running-sessions digest** (§4) - a summary of the active sessions.
7. **RELEVANT MEMORIES** - top-K recall (§2.1).
8. **MEMORY explainer** - a `## MEMORY` section telling the model how its memory
   works and that `memory.config` can tune it.
9. Recent conversation (rolling, trimmed front-first when over budget;
   a summarize-and-compress step keeps the last 2 messages + a summary block).

### 2.1 Recall
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
[orchestrator-storage.md §2a](orchestrator-storage.md) and CUM-225.

## 3. Capability & world manifest

Generated, never hand-written (so it can't drift): hardware inventory (ring,
screen, mic/speaker, battery state), connectivity (Wi-Fi/BLE/Telegram
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
- **Honesty:** it claims to have logged, saved, or remembered something only when
  it actually called the corresponding tool that turn; owner-only knobs (keys,
  routing) are named as owner-only so it can explain the boundary instead of
  silently trying and failing.

## 4. Sessions as conversational sub-agents

A conversation-manager + agent-adapter layer over the existing heavy fabric +
6-slot `Journal`:

- **Adapter seam**: `create_session(title, cfg)`,
  `send_message(id, text)`, `get_status(id)`, `abort(id)`, `cleanup(id)`.
  Provider adapters (openai/anthropic/custom) implement it for remote
  sub-sessions; the Journal keeps reboot re-attach exactly as today.
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

## 7. Dashboard (web UI) - `/memory/*` + `/providers*` routes

- `GET /api/mem/stats` - VDB count, blob file count/MB, DB size.
- `GET /api/mem/vector?query&limit&offset` - browse (importance-sorted) or
  semantic search; row actions: delete, mark-permanent, importance shown.
- `POST /api/mem/vector/flush?keep_permanent` · `POST /api/mem/vector/dedupe`.
- `GET /api/mem/blob?kind&limit` - filtered episodic browse by day/kind.
- `GET /api/mem/working` - **the prompt-sections snapshot**: exactly what the
  model saw last turn, section by section, with byte/token estimates - the
  single most useful debug view.
- `PUT /api/mem/config` - retrieval_count / relevance_threshold /
  decay_factor / max_context_bytes (same clamps as the model's tool).
- `GET/POST /api/sessions` + transcript view.
- Embed-config panel with the **set-once destructive-change warning**.
- Scratchpad + directive editors (directive human-editable; scratchpad
  human-viewable, clearable).

## 8. Phasing

- **Ph1 (portable, no SD/network):** scratchpad module · capability manifest ·
  sessions digest · context assembler (§2 ordering + budgets) · tool registry
  + in-process MCP dispatch · memory-config clamps. All host-tested.
- **Ph2 (SD):** SQLite episodic store + JSONL blob stream + artifact sidecars
  + `memory.sql`/`memory.grep` tools + retention/archival.
- **Ph3 (SD+net):** embedding adapter (set-once) · int8 VDB + recall +
  dedup/decay/prune · `memory.write/search/config` tools · dashboard memory
  panels.
- **Ph4:** conversational session control (`tell`/`poll` as user) · LAN MCP
  + auth · dashboard session console.

## 9. Open items

- SQLite-on-ESP32 lib (`siara-cc/esp32_arduino_sqlite3_lib`) flash/RAM cost on
  the N16R8 - validate early in Ph2; fallback: typed flat-file index.
- int8 quantization recall accuracy at 256 dims (bench vs f32 on host).
- Tool-use loop cap per turn (start with 8 actions).
- Re-embed UX (background job progress on dashboard).
- Whether `memory.sql` free-form SELECT is worth the parser risk vs. canned
  query templates (by-date, by-kind, by-session, full-text).

## Build status (2026-07-03)

Legend: **done** = built + host-tested · **hw** = hardware-verified on-device ·
**compile** = device code built + compile-verified · **seam** = portable core done,
device seam pending · **designed** = designed only

**307 native tests green + Part B MERGED to main (merge `f9baf9e`) + HARDWARE-
VERIFIED E2E** on the bench device. The UX2/BLE work is
committed + tagged `agent2-ux2-ble`. On-device E2E (11/11): live OpenAI embedding
→ int8 → store → cosine recall (the "teal" memory ranked first for a color query)
over `POST /mcp`, `session.list`, the memory dashboard endpoints, the set-once
embed lock, AND durable persistence across reboot (after fixing a LittleFS
`/data`-dir bug, commit `0d1bf95`).

### Ph1 - portable cognitive core  [done] (28 host tests)
- [done] Scratchpad (`orch/scratchpad`): short/mid/long goal tiers + active task,
  device-capped, NVS-serializable.
- [done] MemConfig (`orch/mem_config`): retrieval/decay clamps, model+user tunable.
- [done] Capability manifest + running-sessions digest + context assembler (`orch/world`).
- [done] Tool registry + MCP JSON-RPC 2.0 dispatcher (`orch/tool_registry`).
- [seam] Wiring into the live orchestrator turn loop - deferred: the console `TURN`
  path reboots the device (watchdog), so live-turn changes can't be hardware-
  verified yet; the memory server runs decoupled (web + MCP), so this is additive.

### Ph2 - episodic store  [seam] (7 host tests)
- [done] Records (session/message + kind column) + query model (session/kind/time/
  text) + `InMemoryEpisodicStore` reference (`orch/episodic`), used on-device now.
- [designed] Device SQLite-on-SD impl behind the same `EpisodicStore` interface +
  JSONL day-stream + blob sidecars - needs an SD card + the esp32 sqlite lib.

### Ph3 - associative vector memory  [hw] hardware-verified (24 host tests)
- [done] `VectorMemory` (`orch/vector_memory`): int8 vectors, cosine recall,
  dedup/decay/prune/TTL/permanence; + binary serialize/
  deserialize for the SD/flash blob.
- [done] `memory.*` MCP tools (`orch/memory_tools`): write/search/config/scratchpad,
  host-tested E2E over JSON-RPC with an injected fake embedder.
- [done] Portable embeddings wire format (`orch/embedding`): build request + parse
  response (OpenAI shape), host-tested.
- [hw] **Device (hardware-verified)**: `agent::embeddings` (real OpenAI
  `/embeddings` TLS call → int8), `agent::memory` subsystem (engines + real
  embedder + LittleFS/NVS persistence + registered tools), web dashboard
  (`net/web_memory`: browse/search/tune/embed-config-set-once). Live embed →
  store → recall → persist-across-reboot all confirmed on the bench device.

### Ph4 - sessions + LAN MCP  [hw] device / [compile] bidirectional pending (9 host tests)
- [done] `session.*` tools (`orch/session_tools`): spawn/tell-as-user/poll/terminate/
  list, host-tested with injected handlers + over MCP.
- [hw] **Device**: `POST /mcp` LAN JSON-RPC endpoint + `session.list` wired to the
  journal - hardware-verified (session.list returns over `/mcp`).
- [compile] Bidirectional `spawn/tell/poll/terminate` handlers are documented null seams
  (report "not supported") until the fabric exposes conversational sub-session
  control; MCP LAN auth is an open item.

### What's proven where
- **Part A control surface**: hardware-verified E2E (the bench device,
  live OpenAI key verify=1).
- **Part B memory system**: hardware-verified E2E (11/11 on the bench device -
  live embed→store→recall→persist over `/mcp` + dashboard) + 307 native tests,
  merged to main. Remaining: SQLite-on-SD episodic impl, live-turn-loop recall
  injection (blocked by the console-`TURN` watchdog reboot), bidirectional
  session control, MCP LAN auth.

### Hardware notes (2026-07-03 bring-up)
- **A full `esptool write-flash` resets NVS** (mode → Notifier, Wi-Fi creds + API
  keys wiped) - re-provision after every flash: `WIFI <your-ssid>|<pass>` over
  the console, then the OpenAI key via `/api/orch`. A plain reboot keeps NVS.
- **Wi-Fi STA rejoin is slow/flaky right after a flash or reboot** (tens of
  seconds, sometimes needs a re-provision); it joins reliably from a clean boot.
  Suspected Wi-Fi/BLE radio contention in the merged firmware (BLE is on in
  Notifier mode).
- **LittleFS won't auto-create parent dirs** - the vector blob at
  `/data/orchvec.bin` needs an explicit `mkdir("/data")` (fixed, `0d1bf95`);
  same caveat applies to any new `/data/...` file.
