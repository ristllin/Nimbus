<!-- audience: dev -->
# On-device tool catalog

The Orchestrator exposes one set of tools through an on-device MCP (Model
Context Protocol) server. This page is the reference for every tool: its
namespace, name, purpose, arguments, and current state. Use it when you need to
know what the model can call, what each call takes, and which calls are
admin-only or gated on a key.

**Single source of truth.** Every tool is registered on one `ToolRegistry`
(`lib/core/include/nimbus/orch/tool_registry.h`): `name` + one-line description
+ JSON input-schema + handler. Two consumers read that one registry, so they can
never drift:

- **The live orchestrator turn** - the tool list is pulled from
  `memory::registry().manifest()` and rendered into the composed World prompt's
  `## CAPABILITIES` section, so the LLM sees exactly what it can call. (See the
  turn-side dispatch in the [turn contract](turn-contract.md):
  `mem_write[]` → `memory.write`, `mem_query[]` → `memory.search`,
  `session_ops[]` → `session.*`.)
- **External MCP clients** - a JSON-RPC 2.0 client on the LAN reaches the same
  registry over `POST /mcp` (`handleMcp()` → `ToolRegistry::handleRpc()`,
  `src/agent/memory_subsystem.cpp`). Supported methods: `tools/list`,
  `tools/call`, `ping`. The endpoint requires the device token in the
  `X-Nimbus-Token` header; a token-authenticated client acts in its own
  namespace with user rights, never as the admin (see [MCP](../mcp.md)).

Both paths execute through the same `dispatch()`, so the capability manifest, the
prompt's tool list, and what actually runs are the same thing. Architecture:
[Orchestrator World](../orchestrator-world.md)
(§5 On-device MCP server & tool registry).

## The human-web-only rail

Secrets, provider keys, provider **priority/routing**, and the device's own auth
are **never tools** - no registry tool and no `device.config` action can set
them, and the protected-config denylist blocks them by name. Only the human web
UI may change them. Specifically off-limits to the model:

- provider keys (`oaiKey` / `antKey` / `mistralKey` / `custKey` / `tavilyKey` by
  name, plus `zaiKey`, `cumuloKey` and any other key ending in "Key" through a
  generic backstop in `isProtectedConfigKey`), `tgToken`, `tgAllow` - secrets,
  blocked entirely; only the owner changes them, in the
  web UI or over Telegram.
- provider `priority` / routing (`providerPriority` and its aliases),
  `orchHost` / `fabric` - a model that could rewrite these could redirect its
  own head or its sub-agent providers.
- `allowlist` - the device's own auth gate; the model must not grant itself
  access.
- the routines blob (`loops`) and the budget/governor knobs - only the
  dedicated `loop.*` tools and the human web UI write these.

The denial reports only a policy tag (`protected-BLOCKED`), **never** the blocked
value, so a secret can never leak into a device log or the next-turn context
(`lib/core/include/nimbus/orch/device_actions.h`, `kProtectedConfigKeys`). The
one deliberate exception is the `orch_model` device action: the model may
repoint its **own** head host + model at runtime.

## State legend

| Mark | Meaning |
|---|---|
| live | registered and executing on hardware today |
| wiring | schema/handler shape landed; device handler pending in a named phase |
| planned | not built yet |

**Access.** Every tool call carries a `Principal` - the caller's namespace,
role, and quota. Tools marked *admin-only* require an admin conversation (the
`manageTenants` permission) and additionally refuse in scheduled/unattended
turns; the rest are open to any approved conversation. Hiding a tool from the
advertisement is never the boundary - each handler re-checks the caller.

**Advertisement is principal-scoped.** An admin-only tool is not listed in a
member's or guest's prompt, so the model is never offered a capability the
handler would refuse, and a guest is not shown the owner's admin surface. The
registry carries an `adminOnly` mark (`ToolRegistry::setAdminOnly`) and the turn
engine advertises `toolSpecsFor(who)`. This is **visibility only**: the handler
check is unchanged, so a hidden tool reached another way (for example over
`/mcp`) still refuses.

## `memory.*` - associative & working memory

Registered by `registerMemoryTools()`
(`lib/core/src/orch_memory_tools.cpp`). Backed by the portable engines
(`VectorMemory`, `Scratchpad`, `MemConfig`, optional `EpisodicStore`) plus an
injected `Embedder` (text → int8 vector via the provider `/embeddings` API).

