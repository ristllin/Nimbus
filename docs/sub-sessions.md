<!-- audience: dev -->
# Sub-sessions - the orchestrator's background agents, end to end

What actually happens when the head model spawns a sub-agent: the exact brief it
receives, the wire protocol per provider, what the head can and cannot do with a
running or finished session, and what is persisted where. Sibling docs: [turn-anatomy.md](turn-anatomy.md) (the head's own prompt),
[harness.md](harness.md) (the lifecycle around turns), and
[orchestrator-storage.md](orchestrator-storage.md) (the storage tiers).

## The model's surface

The turn contract advertises exactly two ops (`session_ops[]`):

- `spawn` - `task` (required), optional `provider`, `model`. Fire-and-forget.
- `terminate` - stop the running session named by `id`.

`tell` / `poll` / `list` are **deliberately not offered** (they used to be
advertised and hard-failed at runtime; "advertised == real" is a standing rule).
If the model emits `tell` or `poll` anyway, its next turn's input carries a
plain-text refusal in `[MEMORY RESULTS]` (`[SESSION] '<op>' isn't supported: ...`);
`list` is silently ignored, since `[ACTIVE SESSIONS]` already answers it. `[ACTIVE SESSIONS]` (in every turn's input) is the authoritative digest of
what is running; `[FRESH RESULTS]` is how output comes back.

Caps: 6 journal slots (`kAgentMaxJobs`), 4 in flight (`kMaxActiveInflight`),
a 12-deep pending-spawn queue (`kMaxPendingSpawns`, the depth one turn may
enqueue, decoupled from concurrency), task text up to 4095 bytes
(`kSpawnTaskMax`, truncation logged), at most one dispatch every 2.5 s, and
dispatch deferred below 12 KB of free internal heap (`ORCH_DISPATCH_MIN_HEAP`, the
same floor as a turn; see [Memory](memory.md)). The model never sees raw
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

**The head hears the real reason a spawn did not start.** Every refusal (queue
full, no keyed provider, a provider that refused the dispatch, including a 429
with its quota window) is told to the owner at once and also leaves one line in
the job engine, kept per spawning chat with its own 640-byte budget (past it,
that chat gets one `(more refusals not listed)` line; no other chat is
affected). At most 16 chats (the chat registry size, `kFoldMaxChats`) hold
notes at once; a seventeenth drops the notes that waited longest, but never a
chat's "may have started" line (below) while another chat's notes hold none. That chat's
next turn (and no other chat's: the line carries its task text) reads them in a
`[SPAWN REFUSED]` block (`JobEngine::takeSpawnNotes(chatId)`, background framing:
the owner was already told, nothing is running for them), so the head neither
retries blind nor invents a reason. A dispatch that **timed out** is different:
a synchronous sub (Mistral, Z.ai, the Cumulo router, the custom endpoint) runs
inside the request that outlived its wait, so it most likely ran and only its
result was lost, and no job exists to wait on. Its line
reads `'<name>' may have started: ... Do not spawn it again unless the owner
asks` (such a line is never folded away while the chat's notes are under twice
the budget), the block's framing says a "may have started" sub must not be
spawned again, and `[SPAWN CAPACITY]` in every chat counts it as possibly running
for 10 minutes (`JobEngine::maybeRunningCount`; the "do not spawn it again"
instruction stays with the spawning chat), so the head does not pay for the same
work twice. A head that calls a spawn field as a tool
(`session_ops`, `spawn`) gets the live `[SPAWN CAPACITY]` number in the recovery
text ([turn-contract.md](reference/turn-contract.md)): a Mistral head once
claimed "spawn capacity is 0 this turn" while the prompt offered 6, right after
a bare `unknown tool: session_ops`.

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
  digest-rendered under the head's brief budget (`briefBytes`: 1200 bytes for a
  200K-context head, scaled with the head's context window between 600 and 4096
  bytes; oldest lines clipped first,
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
preamble. If a tool or connector you need does not run, fails, or returns
nothing, say so plainly; never invent events, messages, files or other
records."*

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
An Anthropic sub-agent's working notes ("I'll start with...", said before each
tool call) arrive as their own short messages, so the reply - and the file it is
saved to - is the run's longest message plus anything after it; short notes before
it are left out, while a long earlier message (part of the work) is kept.

**A sub that outruns its blocking read is reported as a timeout, not as a
failed start.** A Mistral, Z.ai, Cumulo router or custom-endpoint sub runs
inside one synchronous request (Mistral's `POST /v1/conversations` with a 60 s
read deadline, the chat-completions or Messages request of the others with
30 s), so a slow one outlasts the deadline. `mistralDispatch`,
`openaiCompatDispatch` and `customDispatch` (`lib/harness/src/providers/`)
classify a transport failure that took at least nine tenths of the deadline as
`FabricErr::Timeout` rather than a connection failure (a fast failure stays a
failed start: the work never left), and `JobEngine::dispatchSpawn`
(`lib/harness/src/jobs.cpp`) tells the owner *"The agent on zai didn't respond
in time - it may have run without me getting its result. Try a smaller task or
split it into steps."*, since the agent may have started.

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
   content: no `reboot`, no `ttsOn`, no `loop.create`, etc.).
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
5. **Connector provenance.** A result whose task asked for an attached
   connector that never ran opens with an honest marker, and its turn stores
   nothing. The Mistral sub-agent is synchronous, so the dispatch response
   already lists every tool run as a `tool.execution` output (no extra request).
   Before the run, `mistralDispatch` records which Studio connectors rode the
   request and asks `connectorAskedFor` whether the bare task
   (`Directive::task`, never the chat `[CONTEXT]` tail) or the spawn's `skill`
   names one of them (its name, catalog or workspace id, or an owner-data phrase
   such as "my calendar"; talk about calendars in general does not count). A
   catalog name that is an ordinary English word (Linear, Notion, Slack) counts
   only inside a phrase ("Linear issues", "post it in slack", "my notion",
   "Linear connector"), so "explain linear regression" is not a Linear request;
   Linear's prepositional phrases need the capital ("In Linear, list my bugs")
   and are never part of a title ("from Linear Algebra Done Right"). The name
   the owner gave a catalog connector never counts on its own (a Notion
   connector named "notes" does not claim "summarize the meeting notes"). The wire
   carries only connector ids, and a custom Studio connector rides by UUID, so
   the dispatch also runs the attach once on a scratch document that is never
   sent (`kMistralIdentityProbe`), which adds each connector's device name and
   type: a task that says "pull my deals from crm" is matched to the connector
   named `crm` on the device (a plain-word name such as `crm` or `notes` counts
   in an owner-data frame like "from crm" or "my notes"; one with a digit, a
   separator or mixed case, such as `crm-eu` or `HubSpot`, counts anywhere), and an
   explicit id such as `notion_mcp` still reads as Notion. After it, `countsAsConnectorRun` decides which runs count for that
   connector (named by its id, catalog id, device name or type):
   provider built-ins (`isProviderBuiltinTool`) and runs named for another
   attached connector (`gmail_search` when Google Calendar was asked) do not;
   unclaimed or nameless runs do, so real data is never called a guess on a
   naming hunch. The `ResultEnvelope` carries `connectorCalls` and
   `connectorAsked`; the log line is
   `mistral sub: connectors attached=1 calls=0 asked=Google Calendar`. When
   `connectorAsked` is set and `connectorCalls` is 0, `JobEngine` builds the
   text with `connectorNotCalledNote` (*"The Google Calendar connector was NOT
   called. The text below is the model's own guess, not your calendar."*) in
   front BEFORE any copy is made: the result hook's activity row, the project
   auto-persist, `[FRESH RESULTS]` and the owner's raw fallback all carry it,
   and the fresh result is flagged. The turn that takes a flagged result (the
   synthesis turn, or an owner turn that folds it in) gets a provenance sentence
   in its `[SYSTEM]` text and stores nothing in long-term memory: `mem_write[]`
   items in `applyTurn` and the mid-loop `memory.write` and `memory.update`
   tools are refused with `kUnverifiedMemoryRefusal`, which also lands in the
   next turn's `[MEMORY RESULTS]`. A head that calls the `mem_write` field as a
   tool on that turn hears that memory writes are refused this turn, through
   that field too, and why, instead of the usual "call memory_write now". Refusing (rather than tagging) keeps the
   guess out of memory in that turn; a later turn can store it only when asked,
   and the marker stays on every copy it would read. Known limits: the match is
   lexical (a task that never names the connector is not marked, and the sub
   system prompt's no-invention rule is the backstop), and adapters that cannot
   see tool runs leave the fields zero, so their results are delivered as
   before.

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

| data | store | survives a restart |
|---|---|---|
| active job records (tag, provider jobId, model, chat, state) | **NVS** namespace `agjournal`, 6 slots, ~200 B each | yes - the device re-attaches and resumes polling (anthropic/openai only; mistral/custom caches are RAM) |
| spawn + result trace rows (`ev:spawn` / `ev:subresult`, the glass-box disclosure) | episodic store, the **spawning chat's** session, kind=log | yes (SD day-streams) |
| the sub-agent's final reply | 16 KB envelope → 3500 char fresh result → the synthesis turn's reply + anything it `mem_write`s; the full reply in the `ev:subresult` row | via episodic / vector memory |
| the sub-agent's internal transcript | **not stored** - provider-side only | - |
| pending spawn queue, fresh results awaiting synthesis | RAM | no - lost on restart (results already delivered per-job survive as chat messages) |

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
  filterable by a time window (`since_hours`/`before_hours` on the tool,
  `sinceh`/`beforeh` on the HTTP route).
- `[ACTIVE SESSIONS]` in any turn, or the "Active sessions" card on the web
  Home page: live state.
- `/api/lastturn` - whether the last head turn spawned anything (section 3, the
  raw `orch_turn` JSON's `session_ops`).
