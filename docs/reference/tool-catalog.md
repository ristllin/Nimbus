<!-- audience: dev -->
# On-device tool catalog

The Nimbus Orchestrator exposes a single, authoritative set of tools through an
on-device MCP (Model Context Protocol) server. This is the reference for every
tool: its namespace, name, one-line purpose, arguments, and current state.

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
  `tools/call`, `ping`. LAN MCP auth is an open item.

Both paths execute through the same `dispatch()`, so the capability manifest, the
prompt's tool list, and what actually runs are the same thing. Architecture:
[Orchestrator World](../orchestrator-world.md)
(§5 On-device MCP server & tool registry).

## The human-web-only rail

Secrets, provider keys, provider **priority/routing**, and the device's own auth
are **never tools** - no registry tool and no `device.config` action can set
them, and the protected-config denylist blocks them by name. Only the human web
UI may change them. Specifically off-limits to the model:

- provider keys (`oaiKey` / `antKey` / `mistralKey` / `tavilyKey`), `tgToken`,
  `tgAllow` - SECRETS, blocked entirely; changing them needs owner confirmation
  via the web UI / Telegram.
- provider `priority` / routing (`providerPriority` and its aliases),
  `orchHost` / `fabric` - letting the model rewrite these would let it redirect
  its own brain / sub-agent backends.
- `allowlist` - the device's own auth gate; the model must not grant itself
  access.
- the Local Loops blob (`loops`) and the budget/governor knobs - only the
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

**Advertisement is principal-scoped (v4.1.0).** An admin-only tool is not
listed in a member's or guest's prompt: previously every conversation was told
the assistant could call `skill.save`, `tenant.set_role` or `loop.create`, so it
offered a capability the handler then refused (a wasted round and a confusing
walk-back) and described the owner's admin surface to a guest. The registry
carries an `adminOnly` mark (`ToolRegistry::setAdminOnly`) and the turn engine
advertises `toolSpecsFor(who)`, restoring *advertised == callable*. This is
**visibility only** - the handler check is unchanged, so a hidden tool reached
another way (for example over `/mcp`) still refuses exactly as before.

## `memory.*` - associative & working memory

Registered by `registerMemoryTools()`
(`lib/core/src/orch_memory_tools.cpp`). Backed by the portable engines
(`VectorMemory`, `Scratchpad`, `MemConfig`, optional `EpisodicStore`) plus an
injected `Embedder` (text → int8 vector via the provider `/embeddings` API).

| Tool | Description (as registered) | Arguments | State |
|---|---|---|---|
| `memory.write` | Store a fact in long-term associative memory so you can recall it later. | `content` (string, **required**), `importance` (number, 0–1, default 0.5), `permanent` (boolean, default false), `ttl` (enum: `session` \| `days` \| `weeks` \| `months` \| `permanent`), `source` (string, default `"self"`) | live |
| `memory.search` | Search your long-term memory for facts relevant to a query. | `query` (string, **required**), `n_results` (integer, default = `cfg.retrievalCount`, i.e. 5) | live |
| `memory.update` | Replace a fact that changed: store new `content` and remove the closest stale match. | `content` (string, **required**), `old` (string) or `id` (string), `importance` (number), `permanent` (boolean), `ttl` (enum) | live |
| `memory.pin` | Pin a memory (never forgotten), unpin it, or delete it. | `action` (**required**, `pin` \| `unpin` \| `delete`), `content` (string) or `id` (string) | live |
| `memory.config` | View or tune how memory is retrieved. | `action` (**required**, `view` \| `update`); on `update`: `retrieval_count` (int), `relevance_threshold` (number), `decay_factor` (number), `max_context_bytes` (int), `max_vectors` (int), `recency_half_life_hours` (int), `mmr_lambda` (number) | live (update admin-only) |
| `memory.scratchpad` | Read or edit your working scratchpad (active task + short/mid/long-term goal tiers). | `action` (**required**, `view` \| `set_active` \| `add` \| `replace` \| `clear`), `tier` (`short` \| `mid` \| `long`), `text` (string), `items` (array of string) | live (admin-only) |
| `memory.episodic` | Search your episodic history by kind, session, text, or a time window. | `kind` (enum: `message`, `tool_output`, `llm_response`, `file`, `image`, `audio`, `transcript`, `log`), `session` (string), `text` (string), `limit` (int, clamped 1–100), `since_hours` (int), `before_hours` (int), `before` (paging cursor) | live (device only) |

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
  until it is per-person. **Since v4.1.0 the model's normal way to write the
  scratchpad is the top-level `scratchpad` response field of the turn contract**
  - a free write (no tool round), persistent across turns and reboots (see
  [turn-contract.md](turn-contract.md)). This tool remains as the admin
  read/edit path (`view` / `set_active` / `add` / `replace` / `clear`), also
  reachable over `/mcp`.