| Tool | Description (as registered) | Arguments | State |
|---|---|---|---|
| `memory.write` | Store a fact in long-term associative memory so you can recall it later. With `standing: true` an always/never rule for every reply is stored in the owner's standing rules instead (`[STANDING RULES]`, shown every turn). | `content` (string, **required**), `importance` (number, 0–1, default 0.5), `permanent` (boolean, default false), `standing` (boolean, default false: an every-reply rule), `ttl` (enum: `session` \| `days` \| `weeks` \| `months` \| `permanent`), `source` (string, default `"self"`) | live |
| `memory.search` | Search your long-term memory for facts relevant to a query. | `query` (string, **required**), `n_results` (integer, default = `cfg.retrievalCount`, i.e. 5) | live |
| `memory.update` | Replace a fact that changed: store new `content` and remove the closest stale match. | `content` (string, **required**), `old` (string) or `id` (string), `importance` (number), `permanent` (boolean), `ttl` (enum) | live |
| `memory.pin` | Pin a memory (never forgotten), unpin it, or delete it. A pin on an already pinned memory answers "already pinned, no change". Pinning stops decay; it does not put a memory into every prompt. | `action` (**required**, `pin` \| `unpin` \| `delete`), `content` (string) or `id` (string) | live |
| `memory.config` | View or tune how memory is retrieved. | `action` (**required**, `view` \| `update`); on `update`: `retrieval_count` (int), `relevance_threshold` (number), `decay_factor` (number), `max_context_bytes` (int), `max_vectors` (int), `recency_half_life_hours` (int), `mmr_lambda` (number) | live (update admin-only) |
| `memory.scratchpad` | Read or edit your working scratchpad (active task + short/mid/long-term goal tiers). | `action` (**required**, `view` \| `set_active` \| `add` \| `replace` \| `clear`), `tier` (`short` \| `mid` \| `long`), `text` (string), `items` (array of string) | live (admin-only) |
| `memory.episodic` | Search your episodic history by kind, session, text, or a time window. | `kind` (enum: `message`, `tool_output`, `llm_response`, `file`, `image`, `audio`, `transcript`, `log`), `session` (string), `text` (string), `limit` (int, clamped 1–100), `since_hours` (int), `before_hours` (int), `before` (paging cursor) | live (device only) |
| `memory.archive` | Search or restore memories that expired (reached their TTL) and were moved to the SD archive instead of being deleted. | `action` (**required**, `search` \| `restore` \| `list`); `query` (string), `id` (string), `n_results` (int), `limit` (int), `ttl` (enum, on restore) | live (SD only) |

Notes:
- `memory.write` derives a deterministic content id (djb2 hash), embeds the
  content, and inserts; a duplicate bumps importance instead of storing again.
- `memory.search` filters hits below `cfg.relevanceThreshold`
  (similarity = 1 − distance) and formats each as `- [NN%] content`.
