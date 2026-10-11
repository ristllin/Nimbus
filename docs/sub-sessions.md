<!-- audience: dev -->
# Sub-sessions - the orchestrator's background agents, end to end

What actually happens when the head model spawns a sub-agent: the exact brief it
receives, the wire protocol per provider, what the head can and cannot do with a
running or finished session, and what is persisted where. Line references are
v3.5.0. Sibling docs: [turn-anatomy.md](turn-anatomy.md) (the head's own prompt),
[harness.md](harness.md) (the lifecycle around turns), and
[orchestrator-storage.md](orchestrator-storage.md) (the storage tiers).

## The model's surface

The turn contract advertises exactly two ops (`session_ops[]`):

- `spawn` - `task` (required), optional `provider`, `model`. Fire-and-forget.
- `terminate` - stop the running session named by `id`.

`tell` / `poll` / `list` are **deliberately not offered** (they used to be
advertised and hard-failed at runtime; "advertised == real" is a standing rule).
If the model emits one anyway it gets a plain-text refusal in its next turn's
input. `[ACTIVE SESSIONS]` (in every turn's input) is the authoritative digest of
what is running; `[FRESH RESULTS]` is how output comes back.

Caps: 6 journal slots (`kAgentMaxJobs`), 4 in flight (`kMaxActiveInflight`),
a 12-deep pending-spawn queue (`kMaxPendingSpawns`, the depth one turn may
enqueue, decoupled from concurrency), task text up to 4095 bytes
(`kSpawnTaskMax`, truncation logged), one dispatch per ~1.5 s pump cycle, and
dispatch deferred below 28 KB of free internal heap. The model never sees raw
slot counts; every turn it reads a `[SPAWN CAPACITY]` line (see "Throughput"
below).

**Sub-agents run serially on-device, by design.** This is a stability
constraint, not a limitation to fix. Everything (head turns, dispatch, polling,
synthesis) runs on the single `tg_poll` task with one TLS slot (`tlsSlots`
default 1). Mistral sub-dispatch is synchronous: it blocks the `tg_poll` task
inside `POST /v1/conversations` for up to 60 s, because Mistral has no background
job API. OpenAI (`background:true`) and Anthropic (async sessions) parallelize on
the provider's compute, but the device still polls them one at a time. Adding a
worker task or a second concurrent TLS connection collapses the head's contiguous
internal heap and fails turns (measured in the `tlsSlots` 2 vs 1 A/B on
hardware), and starves the watchdog into a reset. Deep fan-out scales by
sequential waves (queue depth), never by concurrency; do not add on-device
parallelism.

## Throughput - the capacity model

The device does one network call at a time, so "how many sub-agents can I run?"
is not a fixed slot count. Every turn the model reads a `[SPAWN CAPACITY]` line,
assembled in `TurnEngine::buildDynamicContext` (`lib/harness/src/engine.cpp`).
The line states how many are
running, how many are queued, how many it can start this turn
(`min(kAgentMaxJobs, queue room)`), that they run sequentially (up to
`kMaxActiveInflight` = 4 in flight, draining as each finishes), and that this is
not a hard total limit. A big fan-out runs over successive waves: start a wave
now, and when it finishes an automatic synthesis turn lets the head spawn the
next, so it can work through dozens over a run. Mistral sub-agents run strictly
one at a time; OpenAI and Anthropic ones run in parallel on the provider while
the device collects them one by one.

**The pending queue is decoupled from concurrency.** `kMaxPendingSpawns`
(12, `caps.h`) is the depth one turn may enqueue; `kAgentMaxJobs` (6) is the
journal and concurrency ceiling. They are two distinct numbers.
`JobEngine::enqueueSpawn` (`lib/harness/src/jobs.cpp`) refuses only when the
inbox itself is full, with *"My spawn queue is full - I couldn't take that one.
Ask again once some agents finish."*; the refused spawn is dropped, nothing
re-queues it. The pump then drains one queued spawn per cycle into the window of
at most 4 in flight, so a deep wave is a bigger inbox for the same single-file
worker, not extra parallelism. The queue's internal-SRAM backing is released
once it drains (a `std::vector().swap` back to the baseline reserve), so a deep
fan-out costs that memory only transiently.

## The brief - the exact prompt a sub-agent receives

Assembled inline in `JobEngine::dispatchSpawn` (`lib/harness/src/jobs.cpp`), in
this order:

```
[Current date-time: 2026-07-27 Sun 14:05 IDT - trust this over any internal sense of the date.]
[SKILL: <id>]
<skill capsule, ≤4096 bytes, from SD /mem/skills/<id>/SKILL.md>

---
<the task text, ≤4095 bytes - always the lead instruction>

[CONTEXT] (the conversation this task came from, oldest first)
- user: <message, ≤300 chars/line>
- assistant: <message>
…

[ATTACHED: <project>/<name>]
<the document's full text - device-supplied, ≤24 KB across all attachments>
```

- **Now-header**: providers give spawned agents no clock, so the brief anchors
  "now" explicitly. Omitted entirely while the device clock is unsynced.
- **Skill capsule** (`composeSkillInjection`, `lib/harness/src/skill_md.cpp`):
  prepended only when the spawn names a skill whose SD capsule has front-matter
  `inject: spawn|both`, and only when that capsule is approved - an
  agent-written capsule stays inert until the owner approves it
  ([harness.md](harness.md)). Built-in skill ids (`web`, `deep_research`, …)
  have no capsule - they act as provider/model routing hints only.
- **[CONTEXT]** (v3.3.0): the last ≤6 chat messages of the spawning conversation,
  digest-rendered under a 1200-byte budget (oldest lines clipped first,
  `(earlier messages omitted - byte budget)` marker when clipped). This is what
  makes "book the place we talked about" actionable. Appended AFTER the task so
  the task stays the lead instruction (pinned by `test_harness_jobs`).
- **[ATTACHED]** (v4.0.0): the full text of each document named in the spawn's
  `attach` array, read from the file store at dispatch and spliced in by the
  firmware. The model references documents by name - cheap output tokens - and
  the device supplies the bytes, so a sub-agent can read files it has no tools
  to open. Up to 4 documents, 24 KB total, UTF-8-safe clip; a document that is
  missing or unreadable becomes an explicit `not found or unreadable` note
  rather than a silent omission. Naming a document only in the task text
  attaches nothing.

Each backend adds its own system prompt around the brief - e.g. OpenAI:
*"You are an autonomous research agent. Complete the task fully and reply with
the final result only."*; Anthropic managed agents get a sandbox system prompt
with bash/web/files tools; Mistral: *"You are an autonomous assistant agent.
Complete the user's task fully and reply with the final result only - no
preamble."*

## What a spawn can carry

`session_ops` spawn fields, beyond `task`/`provider`/`model`:

| field | effect |
|---|---|
| `skill` | an approved capsule id, injected into the brief (above) |
| `name` | short display name - also the sub-agent's saved-document name |
| `project` | run tag: the full result auto-saves as `<project>/<name>-<tag>.md` |
| `attach` | up to 4 `<project>/<name>` documents spliced into the brief |

`project` is what makes multi-stage fan-out work: every sub-agent in a run
writes into one project, and the outcome (`[saved: …]` or `[persist FAILED: …]`)
is appended to the result the head reads, so the head plans the next stage from
what actually landed rather than from an assumption.

## What a sub-agent can and cannot do - the decomposition rule

A sub-agent returns **text only** to the head and has **no device tools** - it
cannot send Telegram, write the device file store, or touch memory. So the HEAD
does every owner-delivery step after a sub returns (`artifact.save`,
`files.send`). A sub CAN, however, run its provider's connectors server-side: a
sub spawned on a provider that hosts Gmail drafts an email in its own run, a
Notion/Drive sub creates a page, and it reports the outcome back as text.

Decompose a big ask accordingly: **sub-agents gather/research (and run
connectors); the head assembles the results and delivers them to the owner.**
Give each sub a `project` so its full reply auto-saves as a document the head can
then deliver. This is exactly what the model is told each turn - the
`[SUB-AGENT CAPABILITIES]` block, generated per keyed provider in `catalogText`
(`lib/core/src/orch_connectors_wire.cpp`). See [connectors.md](connectors.md) for
which provider hosts which connector.

## Provider/model resolution

1. Explicit `provider` in the spawn, if keyed.
2. Else, if the spawn names a skill matching an enabled **connector**, that
   connector's provider (keyed only).
3. Else the first keyed provider in the sub-session priority order
   (`providerPriority`, web-settable, never model-settable).

The model id is checked against the provider's choice list (live-harvested,
else the cold-start list); an id outside it logs `spawn model coerced` and uses
the per-provider sub-model default. The head's `[AVAILABLE MODELS]` line is
built from that same list, so every model it is offered spawns as named. Deep
research models are not in any list (they cannot run the turn contract), so a
`deep_research` skill hint runs on the provider's sub-model default with its
always-on web search, not on a dedicated deep-research model.

## The wire, per backend

| backend | create | poll | cancel | survives a restart? |
|---|---|---|---|---|
| **anthropic** | `POST /v1/environments` + `/v1/agents` (both cached: NVS, plus a RAM copy for the boot) → `POST /v1/sessions` → brief as a `user.message` event | `GET /v1/sessions/<sid>/events?types[]=agent.message&types[]=session.status_idle&types[]=session.error` | `user.interrupt` event | **yes** (session id in the journal) |
| **openai** | `POST /v1/responses` with `background:true, store:true` + always-on `web_search` tool | `GET /v1/responses/<id>` (status + output text + **usage**) | `POST /v1/responses/<id>/cancel` | **yes** (response id) |
| **mistral** | `POST /v1/conversations`, which is **synchronous**: the reply streams back at dispatch and is cached on-device | poll serves the cached reply once as Done | cache-local | **no**: a restart loses the cache and the job reads as "expired" |
| **custom** | `POST /v1/chat/completions` (or `/v1/messages`) - synchronous, single cache slot | cache, once | cache-local | no |

The device polls round-robin every ~15 s on the `tg_poll` task. The result
envelope carries up to **16 KB** of reply (PSRAM-backed, v4.0.0 -
large enough for a sub-agent to return a whole document); token usage is filled only by
OpenAI (the Anthropic events poll returns no usage object - honest zeros).

**A Mistral sub that outruns the 60 s blocking read is reported as a timeout,
not as a failed start.** Because the sub runs inside that synchronous
`POST /v1/conversations`, a slow one outlasts the device's 60 s read deadline.
`mistralDispatch` (`lib/harness/src/providers/mistral.cpp`) classifies that case
as `FabricErr::Timeout` (elapsed is roughly the whole deadline) rather than a
connection failure, and `JobEngine::dispatchSpawn` (`lib/harness/src/jobs.cpp`)
tells the owner *"The agent on mistral didn't respond within 60s - it may have
run without me getting its result. Try a smaller task or split it into steps."*,
since the agent may have started.

**A sub the provider refuses with HTTP 429 names the quota window.**
The dispatch hands the window back through `Directive::rateLimit`
(`lib/harness/include/nimbus/harness/fabric.h`): from Mistral's `x-ratelimit-*`
headers (a spent Studio connector day reads the same in the body as a
per-minute limit, so only the headers tell them apart), else from the provider's
error text (OpenAI, the custom endpoint, Z.ai, the Cumulo router). The owner
hears *"Couldn't start that agent on mistral."* followed by the same reply a
head-turn 429 gets (`rateLimitReply`, `lib/harness/src/rate_limit.cpp`); for a
spent connector day that ends *"it resets at midnight UTC."* A Virtual Nimbus
runs the same code. An Anthropic spawn makes up to four calls (environment,
agent, session, first turn), and a 429 on any of them is classified the same
way, from the provider's error text.

**An Anthropic spawn names why it failed, and never re-creates what it already
has** (`antDispatch` in `lib/harness/src/providers/anthropic.cpp`):

- **The ids live in RAM for the whole boot.** The environment id and the
  per-model agent ids are loaded once from NVS and written through to it. Each
  write is read back; when NVS is full the write is dropped silently, so a
  dropped one logs `anthropic: env id not saved (storage full?) - kept in RAM
  until restart` once and the RAM copy keeps serving it. The session request
  always carries both ids, and no spawn creates a second environment or agent.
  (Before this, a full NVS sent the session with an empty `environment_id`,
  which the API refuses with HTTP 400 `environment_id: value is required`.)
- **Every failure names its reason.** A non-2xx on any call of the chain logs
  the step, the status and the provider's reason (at most 150 characters), for
  example `anthropic: create session HTTP 400: invalid_request_error:
  environment_id: value is required`. The owner still hears *"Couldn't start
  that agent on anthropic."* for every refusal except a 429; the reason is in
  the device log (`GET /api/errlog`).
- **The error is classified.** 429 is a rate limit with its window, 401/403 a
  key problem, 404 a missing environment or agent, any other 4xx a refused
  request, and 5xx a provider fault.
- **Dead ids are dropped.** When the session create says the cached environment
  or agent is gone (404) or archived (a 400 "... is archived"), both cached ids
  are dropped so the next spawn re-creates them. Before, an archived environment
  failed every spawn until NVS was erased.
- **A stopped session ends the job.** A session that stops for any reason other
  than finishing or waiting for input (for example `retries_exhausted`) ends as
  an error instead of running forever.
- **MCP connectors fit the managed-agents shape.** Owner MCP connectors ride the
  sub-agent as `mcp_servers` plus a matching `mcp_toolset` (tool calls allowed
  without confirmation, since a sub-agent runs unattended). A connector that
  needs a token is left off and logged (`anthropic: connector <name> needs a
  vault credential - left off the sub-agent`): the API refuses inline MCP
  tokens, and the spawn would otherwise fail at agent creation.

The host harness runs this whole chain live: `nimbus-lab sub` in
`tools/harness-lab` (add `--nvs-full` to reproduce a full partition).

## Results - how output reaches you

1. Terminal poll → the result is parked as a **fresh result** (UTF-8-capped at
   3500 chars). Mid-run the chat stays quiet: results accumulate until every
   spawned sub-agent has finished (the batch **drains**).
2. On the drain, an automatic **synthesis turn** runs with a `[FRESH RESULTS]`
   block and a `[SYSTEM]` preamble - no user message. It runs under the same
   unattended rails as scheduled loops (sub-agent output is untrusted web
   content: no reboot, no ttsOn, no loop.create, etc.).
3. If the synthesis cannot run (low memory, provider down) for 60 s **after the
   drain**, or the synthesis turn itself fails, the raw results are delivered
   directly, so results are never lost to a stuck head. The clock starts at the
   drain, not at the first result, so subs that finish minutes apart still get
   their synthesis. The raw delivery is written for the owner: one line
   ("Here are the raw findings; the summary step did not complete."), then
   each sub-agent's name and its findings, with none of the model-facing
   scaffolding (no `[FRESH RESULTS]` header, job tags or `results.get` handles).
4. A failure while other sub-agents still run joins the fresh results (the
   final report names it); a failure with nothing else running is delivered at
   once: `Job [<name>] failed: <error>`.

Every job-engine message (spawn acknowledgements, "Couldn't start that agent
on ...", failures, the raw fallback) is also recorded in the chat's history, and
the synthesis reply is recorded like any turn reply, so the web chat, which
renders that history, shows them and they survive a reload. They are not
conversation the assistant reads back: the recent-conversation window, a
sub-agent's brief and compaction all skip them (a raw fallback is sub-agent web
content). An open web chat watches the device's activity (every 4 seconds while
sub-agents run, every 30 seconds otherwise) and re-renders when a result lands,
so the synthesis, or a reminder that fires later, appears without sending
another message.