- `memory.episodic` reaches past the startup index - a cold scan over the full SD
  history - and answers one page at a time: when a result ends with a continue
  token, pass it back as `before` to read further into the past. Session
  `system` holds the device event timeline (restarts with reset reason, firmware
  updates with release notes, mode switches, storage changes). Registered only
  when an episodic store is bound (device).

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
  `spawn` and `terminate` ops - `tell`/`poll`/`list` are not on it (they were
  advertised before and hard-failed at runtime), though they remain registry
  tools reachable over `/mcp`.
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
`src/agent/skills.cpp`). On-device markdown playbooks: four built-ins
(`architecture`, `deep-research`, `hardware`, `tools-guide`) plus owner- or
agent-authored capsules on the SD card.

| Tool | Description (as registered) | Arguments | State |
|---|---|---|---|
| `skill.list` | List the on-device knowledge capsules you can read (id + title). | (none) | live |
| `skill.get` | Read a knowledge capsule by id (from `skill.list`). | `id` (string, **required**) | live |
| `skill.save` | Save (or update) a reusable skill capsule you authored. | `id` (string, **required**, `a-z0-9-_`, max 23 chars), `md` (string, **required** - the SKILL.md text, optional `title:` / `inject:` front matter) | live (admin-only) |
| `skill.delete` | Delete a skill capsule you authored (`created_by: agent`). | `id` (string, **required**) | live (admin-only) |

Notes:
- `skill.save` and `skill.delete` require an admin conversation and are refused
  in scheduled/unattended turns.
- A capsule the model saves is server-stamped `created_by: agent`,
  `approved: false`, and stays **inert**: it can be read, but is never injected
  into a sub-agent brief until the owner approves it - in the web UI
  (Assistant → Skills) or via Telegram `/skill approve <id>`. Approval is
  **asynchronous**: save a skill as an investment for future runs, never as a
  step of the current task.
- Reserved built-in ids cannot be shadowed. At most 4 agent-authored capsules
  may await approval at once.
- `skill.delete` removes only agent-authored capsules; owner-created ones are
  deleted from the web UI.

## `docs.*` - the device's own documentation

Registered by the memory subsystem (`src/agent/memory_subsystem.cpp`; portable
access layer in `lib/core/src/docs_pack.cpp`). A curated set of these docs -
capability, tool, hardware, and privacy references - is embedded in the firmware
image as a table of small sections, so the model can look up what it can do
instead of guessing. Read-only, open to every principal.

| Tool | Description (as registered) | Arguments | State |
|---|---|---|---|
| `docs.list` | Browse the documentation index - the doc files, or one file's sections. | `file` (string, optional - a file slug from the top-level listing) | live |
| `docs.search` | Keyword AND-match over section titles + bodies; returns ids + a snippet. | `query` (string, **required**) | live |
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
| `files.read` | Read a stored TEXT document's content, paged. | `project` (**required**), `name` (**required**), `offset` (number) | live |
| `files.search` | Search the CONTENT of saved text documents for words; returns the best matches with a snippet, ranked by term frequency. | `query` (**required**), `project` (optional filter) | live |
| `artifact.save` | Persist a TEXT artifact (report, summary, document) into the durable store. | `project` (**required**), `name` (**required**, with extension `.md`/`.txt`/`.csv`/`.json`), `text` (**required**, max ~24 KB per call) | live |
| `files.share` | Share one of your saved files read-only with everyone, or stop sharing it. | `project` (**required**), `name` (**required**), `share` (boolean, default true) | live |
| `files.send` | Send a stored file to the owner on Telegram - images go as an inline photo, other files as a document. | `project` (**required**), `name` (**required**), `chat_id` (optional) | live |
| `image.generate` | Generate an image from a text prompt and save it as a PNG in the durable store; returns the saved `project`/`name`. | `prompt` (**required**), `project` (default `images`), `name` (optional stem), `size` (optional), `quality` (`low`/`medium`/`high`, default `low`), `model` (optional, default `gpt-image-1`), `describe` (boolean, default false) | live |