- `memory.update` and `memory.pin` identify the target by exact content or by id.
- `memory.config` knobs are device-global (a member changing `max_vectors` would
  evict the admin's memory), so only an admin may `update`; viewing is open to
  any approved conversation.
- `memory.scratchpad` is injected into every turn's prompt, so it is admin-only
  until it is per-person. The model's normal way to write the scratchpad is the
  top-level `scratchpad` response field of the turn contract: a free write (no
  tool round) that persists across turns and restarts (see
  [turn-contract.md](turn-contract.md)). This tool remains as the admin
  read/edit path (`view` / `set_active` / `add` / `replace` / `clear`), also
  reachable over `/mcp`.
- `memory.episodic` reaches past the startup index - a cold scan over the full SD
  history - and answers one page at a time: when a result ends with a continue
  token, pass it back as `before` to read further into the past. Session
  `system` holds the device event timeline (restarts with reset reason, firmware
  updates with release notes, mode switches, storage changes). `text` finds rows
  containing **any** of its words (common words ignored, plural and verb endings
  trimmed); two or more words rank rows holding more of them first, and the
  answer says when more rows matched than the limit returned (the full rule:
  [orchestrator-world.md](../orchestrator-world.md) §1.3). Each row says when
  it happened: its date and hour (UTC) and its age, such as `16 h ago`, so a
  photo from yesterday never reads as today's; a row stamped before the device
  clock was set says the time is not recorded. Registered only when an episodic
  store is bound (device).
- `memory.archive` reaches the **cold store** of TTL-expired memories. Because
  re-embedding is expensive, a memory that reaches its time-to-live is moved to an
  archive on the SD card (embedding preserved) instead of being deleted, so it can be
  found or restored later. It is **SD-only**: registered only when a card is present,
  and every action refuses cleanly if the card is later pulled. `restore` brings an
  entry back into the live store and counts against the normal memory limit (it resets
  the entry's TTL, lifts its importance above the prune floor, and honors
  `max_vectors` / the pin budget - refusing without losing the entry if the live store
  is full). Archived entries are invisible to `memory.search`/recall; this is the only
  surface that reaches them. Everything stays inside the caller's namespace: a member
  searches or restores only its own archived memories. See
  [orchestrator-storage.md §2a](../orchestrator-storage.md). With no card, memories
  drop at their TTL exactly as before and this tool is absent.

## `session.*` - sub-agent control

Registered by `registerSessionTools()`
(`lib/core/src/orch_session_tools.cpp`). Each tool's device behavior depends on
its injected `SessionHandlers` handler; a missing handler makes the tool report
`"not supported on this device"`.

| Tool | Description (as registered) | Arguments | State |
|---|---|---|---|
| `session.spawn` | Start a sub-agent to work on a task in parallel. Returns its session id. | `task` (string, **required**), `provider` (string, optional), `model` (string, optional) | live |
| `session.list` | List your running sub-agents and their state. | (none) | live |
| `session.terminate` | Stop a running sub-agent. | `id` (string, **required**) | live |
| `session.tell` | Send a message to a running sub-agent, as if you were its user. | `id` (string, **required**), `message` (string, **required**) | wiring: adapter continuation |
| `session.poll` | Read the latest reply from a sub-agent (like reading its message to you). | `id` (string, **required**) | wiring: adapter continuation |

Notes:
- Sub-agents are started **only** via the spawn op; there is no separate
  `spawn[]` turn-contract array. The turn wire (`session_ops[]`) offers only the
  `spawn` and `terminate` ops; `tell`, `poll` and `list` are not on it, though
  they remain registry tools reachable over `/mcp`.
- The turn's `session_ops[]` **spawn** op accepts, beyond `task`/`provider`/`model`:
  - `skill` - an **approved** skill capsule id, injected into the sub-agent's brief.
  - `name` - a short display name (also the sub-agent's saved-document name).
  - `project` - a run tag: the sub-agent's full result **auto-saves** to the
    durable file store as `<project>/<name>-<tag>.md`. Use one project per
    fan-out run.
  - `attach` - up to 4 `<project>/<name>` documents whose full content the device
    splices into the sub-agent's instructions. It must be the JSON array field on
    the op; naming docs in the task text attaches nothing.
- `session.tell` / `session.poll` report the fire-and-forget boundary honestly
  today. Making them real needs adapter **continuation** - persisting each
  sub-session's conversation id (OpenAI `previous_response_id`, Anthropic
  multi-turn) so the orchestrator can drive a back-and-forth. The handler shape
  and schemas are in place; the fabric API is not yet exposed.

## `skill.*` - knowledge capsules (read + author)

Registered by the memory subsystem (`src/agent/memory_subsystem.cpp`; store in
`src/agent/skills.cpp`, built-in text in `lib/harness/src/builtin_skills.cpp`).
On-device markdown playbooks: six built-ins (`architecture`, `deep-research`,
`hardware`, `tools-guide`, `deliver-pdf`, `log-rows`) plus owner- or agent-authored capsules
on the SD card. A built-in names `reply.telegram` or `files.send` only on a turn
that was offered both; on any other turn it points at the Files list on the
device web page.

| Tool | Description (as registered) | Arguments | State |
|---|---|---|---|
| `skill.list` | List the skills (task playbooks and knowledge capsules) with their status: approved, waiting for the owner's approval, or an edit waiting on an approved one. | (none) | live |
| `skill.get` | Read a skill by id from the `[SKILLS]` index, before starting (or refusing) a task it covers. Once read it stays attached to the chat (`[ACTIVE SKILLS]`). | `id` (string, **required**) | live |
| `skill.save` | Save or update a skill. A new skill waits for the owner's approval; saving an approved skill proposes an **edit** (the approved version keeps running until the owner approves it). | `id` (string, **required**, `a-z0-9-_`, max 23 chars), `md` (string, **required** - the SKILL.md text; author keys: `title`, `desc`, `version`, `inject`) | live (admin-only) |
| `skill.delete` | Delete a skill capsule you authored (`created_by: agent`). | `id` (string, **required**) | live (admin-only) |

Notes:
- `skill.save` and `skill.delete` require an admin conversation and are refused
  in scheduled/unattended turns.
- A skill the model saves is server-stamped `created_by: agent`, `approved:
  false`, and stays **inert**: it can be read, but is never attached to a turn
  or injected into a sub-agent brief until the owner approves it - in the web UI
  (Assistant → Skills) or via Telegram `/skill approve <id>`.
- An edit of an approved skill lands in `SKILL.next.md`, never the live file:
  the approved revision keeps running until the owner approves the edit
  (`/skill approve`), discards it (`/skill deny`), or a later change makes it
  stale (approve then refuses it). The owner's own saves replace the live file
  at once (approved, the owner's, revision +1).
- The owner-origin guard: the assistant never changes who wrote a skill, never
  deletes an owner's skill, and never approves anything. With the owner's
  "the assistant may approve its own skill changes" switch on
  (`/skill auto on`), an attended admin save that read no outside content goes
  live at once and the owner is told.
- Reserved built-in ids cannot be shadowed by the assistant. At most 4 waiting
  skills plus proposed edits may pile up at once.
- Skills the model read in a chat stay attached to its next turns (at most 2 per
  chat, 3 h idle); approved skills with `inject: context` or `both` ride every
  turn in every chat. The `[SKILLS]` index names every skill and its one-line
  `desc:`; the assistant's history rows carry a `skills:` tag saying which
  skills a reply used.

## `prefs.*` - the owner's standing rules

Registered by `registerPrefsTools()` (`lib/core/src/orch_prefs_tools.cpp`) over
the portable `OwnerPrefs` store (`/data/prefs.txt`, zero NVS keys). The rules
ride the never-dropped part of every owner-facing prompt as `[STANDING RULES]`.

| Tool | Description (as registered) | Arguments | State |
|---|---|---|---|
| `prefs.edit` | Your owner's STANDING RULES: always/never rules they want in EVERY reply whatever the topic. Add one only when the owner asks in their own words; facts and topic-specific likes go to memory. | `action` (**required**, `add` \| `replace` \| `remove` \| `list`), `text` (string), `id` (string, e.g. `p3`) | live (owner/admin only) |

Notes:
- Owner-only: a member or guest turn never sees the tool or the rules.
- Refused in unattended turns (a routine, a wakeup, sub-agent synthesis, the
  dream) for every action except `list`.
- A turn that already read outside content can only add, and its rule waits as
  pending until the owner approves it (`/prefs approve <id>` or the web page).
- The owner sees every change in a line the device writes itself, over Telegram.
- A rule whose words ban em dashes also switches on a device-enforced filter
  that removes them from every delivered reply.

## `docs.*` - the device's own documentation

Registered by the memory subsystem (`src/agent/memory_subsystem.cpp`; portable
access layer in `lib/core/src/docs_pack.cpp`). A curated set of these docs -
capability, tool, hardware, and privacy references - is embedded in the firmware
image as a table of small sections, so the model can look up what it can do
instead of guessing. Read-only, open to every principal.

| Tool | Description (as registered) | Arguments | State |
|---|---|---|---|
| `docs.list` | Browse the documentation index - the doc files, or one file's sections. | `file` (string, optional - a file slug from the top-level listing) | live |
| `docs.search` | Ranked keyword match over section titles + bodies (any of the words; sections holding more of them first); returns ids + a snippet. | `query` (string, **required**) | live |
| `docs.read` | Read one section by id; an unknown id fails honestly with near-miss ids. | `id` (string, **required** - `<file-slug>#<heading-slug>`) | live |

Notes:
- The pack is **generated**: edit the markdown under `docs/`, then re-run
  `python3 tools/gen_docs_pack.py` (it rewrites
  `lib/core/include/nimbus/docs_pack_data.h`, which is committed). The pack
  ships inside the firmware image, so an OTA update carries matching docs -
  no SD sync, no version skew.
- Sections are split at build time (`##`/`###` headings, then paragraphs) so
  every `docs.read` result fits under the tool-loop's per-result clamp.

## `files.*` & `artifact.save` - durable artifact store

Registered by the files subsystem (`src/agent/files_subsystem.cpp`). Backed by
the durable artifact store on the SD card (`/mem/files/<project>/<name>`);
absent when no SD card is present. Reads honor per-person visibility (RBAC
`readableBy`).

| Tool | Description (as registered) | Arguments | State |
|---|---|---|---|
| `files.list` | List the device's durable artifact store: projects and files. | `project` (string, optional filter) | live |
| `files.stat` | Metadata for one stored file: size, kind, created time, provider file id. | `project` (**required**), `name` (**required**) | live |
| `files.read` | Read a stored TEXT document's content, paged; with `tail`, only the last rows of a log or CSV. | `project` (**required**), `name` (**required**), `offset` (number), `tail` (number of rows, max 200) | live |
| `files.search` | Search the CONTENT of saved text documents for words; returns the best matches with a snippet, ranked by how many of the words each document holds, then by how often they occur. | `query` (**required**), `project` (optional filter) | live |
| `files.append` | Add rows to the END of a text document (a log, a CSV); creates it when missing. Only the new rows are written. With `key`, a keyed CSV upsert: a row whose key matches an existing one UPDATES it instead of adding a duplicate. | `project` (**required**), `name` (**required**, a text extension), `text` (**required**, max 4 KB per call), `key` (optional, the column whose value identifies a row), `sum` (optional, a numeric column - the reply carries its total for today) | live |
| `files.edit` | Edit a saved text document surgically: find an exact text span (a phrase, or a whole line's text) and replace it - instead of reading and re-saving the whole file. Replaces the first match by default; `all: true` for every match, `count` for the first N. The reply says how many matches were found and replaced; a no-match changes nothing. | `project` (**required**), `name` (**required**, a text extension), `find` (**required**, max 2 KB), `replace` (optional, max 4 KB; empty deletes the span), `all` (boolean, default false), `count` (integer, default 1, max 200) | live |
| `artifact.save` | Persist a TEXT artifact (report, summary, document) into the durable store. | `project` (**required**), `name` (**required**, with extension `.md`/`.txt`/`.csv`/`.json`), `text` (**required**, max ~24 KB per call) | live |
| `files.share` | Share one of your saved files read-only with everyone, or stop sharing it. | `project` (**required**), `name` (**required**), `share` (boolean, default true) | live |
| `files.send` | Send a stored file to the owner on Telegram - images go as an inline photo, other files as a document. | `project` (**required**), `name` (**required**), `chat_id` (optional) | live |
| `files.pdf` | Write a PDF with the device's own writer from a title and light Markdown text (`#` headings, `-` bullets, `| table |` rows); text and simple tables only. Saves `<project>/<name>.pdf`, then deliver it with `files.send`. Owner only; needs an SD card. | `project` (**required**), `name` (**required**), `title`, `text` (**required**, up to 48 KB) | live |
| `files.fetch` | Download a document from an https URL into the file store, governed by the owner's URL-download trust policy (Off / Ask me per link / Scan, then keep / Full trust; web: Assistant → Safety → Downloads). Runs in the background; per-file size cap applies; under Ask me per link the owner approves each link (Telegram `/fetch approve <id>` or the web card); under Scan, then keep the file is quarantined and promoted only on a SAFE AI verdict. | `url`, `project`, `name` | live |
| `image.generate` | Generate an image from a text prompt and save it as a PNG in the durable store; returns the saved `project`/`name`. | `prompt` (**required**), `project` (default `images`), `name` (optional stem), `size` (optional), `quality` (`low`/`medium`/`high`, default `low`), `model` (optional, default `gpt-image-1`), `describe` (boolean, default false) | live (listed while an OpenAI key is set; a key added later works on the next turn) |
| `image.look` | Look again at a photo sent earlier and answer one question about it. A photo goes to the head as an image only in the turn it arrived; later turns read its short record, and this tool looks at the photo itself when the record does not answer. One provider call (the photo reader's provider walk); only during a turn; a caller that is not the owner looks only at photos of its own chat. | `ref` (**required**: the 16 hex characters in the photo's `[image ... ref=...]` label), `question` (**required**, up to 500 bytes) | live (needs an SD card) |

Notes:
- `files.read` serves text extensions only (`.md` / `.txt` / `.json` / `.csv` /
  `.log`); binary documents are refused (use `files.share` or the web viewer).
  It is **allowed in scheduled turns**.
- `artifact.save` overwrites the same `project`+`name`, refuses a write onto
  another person's file, and counts against the caller's file-storage quota.
- `files.append` never reads back or rewrites stored bytes: it writes only the
  new rows (line ends normalized, a final line end added). When the file's last
  row was cut mid-write, the new rows start on a fresh line and the result says
  so. It follows the same ownership and quota rules as `artifact.save`.
- `files.edit` changes only the matched spans; every other byte keeps its exact
  position, so a CSV's rows stay rows. It never creates a file (`not found` is
  the answer), never reads the file into the model's context, and streams the
  substitution on the device into a temp sibling that is renamed over the
  original only after it is fully written and size-verified - any refusal (a
  cap, no match, quota, a short write) leaves the stored file untouched. It
  follows the same ownership, lock and quota rules as `artifact.save` (an
  edit's growth counts against the caller's file-storage quota).
- The keyed upsert (a `key` column) is the one exception to "never rewrites":
  the merge happens on the device, so only the new rows cross the model's
  context - never a whole-file read plus a whole-file save. The reply names how
  many rows were updated and appended, and - with `sum` - the day's running
  total, so a log update never needs a second read. `files.read {tail}` takes
  the same `sum` argument for the same total.
- `files.read` with `tail` reads only the end of the file (about 7.5 KB) and
  returns the last complete rows, oldest first, plus a CSV's header row when it
  is not among them; an incomplete last row is reported, not hidden.
- `files.share` is owner-controlled and read-only; guests cannot share.
- `files.send` is a turn-output tool - refused from the LAN `/mcp` path. A file
  another person owns is invisible: the refusal deliberately matches "not found"
  so its existence is not disclosed. The call answers "queued <project>/<name> for
  Telegram delivery"; the upload runs on the next Telegram poll, and if it fails the chat
  gets "Couldn't send <name>. It's saved under Files."
- `image.generate` uses OpenAI **gpt-image-1**, which returns the image inline
  (the dall-e models hand back a URL the device cannot re-fetch). The image is
  decoded in PSRAM during the download and written to the SD card under a brief
  lock, so a long download never holds the internal heap or the SD bus. It is a
  **turn-only** tool (like `files.send`): a call can take 1-2 minutes, so it
  runs on the turn task and is refused on the `/mcp` path. `quality` defaults to
  **low** (fast, small, cheap). It needs an SD card and an OpenAI key. The saved PNG is a first-class `Image` artifact:
  `files.list`/`files.search`/`files.send` all see it, and `files.send` delivers it
  as an inline Telegram photo. `describe:true` adds a vision read-back (a second
  provider call). Not available on this device: embedding images into PDFs,
  email attachments, and multimodal vision beyond that one read-back; a
  generated or received image enters the conversation as a description, not as
  pixels.

## `results.*` - recent tool outputs

Registered by `registerResultTools()` (`lib/core/src/orch_result_store.cpp`).
A bounded ring of recent tool outputs and overflowed sub-agent results, so a
truncated payload can be read back in full.

| Tool | Description (as registered) | Arguments | State |
|---|---|---|---|
| `results.get` | Fetch the full text of a stored result by tag, paged. | `tag` (string, **required**), `offset` (number) | live |
| `results.list` | List the stored recent results (tag, kind, name, size). | (none) | live |

Note: a tag that has aged out of the ring is not an error to retry - fall back to
`memory.episodic` with the tag as `text`.

## `loop.*` - routines (scheduled turns)

Registered by the memory subsystem. Scheduled or recurring tasks ("routines")
that fire as future Orchestrator turns.

| Tool | Description (as registered) | Arguments | State |
|---|---|---|---|
| `wakeup.set` | Schedule ONE future wakeup for the assistant itself - no owner approval (a bounded single fire under the same daily governor as routines; at most 4 armed; chaining allowed). In `minutes` (2 min–7 days) it gets a single automatic turn carrying its `note`, then the wakeup retires (one short retry if the turn itself failed before it reached the owner, then an owner alert; a turn that failed after its message reached the owner is not repeated, and the owner is told). Visible in /loops and web Routines; cancel with `loop.cancel`. | `minutes` (**required**), `note` (**required**) | live (admin-only) |
| `loop.create` | Schedule a recurring or timed task that fires as a future turn. | `name` (**required**), `prompt` (**required**), `chat_id` (optional; defaults to the chat it is created from), `schedule` (**required** object: `{kind:"interval", every_seconds: N≥300}` or `{kind:"daily"\|"weekly", at:"HH:MM", days:[...]}`) | live (admin-only) |
| `loop.list` | List all routines with schedule, next run, last result, enabled, and approval. | (none) | live (admin-only) |
| `loop.cancel` | Cancel (delete) a routine by its id. | `id` (**required**) | live (admin-only) |

Notes:
- All four require an admin conversation. `loop.create` is additionally refused
  in scheduled turns (a routine cannot create routines).
- A routine the model creates is saved **pending** the owner's approval and
  cannot fire until approved (web Routines tab, or Telegram `/loop approve <id>`).
- Routines, reminders, and wakeups deliver to the chat they were created from (the
  web chat, a Telegram chat, or the device screen for a hold-to-talk request);
  `chat_id` overrides that. One with no remembered chat goes to the owner's Telegram
  when it is set up, otherwise to the web chat. If the bot is turned off when the
  owner's own Telegram routine fires, it shows in the web chat and the reply says so;
  a routine aimed at a chat that is no longer allowed pauses instead. An open web
  chat shows a routine's reply within about 30 seconds, without a reload, and never
  as the answer to the owner's next message.

## `reply.*` - output channels

Registered by the memory subsystem. Turn-output tools: the model chooses how to
reach the person. Both refuse outside a live turn.

| Tool | Description (as registered) | Arguments | State |
|---|---|---|---|
| `reply.speak` | Read text aloud on the device speaker (text-to-speech). | `text` (**required**) | live |
| `reply.telegram` | Send a Telegram message; `voice:true` delivers it as spoken audio, as Telegram voice messages. | `text` (**required**), `chat_id` (optional), `voice` (boolean) | live |

Note: `reply.speak` degrades honestly when voice replies are switched off or the
speaker volume is 0, refuses text over 1,200 characters, and reports success only
once the speaker started playing; otherwise it says why ("could not play on the
speaker: the audio driver could not start ..."). With voice replies off,
`reply.telegram` `voice:true` falls back to text - the content is never dropped.
A Telegram voice reply always goes to Telegram, never to the speaker: text of up to
2,400 characters is split into voice messages of up to 600 characters, each one
synthesized, uploaded at once and checked against Telegram's answer. The result names
Telegram's `message_id` for every voice message that arrived ("delivered to Telegram
as a voice message (message_id 812, 210 KB)"); when one did not, it says why and that
nothing (or only some) was delivered, so the reply can fall back to text. `reply.telegram` is
offered only on a turn that can deliver over Telegram: the bot is running and
either a chat is allowlisted, or public mode is on and the message itself came in
over Telegram. `files.send` delivers only to an allowlisted chat, so it is offered
only when one exists: a guest's Telegram turn in public mode with no allowlisted
chat gets `reply.telegram` (its own chat) but not `files.send`. A web, voice or
serial turn in public mode with no allowlisted chat gets neither, and a reminder
or wakeup answers in the chat that set it. The device decides this once per turn,
so the tool list and the turn's hints agree: on a turn without `files.send` the
built-in playbooks (`skill.get`), the capability line and the `image.generate`
result point at the Files list on the device web page instead of naming it.

## `web.*` - live web search

| Tool | Description (as registered) | Arguments | State |
|---|---|---|---|
| `web.search` | Search the live web; returns an answer plus top results (title, url, snippet). | `query` (string, **required**), `max_results` (integer, default 5) | live (when a web-search key is set) |

Note: always registered, and listed (to the model, in `/api/tools` and in MCP
`tools/list`) exactly while a web-search (Tavily) key is configured. A key added
in the web app takes effect on the next turn, no restart; without one a call
refuses and says the key is missing. The key is human-set, never
model-writable. A monthly call budget can gate it. The provider-gated tools are
one table, `kGatedTools` in `lib/core/include/nimbus/orch/tool_gates.h`.

## `models.list` - the provider model lists

Registered by `src/agent/model_refresh.cpp` (the portable tool is
`lib/core/src/orch_models_tool.cpp`). The head asks which models a provider
offers instead of reading every provider's list in every prompt: when the
config's tool loop is on and the tool is registered for the head, the prompt's
`[AVAILABLE MODELS]` block is one line naming the providers with a key and
pointing here - a config-keyed shape, so the cached block is byte-stable across
turns (a head without the tool loop keeps the lists; when low memory gates the
loop off for one turn, the pointer stays and that turn's loop-off note says the
tool cannot run either).

| Tool | Description (as registered) | Arguments | State |
|---|---|---|---|
| `models.list` | List the models a provider offers, from the device's cached list: the id (what `orch_model` and a spawn take), name, line, size, release date, context, lifecycle, price per 1M tokens with its source, and when the list was fetched. At most 20 rows; narrow with provider, query or line. `refresh:true` also queues a fresh fetch from the provider for after this turn (at most once an hour per provider); this call still answers from the cache. | `provider` (string: `cumulo`, `openai`, `anthropic`, `mistral` or `zai`; omit for every provider with a key), `query` (part of an id or name), `line` (a model line), `role` (`orchestrator`, `sub-agent`, `vision`, `embedding`, `stt`, `tts` or `image`; default the chat models), `include_legacy` (boolean), `refresh` (boolean) | live |

The answer is one JSON object: `providers[]` (`provider`, `fetchedAt` as a UTC
minute or null, `source` `fetched` or `built-in`, `total` matching rows,
`refresh` `queued` / `pending` / `recent` or null) and `models[]` (`provider`,
`id`, `name`, `line`, `size`, `released`, `ctx`, `lifecycle`, `price` `{in, out,
source}`), every field present and null when unknown, plus `more` (matching rows
not shown). With several providers the rows are taken in turn, so one long list
(the router's) cannot crowd out the rest. Router ids are written
`<upstream>/<model>` and appear only under `cumulo`; the other providers list bare
ids. Every id shown is one the `orch_model` and spawn checks accept.

The tool never opens a connection. `refresh:true` only queues: the device runs the
refresh between turns, one provider at a time, on the verify task, and the next
`models.list` reads the new list. The same queue serves an `orch_model` that names
an id the cached list does not hold (the answer is `not in <provider> list cached
<date>; refresh queued`), the owner opening a model dropdown in the web app, and a
passive refresh 10 minutes after boot and then every 6 hours, for a list that is
missing, only the older build's lists merged at the update, a week old, or written by
another firmware version. Whoever asks, a provider is refreshed at most once an hour;
a refresh that ends with the list still due (no answer, a list cut short, a failed
write) is tried again after 2 minutes, doubling up to an hour, six times, then left to
the next passive pass. A refresh needs only the memory one connection needs (the same
floor as a turn request); when its own worker cannot start on a fragmented memory, it
runs on the Telegram task between messages instead, within 15 seconds in all (one page,
no extra connections; a list it cannot finish in that time is tried again later). There it
gives way to a waiting owner message (a web chat, voice or serial message queued, or a
Telegram album still arriving): it does not start, or stops before reading the list, and
tries again 30 seconds later without counting a try. A Z.ai key with no stored host is
asked on api.z.ai only there, so a refusal from it reads "couldn't verify", never
rejected (the key may belong to open.bigmodel.cn). A key verify that low memory deferred
asks for the list on its first deferral (at once for a key not verified yet), then within
the hourly limit, six times at most; when the list answers, the verify's retries stop, and
the full verify (the selected models' check, the Cumulo and Mistral syncs) runs once memory
allows it. A provider paused by a spend limit or parked by a refusal is still refreshed:
the list request is free.

## `tenant.*` - people & roles

Registered by the memory subsystem. Manage who may talk to the device and what
they may store. Admin-only, and refused in scheduled turns.

| Tool | Description (as registered) | Arguments | State |
|---|---|---|---|
| `tenant.list` | List the people who can talk to this device: chat id, role, quotas. | (none) | live (admin-only) |
| `tenant.set_role` | Approve someone or change their role: `admin` \| `user` \| `guest` \| `unknown` (revokes access). | `chat` (**required**), `role` (**required**, enum) | live (admin-only) |
| `tenant.set_quota` | Set one person's storage limits. | `chat` (**required**), `vectors` (int), `bytes` (int), `ttl_hours` (int), `pins` (int) - 0 restores the role default | live (admin-only) |

Note: the last admin cannot be demoted. Roles and quotas are the model-callable
half of the people surface; the auth allow-list itself stays human-only (see the
rail above). See [People & privacy](../people-and-privacy.md).

## Device & system introspection

Registry tools that report live device state, plus `device.control`, which
applies one device action mid-turn. Only `device.control` and `device.selftest`'s
audible items actuate hardware. Registered in `src/main.cpp`
(`registerDeviceTools`: `device.status`, `device.selftest`, `ota.status`),
`src/agent/memory_subsystem.cpp` (`system.health`) and `src/agent/orchestrator.cpp`
(`device.control`).

| Tool | Description (as registered) | Arguments | State |
|---|---|---|---|
| `system.health` | Report live hardware + subsystem health (ring, display, mic, speaker, SD, memory, PSRAM, Wi-Fi, Telegram). | (none) | live |
| `device.status` | Read the complete live device state: firmware/build, update engine + slot + auto-update, local time + timezone + clock-sync, free heap/PSRAM, SD (including live `sdLost`) + internal flash usage, battery, Wi-Fi signal, Bluetooth bonds, fault mask, sound tier + sound-pack sync, ring posture + battery mode (user and effective) + attention hold + device name + sub-agent provider priority, memory counts vs live caps (vectors, episodic, scratchpad), file-store count/bytes/free, settings-storage health (`nvs.free`, `nvs.fails` saves that failed since boot, `nvs.lastFail` the last failing key name, never a value; `tgOffsetSaved` false means the device owes a Telegram offset write, so messages can arrive again, a device-side repeat), sub-agent capacity (running/queued vs limits), real billed token usage (last turn + session) including `usage.budget[]` - per-provider spend this billing period (tokens in/out, calls, estimated dollars with each model at its own price, and `approx` when part of that estimate is a built-in price) against the owner's dollar and token ceilings plus the reset day (v4.1.2: plan work against the budget instead of discovering a refusal at dispatch), the current value of every config knob (v4.1: the knobs the model can set are all readable here), and `lastFail` - the most recent failed turn since boot (`turn`, `src`, `agoS`, `cause` with the first cause first; `null` when none; RAM only). | (none) | live |
| `device.selftest` | Run a hardware self-test; returns per-item PASS/FAIL/SKIP. | `audible` (boolean - also runs the speaker/mic acoustic loopback) | live |
| `device.control` | Apply ONE device action immediately, mid-turn: `action` is one `device[]` element (`config` / `led` / `lights` / `tts`), so a sequenced flow (read a knob, set it, speak, restore it) completes inside one turn. Same knobs, validator, and policy rails as `device[]`; changes are real and persist. `reboot` is refused here (put it in the end-of-turn `device[]`). | `action` (object) | live |
| `ota.status` | Read the firmware-update engine: state, latest version seen + its release notes, last install outcome. | (none) | live |

Note: `device.status` is silent (never makes a sound); the battery reading is a
voltage **trend** only - the device has no charge-detect hardware.
`device.selftest`'s audible items are refused when the device is on silent or the
owner disabled hardware tests. The model cannot **install** updates - only the
owner can (Telegram `/update` or the web UI).

## `device[]` - physical device actions (turn array)

Device actions are **not** registry tools - they arrive as the turn's
`device[]` array and are validated by the portable half in
`lib/core/include/nimbus/orch/device_actions.h`
(`validateAction()`), then executed by `applyDeviceElement()`
(`lib/harness/src/apply.cpp`) only for items with `allowed == true`; `led`,
`lights` and `reboot` are staged to the main loop through `orchDeviceSink` in
`src/main.cpp`. Each `device[]` element is exactly one of:

| Action | What it does | Params | State |
|---|---|---|---|
| `led` | ring pattern + color + brightness | `mode` (`solid` \| `spinner` \| `pulse` \| `flash` \| `rainbow`), color `r`/`g`/`b` (0–255), `brightness` (0–255 or null) | live |
| `lights` | ring on/off shorthand | `value` (`off` \| `full`) | live |
| `config` | benign, owner-friendly knobs (allow-listed keys only) | e.g. `ledBrightness`, `priority` (routing *preference*), `posture`, `profile`, `theme`, `attnHoldMs`, `sfxLvlN`/`sfxLvlO`, `sfxVol`, `sfxTheme`, `sttProv`, `ttsProv`, `ttsVoice`, `ttsOn`, `sleepOvr`, `brightOvr`, `devName` | live (rails enforced) |
| `orch_model` | switch the head's own provider + model at runtime | `provider` (`openai` \| `anthropic` \| `mistral` \| `zai` \| `cumulo`), `model` (an id that provider lists in `models.list`) | live |
| `reboot` | restart the device | (none) | live |
| `tts` | speak text aloud on the device speaker (Telegram audio fallback) | `text` | live |

Rails: a `config` action carrying any protected key (secrets, the auth
allow-list, provider routing/priority, the routines blob (`loops`) or the budget governors - see
the human-web-only rail above) is refused as a **whole** with reason
`protected-BLOCKED`. `orch_model` is the one deliberate exception to the routing
rule: the model may repoint its own head host + model. A refused switch says why,
in the mid-turn `device.control` answer and in the next turn's `[MEMORY RESULTS]`:
an id the cached list does not hold answers `not in <provider> list cached <date>;
refresh queued`, and a router id (`<upstream>/<model>`) named on a direct provider
answers `not in <provider> list: use <bare id> (<id> is a cumulo router id)` when
that provider lists the bare id, else names the id as a cumulo router id. A switch
away from a head provider that is paused (a monthly or overall spending limit, or
paused by the owner), out of credit, refusing its key or over its budget is refused
too: the reply carries "Anthropic is paused by your monthly limit; replies already
fail over automatically and return to Anthropic when it lifts. To make Mistral your
main provider anyway, say "switch to Mistral" again, or pick it under Assistant >
Models." (The ending follows the cause: "once credit is added", "once its key is
replaced".) Only an owner message that asks for that switch (it names the provider or
model, uses a word such as switch, use or change, and does not say keep or don't)
counts: such a refusal can be confirmed by the owner's next such message within 30
minutes. A refusal the assistant drew on its own leaves nothing to confirm, and a turn
the owner did not write (a member, a guest, a routine) never confirms one and is told
only that the head stays. Session ops (spawn /
terminate) are **not** device actions - they go through the turn's
`session_ops[]` / `session.*`, never `device[]`.

On-device speech plays through the built-in speaker. Both the `reply.speak` tool
(mid-turn) and the end-of-turn `device[]` `tts` action synthesize the reply and
play it on the speaker; `tts` falls back to a Telegram voice message when the
speaker could not play it. A spoken Telegram message is also available via
`reply.telegram` `voice:true`, which never uses the speaker. The speaker plays WAV directly and decodes MP3 with
the bundled decoder, so the reply speaks whether the configured voice provider
emits WAV (OpenAI) or MP3 (Mistral).

## Planned surfaces

| Namespace | Tool | What it will do | State |
|---|---|---|---|
| audio | `audio.transcribe` | mic → STT text as a model-callable tool | planned |

Inbound audio is already transcribed by the Telegram/voice pipeline, and Telegram
media **send** is already exposed as tools - spoken audio via `reply.telegram`
(`voice:true`) and documents via `files.send` - so only a model-callable
mic-transcription tool remains planned.

## Adding tools

Beyond the on-device set, capabilities can be extended two ways, both surfaced
honestly in the web "Tools" panel (`GET /api/tools`, read live from
`memory::registry().manifest()`):

- **External MCP clients** register/reach tools over `POST /mcp`.
- **Provider-side tools** (e.g. hosted web-search / deep-research) run on the
  spawn target, not on the device.

Each row in that panel also carries a **where-it-runs** tag, so the live surface
shows not just what exists but who can reach it:

- **Orchestrator** - the head runs it directly (the registry tools and the
  turn-contract device actions; a connector on the current host provider).
- **Sub-agents** - reachable only by spawning a sub-agent on that provider (a
  connector on a provider that is not the current head).
- **Unavailable** - not usable now: the provider is unkeyed, the connector is
  turned off, or its sign-in failed.

The tag is computed from the same host-vs-spawn policy the model's
`[PROVIDERS & CONNECTORS]` block states in prose - one rule, `connectorScope()`
in `lib/core/src/orch_connectors_wire.cpp`, host-tested so the two never drift.

## See also

- [Orchestrator World](../orchestrator-world.md)
  - the World memory system and the MCP server (§5).
- [Turn contract](turn-contract.md) - the single-shot turn JSON
  and how `mem_write[]` / `mem_query[]` / `session_ops[]` dispatch to these tools.
- [People & privacy](../people-and-privacy.md) - roles, quotas, and the RBAC
  gating the `tenant.*` and admin-only tools enforce.