## What the head can and cannot do

| ask | answer |
|---|---|
| talk to a RUNNING sub-session | **No.** The fabric is fire-and-forget; `tell`/`poll` are refused everywhere (schema, tool registry, prompt self-model). The one adjacent seam - Anthropic's `answer()` for NeedsInput resume - exists in the adapter but has no caller. |
| kill a running session | **Yes** - `session_ops terminate` from a turn (synchronous), or `session.terminate` over LAN MCP / web (staged to `tg_poll`). Note: a terminated job produces no `[FRESH RESULTS]` entry and no `ev:subresult` row. |
| resume / re-address a PAST (completed) session | **No.** Once its result is consumed the journal slot is reclaimed; there is no session handle to call back. Past work survives only as episodic rows, whatever the synthesis turn `mem_write`-ed, and any artifact the sub-agent's result led the head to save. |
| see what a sub-agent did internally | **Not on-device.** Only the final reply text crosses the wire. The full internal transcript (tool calls, thinking) exists provider-side - Anthropic's events API could stream it, but the device deliberately requests only three event types. Capturing it is a documented follow-up. |

## What is persisted, where

**There is no SQL database.** (SQLite was evaluated and retired; the append-log
episodic store is the system of record. The one `sqlite3` in the repo is the
battery-lab's host-side tool, not firmware.)

| data | store | survives reboot |
|---|---|---|
| active job records (tag, provider jobId, model, chat, state) | **NVS** namespace `agjournal`, 6 slots, ~200 B each | yes - the device re-attaches and resumes polling (anthropic/openai only; mistral/custom caches are RAM) |
| spawn + result trace rows (`ev:spawn` / `ev:subresult`, the glass-box disclosure) | episodic store, the **spawning chat's** session, kind=log | yes (SD day-streams) |
| the sub-agent's final reply | 16 KB envelope → 3500 char fresh result → the synthesis turn's reply + anything it `mem_write`s; 512-char excerpt in the `ev:subresult` row | via episodic / vector memory |
| the sub-agent's internal transcript | **not stored** - provider-side only | - |
| pending spawn queue, fresh results awaiting synthesis | RAM | no - lost on reboot (results already delivered per-job survive as chat messages) |

**No-SD degradation** (same tiering as everything else,
[orchestrator-storage.md](orchestrator-storage.md)): episodic falls back to a
500-message RAM ring + a LittleFS whole-blob (so recent context survives, deep
history doesn't); vector memory caps 5000 → 400 with a flash-floor guard; the
glass-box **trace rows stop entirely** while the SD is absent or demoted
(`traceActive()` requires a live SD) - device-event timeline rows keep writing.
A mid-run SD loss demotes seamlessly (debounced 2-fail/2-pass) and promotes back
on recovery; the current conversation's working set stays in the PSRAM ring
throughout.

## Inspecting it live

- Web chat: the "N sub-agent events" disclosure under an assistant bubble shows
  the spawn brief (task) and terminal result rows.
- `memory.episodic` tool or `GET /api/mem/episodic?kind=log`: the same rows,
  filterable by `since_hours`/`before_hours`.
- `[ACTIVE SESSIONS]` in any turn, or the "Active sessions" card on the web
  Home page: live state.
- `/api/lastturn` - whether the last head turn spawned anything (section 3, the
  raw `orch_turn` JSON's `session_ops`).