Notes:
- `files.read` serves text extensions only (`.md` / `.txt` / `.json` / `.csv` /
  `.log`); binary documents are refused (use `files.share` or the web viewer).
  It is **allowed in scheduled turns**.
- `artifact.save` overwrites the same `project`+`name`, refuses a write onto
  another person's file, and counts against the caller's file-storage quota.
- `files.share` is owner-controlled and read-only; guests cannot share.
- `files.send` is a turn-output tool - refused from the LAN `/mcp` path. A file
  another person owns is invisible: the refusal deliberately matches "not found"
  so its existence is not disclosed.
- `image.generate` uses OpenAI **gpt-image-1** (it returns base64 natively;
  dall-e-* need a `response_format` param the current API rejects, or hand back a URL
  a one-TLS-slot device can't re-fetch). The whole image decodes into **PSRAM**
  during the download, then lands on SD under a brief lock - it never holds the
  internal heap, and never the SD bus for the long TLS wait (which would either trip
  the main-loop watchdog or race other SD users; both were seen live). It is a
  **turn-only** tool (like `files.send`): it blocks ~1-2 min, so it runs on the turn
  task, never the AsyncTCP `/mcp` path. `quality` defaults to **low** (fast, small,
  cheap); SD- and OpenAI-key-gated. The saved PNG is a first-class `Image` artifact:
  `files.list`/`files.search`/`files.send` all see it, and `files.send` delivers it
  as an inline Telegram photo. `describe:true` adds a vision read-back (a second
  provider call). **Hardware-verified** end-to-end on-device (a 1024×1024
  gpt-image-1 PNG generated → saved intact → delivered). **Out of scope on this
  device:** embedding images into PDFs (no PDF engine), email attachments (no
  outbound email send exists), and in-turn multimodal vision beyond describe-once - a
  generated or received image enters the conversation as a *description*, not pixels,
  for the heap reasons in `image_vision.h`.

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
| `wakeup.set` | Schedule ONE future wakeup for the assistant itself - no owner approval (a bounded single fire under the same daily governor as routines; at most 4 armed; chaining allowed). In `minutes` (2 min–7 days) it gets a single automatic turn carrying its `note`, then the wakeup retires (one short retry if the turn itself failed, then an owner alert). Visible in /loops and web Routines; cancel with `loop.cancel`. | `minutes` (**required**), `note` (**required**) | live (admin-only) |
| `loop.create` | Schedule a recurring or timed task that fires as a future turn. | `name` (**required**), `prompt` (**required**), `chat_id` (optional, must be allow-listed), `schedule` (**required** object: `{kind:"interval", every_seconds: N≥300}` or `{kind:"daily"\|"weekly", at:"HH:MM", days:[...]}`) | live (admin-only) |
| `loop.list` | List all routines with schedule, next run, last result, enabled, and approval. | (none) | live (admin-only) |
| `loop.cancel` | Cancel (delete) a routine by its id. | `id` (**required**) | live (admin-only) |

Notes:
- All three require an admin conversation. `loop.create` is additionally refused
  in scheduled turns (a routine cannot create routines).
- A routine the model creates is saved **pending** the owner's approval and
  cannot fire until approved (web Routines tab, or Telegram `/loop approve <id>`).

## `reply.*` - output channels

Registered by the memory subsystem. Turn-output tools: the model chooses how to
reach the person. Both refuse outside a live turn.

| Tool | Description (as registered) | Arguments | State |
|---|---|---|---|
| `reply.speak` | Read text aloud on the device speaker (text-to-speech). | `text` (**required**) | live |
| `reply.telegram` | Send a Telegram message; `voice:true` delivers it as spoken audio. | `text` (**required**), `chat_id` (optional), `voice` (boolean) | live |

Note: `reply.speak` degrades honestly when voice replies are switched off or the
speaker volume is 0. With voice replies off, `reply.telegram` `voice:true` falls
back to text - the content is never dropped.

## `web.*` - live web search

| Tool | Description (as registered) | Arguments | State |
|---|---|---|---|
| `web.search` | Search the live web; returns an answer plus top results (title, url, snippet). | `query` (string, **required**), `max_results` (integer, default 5) | live (when a web-search key is set) |

Note: registered only when a web-search (Tavily) key is configured - the key is
human-set, never model-writable. A monthly call budget can gate it.

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

Read-only registry tools that report live state; none mutate durable state or
(except `device.selftest`'s audible items) actuate hardware. Registered in
`src/main.cpp` (`registerDeviceTools`) and the memory subsystem.

| Tool | Description (as registered) | Arguments | State |
|---|---|---|---|
| `system.health` | Report live hardware + subsystem health (ring, display, mic, speaker, SD, memory, PSRAM, Wi-Fi, Telegram). | (none) | live |
| `files.fetch` | Download a document from an https URL into the file store, governed by the owner's URL-download trust policy (off / ask-per-link / AI scan / full trust; web: Assistant → Safety → Downloads). Runs in the background; per-file size cap applies; under "ask" the owner approves each link (Telegram `/fetch approve <id>` or the web card); under "scan" the file is quarantined and promoted only on a SAFE AI verdict. | `url`, `project`, `name` | live |
| `device.status` | Read the complete live device state: firmware/build, update engine + slot + auto-update, local time + timezone + clock-sync, free heap/PSRAM, SD (including live `sdLost`) + internal flash usage, battery, Wi-Fi signal, Bluetooth bonds, fault mask, sound tier + sound-pack sync, ring posture + battery mode (user and effective) + attention hold + device name + sub-agent provider priority, memory counts vs live caps (vectors, episodic, scratchpad), file-store count/bytes/free, sub-agent capacity (running/queued vs limits), real billed token usage (last turn + session) including `usage.budget[]` - per-provider spend this billing period (tokens in/out, calls, estimated dollars from the owner's rates) against the owner's dollar and token ceilings plus the reset day (v4.1.2: plan work against the budget instead of discovering a refusal at dispatch), and the current value of every config knob (v4.1: the knobs the model can set are all readable here). | (none) | live |
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
(`validateAction()`), then executed by `src/agent/device_actions.cpp` only for
items with `allowed == true`. Each `device[]` element is exactly one of:

| Action | What it does | Params | State |
|---|---|---|---|
| `led` | ring pattern + color + brightness | `mode` (`solid` \| `spinner` \| `pulse` \| `flash` \| `rainbow`), color `r`/`g`/`b` (0–255), `brightness` (0–255 or null) | live |
| `lights` | ring on/off shorthand | `value` (`off` \| `full`) | live |
| `config` | benign, owner-friendly knobs (allow-listed keys only) | e.g. `ledBrightness`, `priority` (routing *preference*), `posture`, `profile`, `theme`, `attnHoldMs`, `sfxLvlN`/`sfxLvlO`, `sfxVol`, `sfxTheme`, `sttProv`, `ttsProv`, `ttsVoice`, `ttsOn`, `sleepOvr`, `brightOvr`, `devName` | live (rails enforced) |
| `orch_model` | switch the head's own provider + model at runtime | `provider` (`openai` \| `anthropic` \| `mistral`), `model` (string) | live |
| `reboot` | restart the device | (none) | live |
| `tts` | speak text aloud on the device speaker (Telegram audio fallback) | `text` | live |

Rails: a `config` action carrying any protected key (secrets, the auth
allow-list, provider routing/priority, the Local Loops or budget governors - see
the human-web-only rail above) is refused as a **whole** with reason
`protected-BLOCKED`. `orch_model` is the one deliberate exception to the routing
rule: the model may repoint its own head host + model. Session ops (spawn /
terminate) are **not** device actions - they go through the turn's
`session_ops[]` / `session.*`, never `device[]`.

On-device speech plays through the built-in speaker. Both the `reply.speak` tool
(mid-turn) and the end-of-turn `device[]` `tts` action synthesize the reply and
play it on the speaker; `tts` falls back to a Telegram audio message when the
device has no working speaker. A spoken Telegram message is also available via
`reply.telegram` `voice:true`. The speaker plays WAV directly and decodes MP3 with
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
in `lib/core/.../connectors_wire.cpp`, host-tested so the two never drift.

## See also

- [Orchestrator World](../orchestrator-world.md)
  - the World memory system and the MCP server (§5).
- [Turn contract](turn-contract.md) - the single-shot turn JSON
  and how `mem_write[]` / `mem_query[]` / `session_ops[]` dispatch to these tools.
- [People & privacy](../people-and-privacy.md) - roles, quotas, and the RBAC
  gating the `tenant.*` and admin-only tools enforce.
