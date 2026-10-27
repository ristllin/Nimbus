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
an 8-deep pending-spawn queue (`kMaxPendingSpawns`, the depth one turn may
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
inside `POST /v1/conversations` for up to 120 s (the same ceiling as every other
provider), because Mistral has no background job API. OpenAI (`background:true`) and Anthropic (async sessions) parallelize on
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
next, so it can work through dozens over a run. The line then says how each
keyed provider runs a sub, from one table (`kDeviceBackendSync`,
`lib/harness/include/nimbus/harness/fabric.h`): Mistral, the custom endpoint,
Z.ai and the Cumulo router run one at a time inside the device and must finish
within its wait or their work is lost; OpenAI and Anthropic run in parallel on
the provider while the device collects them one by one, so research and long
work belong there. A new backend without a mode fails to build.

**The spawn ledger.** Each chat's next turn also reads a `[SPAWN LEDGER]` block
(`JobEngine::spawnLedgerText`): that chat's sub-agents from the last two hours,
newest first, from the job engine's own records - queued, running (with its job
tag and provider/model), finished, failed with its reason, did not start with
its reason, or stopped. The head never has to keep this in its notes or re-check
it with file or session tools (on 17-18 Oct a note to "verify files_list" made
every later turn poll a dead project). It is RAM only, at most 12 entries.

**The pending queue is decoupled from concurrency.** `kMaxPendingSpawns`
(8, `caps.h`) is the depth one turn may enqueue; `kAgentMaxJobs` (6) is the
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
full, no keyed provider, a provider that refused the dispatch with its reason -
the key refused, the provider unreachable, a model it does not offer, a provider
error, a 429 with its quota window - and every spawn the turn itself could not
hand over: no task, a spawn list that could not be read, over the per-turn cap,
or a reply that promised sub-agents the turn never started) is told to the
owner at once and also leaves one line in
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
(`session_ops`, `spawn`) has the call's usable spawn items carried into its turn
(started once, deduped against its final reply); a call with nothing usable gets
the live `[SPAWN CAPACITY]` number in the recovery text
([turn-contract.md](reference/turn-contract.md)).

**The model a spawn runs is one its provider lists.** A spawn that names a model
the provider does not list (or names none) runs the provider's sub-agent default
when that is listed, else the cheapest model the list holds by the built-in
price table, never simply the first entry (on most lists the most expensive
model). When no listed model has a known price the spawn is refused,
and the owner and the head are told what was asked, or that nothing was, and
which default is missing. A provider with no list at all (the custom endpoint)
runs the model as asked. The spawn ledger names the model that ran, what was
asked for and why it was substituted, also when no model was named.

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

[Reply size: the first 16 KB of your reply reaches the head directly; a longer reply is saved whole as a file the head can read, so finish your full report and do not pad or repeat it.]
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
- **Reply size**: the last line states the reply cap, so a sub-agent knows a long
  report is kept whole rather than cut (see Results below).

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
does every owner-delivery step after a sub returns (`artifact.save`, then
delivery). A sub CAN, however, run its provider's connectors server-side: a
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

1. Explicit `provider` in the spawn.
2. Else, if the spawn names a skill matching an enabled **connector**, that
   connector's provider (keyed only).
3. Else the first keyed provider in the sub-session priority order
   (`subPrio`, web-settable), then the router keys the device holds (Cumulo,
   then Z.ai), so a device with one key runs a spawn that names no provider on
   that key instead of refusing it.

A provider the one per-provider park holds closed (out of credit or a refused key
learned on any seam, or paused; [provider-wire.md](provider-wire.md)) gets no
spawn: the order skips it, and a spawn that names it (or a connector routed to it)
moves to the first open provider, logged `spawn moved openai -> anthropic (...)`
and noted in the spawn ledger. A connector task cannot move, and with nothing open
the spawn is refused with the reason: *"Couldn't start that agent: openai says the
account is out of credit (HTTP 429); it is checked again in 12 minutes."* A
sub-agent request that the provider refuses for billing or with a refused key
closes that provider for every seam too.

**A provider with no key of its own runs on the Cumulo router** when the
Cumulo key is set and the router offers that provider's model: the spawn's
`<provider>/<model>` selector, else the selector of that provider's sub-model
default, must be in the router's choice list (the `cumulo` rows of `models.list`; a bare
id there is the router's default upstream, openai). The log says `spawn on openai has no key - runs on the Cumulo router as
openai/gpt-5.6` and the sub runs on the `cumulo` backend with that selector. A
provider with a key of its own always runs on it. A spawn whose `skill` names a
connector never runs on the router (the router never carries the owner's
connectors, and its result could not be marked as one whose connector did not
run): without that provider's key it is refused, for example *"Couldn't start
that agent on mistral. The gcal connector runs only on a mistral key. Add it
under Providers & models."* On a hosted instance an approved member's spawn that
names a provider with no key runs the configured sub-agent order instead (only the
owner picks a spawn's model). When neither a key nor the router offers the model,
nothing is sent and the owner hears the reason, for example *"Couldn't start that agent
on openai. No openai key is set, and the Cumulo router does not offer
openai/gpt-9. Add the key under Providers & models, or ask for a model the router
lists."* A key or custom endpoint added later on the web page applies to the next
spawn; no restart is needed.

The model id is checked by the device's one model check: a model in the
provider's model store that the key can run, else (before the first fetch) the
built-in starting list, or the provider's default, or the model the owner chose
for that provider (never refused; a helper with no model named runs it). An id it refuses logs
`spawn model coerced` and uses the per-provider sub-model default when listed,
else the cheapest listed model (above). The head reads the store through
`models.list` (or, when the tool is not registered for that head, the
`[AVAILABLE MODELS]` line: the store's
short list, which says how old it is, "list from 2026-10-22, may miss newer
models"), a subset of what the check accepts, so every model it is offered spawns as
named. A router id (`<upstream>/<model>`) named on a direct provider is not in that
provider's list: the spawn ledger the head reads says `not in <provider> list` and
why, and the spawn runs as the bare id when that provider lists it, else on the
fallback above. Deep
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
large enough for a sub-agent to return a whole document). A **longer reply is kept
whole**: the writer every adapter uses holds the full text when it cuts the
envelope (up to 256 KB), the job engine saves it to the card as a file in the job's
project (or `results` when the spawn named none), and the envelope ends with a
marker naming that file and the reply's real size, for example `[cut at 16 KB: the
whole reply, 40960 bytes, is saved as research/report-job0003.md; read the rest
with files.read from an offset]`. If the card cannot take it the marker says the
rest could not be saved, and why; the cut is never presented as the whole report.
(A note about uncaptured files is kept whole either way.) Every sub-agent's spend is
metered: OpenAI's terminal poll carries its usage, a finished Anthropic session's usage
is read once from `GET /v1/sessions/<id>`, and the synchronous subs (Mistral, Z.ai,
Cumulo, custom) are metered at their one request; the web searches a sub runs on its
provider count too (see [Monthly spend ceiling](spend-ceiling.md)). Stopping a running
OpenAI or Anthropic sub (`session.terminate`, or the assistant ending it) always sends
the cancel, even while paid calls are paused, and the job is read back right after it
(a free `GET` of the response or session) so what it spent until then is counted. An
Anthropic interrupt is processed asynchronously and a session reports its tokens once
it is idle, so that read counts only an idle (or terminated) session; one still running
counts later, as below.

A stop the provider does not confirm (no answer, a refusal, a session still running)
never drops the run. The job leaves its slot at once (no result is delivered, no
synthesis waits on it, a new helper can start) and joins a **stop list** kept apart from
the job records, saved in flash (`/data/jobs_stopping.txt`; on a hosted instance in its
data folder), so a restart keeps it. The assistant says the stop is not confirmed yet.
The list asks the stop again after 30 s, doubling up to 5 minutes, and polls the run
every 15 s, one request a pump: a confirmed stop, or a final poll counted as the
provider reports it (0 included; a poll that cannot read the usage, or a run that lists
work but no tokens, counts the session estimate), ends it. A run the provider no longer
has (404) ends with nothing more to count. Fifteen minutes after a run joined the list
(or after 15 minutes of failed polls) the session estimate is counted at once (a later
final read counts only what exceeds it); after 6 hours of failed polls, or a day on the
list whatever the polls say, the run is dropped with a message. The list holds 24 runs;
a full list drops its oldest, counted at the estimate first. A list that cannot be saved
counts the new run's estimate at once. An Anthropic session counts as stopped only when
it reads idle after running (some active time or usage): a fresh session also reads
idle.

The usual 15-minute give-up on a job whose polls all fail now also counts the session
estimate and puts the job on the stop list (its result still does not arrive). When the
monthly spend ceiling or a provider's monthly limit pauses paid calls, every sub still
running on a paused provider's side goes to the stop list the same way and its chat gets
one message naming them; raising the limit does not restart them. A hosted instance
reads its ceiling directly for this. An Anthropic sub whose starting turn got no answer
(the run may have started with no job id to follow) is interrupted at once; if that
interrupt is not confirmed either, the session joins the stop list. An
OpenAI background run that ends `incomplete` keeps the text it wrote, marked
`[the agent stopped before it finished (<reason>): this is its partial answer]`;
with no text it fails with that reason.
An Anthropic managed agent that names its deliverable under
`/mnt/session/outputs/` and has no file captured into the result ends with
`[report left in the sandbox, not retrieved: <path> - this reply is the agent's
summary, not the report]`, so its summary is never taken for the report.
An Anthropic sub-agent's working notes ("I'll start with...", said before each
tool call) arrive as their own short messages, so the reply - and the file it is
saved to - is the run's longest message plus anything after it; short notes before
it are left out, while a long earlier message (part of the work) is kept.

**A sub that may have started is reported as a timeout, not as a failed
start.** A Mistral, Z.ai, Cumulo router or custom-endpoint sub runs inside one
synchronous request (Mistral's `POST /v1/conversations` with a 120 s read
deadline, a Z.ai sub's chat-completions request with 50 s, a Cumulo router sub's
chat-completions or Messages request with 120 s, or 50 s on the router's zai
upstream, and a custom-endpoint sub's with 30 s), so a slow one outlasts the deadline; an OpenAI sub's `POST /v1/responses` and an
Anthropic sub's first turn (`POST /v1/sessions/<id>/events`) start a billed run
of their own. Every sub dispatcher reads a transport failure of that request by
one rule (`wire::subTransportErr`, `lib/harness/src/providers/wire.h`, on the
device and a hosted instance alike): a request that went out and got no answer
(a connection dropped mid-wait, or the deadline), or a failure that took at
least nine tenths of the deadline, is `FabricErr::Timeout`; a request that
never fully left, or a fast failure, stays a failed start (the work never left),
and so does a cut in the Anthropic calls before the turn, which run nothing. On
a hosted instance a sub's request never rides a kept-alive connection, so the
transport never sends a cut one a second time. `JobEngine::dispatchSpawn`
(`lib/harness/src/jobs.cpp`) tells the owner *"The agent on zai didn't respond
in time - it may have run without me getting its result. Try a smaller task or
split it into steps."*, since the agent may have started. A dispatch the
provider answered with a result that could not be read (`FabricErr::ParseFail`:
empty content, or a body the device could not parse) ran and was charged, so it
is told the same way (*"The agent on zai answered, but I couldn't read its
result - it may have run..."*) and its note is a "may have started" one too.

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
   only inside a phrase ("Linear issues", "post it in slack", "put it in my
   notion", "Linear connector"), so "explain linear regression" is not a Linear
   request; a lowercase "in slack", "on slack" or "to slack" ends the ask ("tend
   to slack in August" and "slack periods" are the word) and "from slack" counts
   at the end of the ask or before the next step, a time or a topic ("summarize
   the messages from slack about the outage", "from slack since Monday" count;
   "from slack to taut" and "from slack demand" do not), never after the words
   that make it the verb or the idiom ("teams tend
   to slack.", "we ran low on slack."), while Slack Connect, a Slack canvas,
   huddle or workflow name it in any case; "my notion" and "my linear" count
   after "in", "into" or "to" at the end of the ask, Linear's prepositional
   phrases need the capital ("In Linear, list my bugs"), and none is part of a
   title ("from Linear Algebra Done Right", "from Slack: Getting Past Burnout";
   "post this to Slack: Deploy finished" is still an ask). The name
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
| active job records (tag, provider jobId, model, chat, state) | flash file `/data/agjournal.json`, 6 records, ~200 B each (NVS `agjournal` until v4.5.16) | yes - the device re-attaches and resumes polling (anthropic/openai only; a mistral/zai/cumulo/custom result lived in RAM, so its owner hears it was lost at the restart) |
| the last 8 finished jobs (tag, name, project, saved file, finish time) | the same file | yes - listed in the head's turn context after a restart so finished work is not run again |
| spawn + result trace rows (`ev:spawn` / `ev:subresult`, the glass-box disclosure) | episodic store, the **spawning chat's** session, kind=log | yes (SD day-streams) |
| the sub-agent's final reply | 16 KB envelope → 3500 char fresh result → the synthesis turn's reply + anything it `mem_write`s; the full reply in the `ev:subresult` row; a reply longer than the envelope saved whole as a file in the job's project (or `results`) | via episodic / vector memory; the file via `files.read` |
| the sub-agent's internal transcript | **not stored** - provider-side only | - |
| pending spawn queue, fresh results awaiting synthesis | RAM | no - lost on restart (results already delivered per-job survive as chat messages) |
| the stop list (runs whose stop is not yet confirmed: tag, provider job id, model, chat, the estimate already counted) | flash file `/data/jobs_stopping.txt`, up to 24 | yes - the stop is asked again at once after a restart |

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
