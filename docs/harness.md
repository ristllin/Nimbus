# The Agentic Harness (`lib/harness`)

This page maps the Orchestrator's policy and orchestration layer, shipped as a
standalone, host-testable PlatformIO library. Read it before changing
`lib/harness/` or `src/agent/`: it covers what loads at boot, which task runs
what, the shape of one turn, the self-driving layers, the contract set and the
invariants that must not regress.

Companion deep-dives: [turn-anatomy.md](turn-anatomy.md) (the exact prompt the
head model sees + how to dump it raw), [sub-sessions.md](sub-sessions.md) (the
sub-agent protocol, brief template, and persistence),
[provider-wire.md](provider-wire.md) (structured-output enforcement, tool-loop
wire rules, and failure handling per provider), and
[orchestrator-storage.md](orchestrator-storage.md) (storage tiers + no-SD
degradation). The lifecycle sections below are the high-level map that ties them
together.

## Lifecycle 1 - what loads when (Orchestrator boot)

Order matters; each step is in `src/main.cpp::setup()` or
`orchestratorBegin()`:

1. **`otaupd::bootGuard()`** - the first line: it burns one A/B boot attempt and
   self-reverts a bad OTA image after 3 boots (before any driver can crash it).
2. Board bring-up (`solide::begin`: SD, NVS, screen/LED tasks, battery),
   then two memory reroutes that everything downstream depends on: **mbedTLS →
   PSRAM** (TLS handshakes don't fit internal SRAM) and **malloc ≥128 B →
   PSRAM** (String/JSON churn).
3. Task watchdog configured (8 s, panic), but the main loop subscribes only at
   the end of setup: a slow SD or Wi-Fi boot must not watchdog-loop the device.
4. Config and mode from NVS (mode is resolved at boot; switching modes restarts
   the device). In Orchestrator mode the Bluetooth controller RAM is released and
   Bluetooth never starts.
5. Orchestrator starts Wi-Fi (station plus the setup access point; a TFT drops
   the access point while the station is connected). Notifier instead keeps
   Wi-Fi off and starts Bluetooth. Then
   **`memory::begin()`** (inside `beginWeb`) loads the vector DB from SD/LittleFS
   into PSRAM, scratchpad +
   retrieval config from NVS, and the episodic **hydrate** - day-stream JSONL
   files on SD are indexed and the last 512 messages rebuilt into the PSRAM hot
   ring, so per-chat context survives a restart with zero flash reads at runtime.
   A boot timeline row (`ev:boot` with reset reason, OTA result, and the crash
   summary after a crash boot) is captured. The boot record itself is the first
   line of the durable log, written before any of this runs
   ([orchestrator storage](orchestrator-storage.md#boot-record-and-crash-report)).
6. **`orchestratorBegin()`**: provider fabric + adapters, sinks, the job
   **journal restore from NVS** (active sub-agents re-attach and resume
   polling), running-memory load (LittleFS), routines load
   (`/data/loops.json`, every `nextRun` recomputed from now), the dream loop
   ensured (insert-if-missing; an owner pause survives), and finally
   `telegram::begin` spawns **`tg_poll`** - the task every turn runs on. The
   task spawns even with no Telegram token, because web/serial/voice turns are
   injected through the same queue.
7. The timezone (NVS) is applied in `loops::begin`, before any wall-clock math.
   SNTP arms from the network-up latch: the first main-loop tick that finds the
   station holding an IP kicks it, however early the join landed, and every
   reconnect kicks it again; while the link is up and the clock is still unsynced
   a backstop re-kicks on a doubling backoff (3 min up to 30 min, after lwIP's own
   retries). The latch reads the live link state, never the GOT_IP event, because
   a fast join on stored credentials fires GOT_IP before `setup()` has finished
   (see `nimbus/wifi/net_up.h`). The main task is the only SNTP producer
   ("Sync now" only requests a kick) and never touches TZ. Until first sync: the
   prompt says the time is UNKNOWN, wall-clock loops can't fire, dream maintenance
   is skipped, and pre-sync episodic rows are held and restamped at first sync.

## Lifecycle 2 - who runs what (task topology)

| task | watchdog | runs |
|---|---|---|
| main loop (core 1) | **8 s panic WDT** | rendering, ring, touch, battery/thermal/OTA ticks, staged device actions, `reapStuckTurn`, retention prune (6 h) |
| **`tg_poll`** (core 0) | **none, deliberately** (its long-poll blocks ~25 s) | everything agentic, single-writer: Telegram long-poll, every LLM turn (owner, synthesis, scheduled, post-OTA), the loops tick (20 s gate), the dream firing, the sub-agent dispatch/poll pump (~15 s), staged-mutation drains |
| AsyncTCP | none | all `/api/*` + `/mcp` handlers - they **stage** mutations and read snapshots, never touch agentic state directly |
| screen / leds / sfx | none | rendering + input + audio |

The single-writer rule is the concurrency model: anything that wants to mutate
orchestrator state from another task goes through a staged mailbox (spinlock'd
slot or PSRAM queue) drained on `tg_poll`, and anything that wants to read it
gets a snapshot rebuilt on `tg_poll`. Cross-task device actions (LED, config)
stage the other way - `tg_poll` → main loop. Because turns run synchronously on
`tg_poll`, a wedged turn freezes Telegram, routines and `/update`. The safety
net is therefore the head-loop caps and the per-round socket-budget clamp, plus
a main-loop reaper that collapses the ring arc after `deadline+120 s`.

## Lifecycle 3 - one turn (summary; full detail in turn-anatomy.md)

```
entry (owner msg | sub-agents finished | loop fired | post-OTA)   [tg_poll]
  → heap gate (12 K total-free floor + 5 K largest-block guard) → episodic user row (durable-first)
  → vector recall (embed + composite score + MMR)
  → composeInstructions: role/contract → identity+time → directive+running-memory
    → capabilities (live manifest + tool list) → running sessions
    → RECENT CONVERSATION (per-chat, one exchange per turn, append-only between
      boundaries, every turn after the fold anchor up to the head's history budget,
      read from the per-chat conversation cache: no card read on a steady-state turn)
      → scratchpad → recall (its own byte bound, and a per-turn floor so it always
      has room) → memory-howto
  → input: [MEMORY RESULTS] + [FRESH RESULTS] + [ACTIVE SESSIONS] + providers/models
    /connectors digest + [CHANNEL]/[USER] (or [SYSTEM]/[SCHEDULED LOOP])
  → host pick (resolveHeadHost: a keyed orchHost, else the first keyed provider) + per-chat conv chain
  → budget gate → head tool loop (≤8 rounds / 600 s wall clock / heap re-gate
    / byte budgets; each round's socket timeout clamped to the REMAINING budget;
    a round whose request would outgrow its body budget gives up, in one order
    (turn_fit.h), the arguments of calls that already ran and the older tool
    rounds, then recall and the attached skills, history before
    today and today's beyond the newest exchange - each named in the request's
    [LEFT OUT] note - and only then the newest round's results, the ones the
    model has not seen yet; nothing goes before a real refusal, and the summary
    pair and the newest exchange never go - so a long loop or a big write still
    answers)
      round: model → tool calls → registry dispatch (memory.*, web.search,
      device.control, files.*, loop.*, session terminate…) → results fed back;
      thinking + merged tool rows captured to episodic (glass box)
  → terminal orch_turn → parse (lenient) → applyTurn:
      memory (per-chat) → device[] (validated; protected keys refused;
      scheduled-turn rails) → mem_write/mem_query → session_ops spawn/terminate
      → reply/ask delivery → episodic assistant rows
  → retry/failover ladder ONLY if zero tools executed (side-effect replay ban)
  → turn end, once on EVERY exit: each host's billed usage, filed under that host and
    the model it billed with (TurnSpend), metered in one ledger write (attribution:
    turn/synthesis/loop:<id>); a failed turn's billed rounds included; the failure
    chain (each failing host's first cause, in order) rides the turn-end event
```

**Spend and causes per turn.** Every billed response is filed once, under the
provider and model that billed it, on every exit path: the fabric gives each host
its own counter, the legacy ladder each attempt, and the engine meters the whole
table once at turn end (`BudgetConfig::recordSpend`; one NVS ledger write per turn
on the device). `nimbus::orch::ledgerDelta` is the one place that knows what each
provider's counts mean: Anthropic leaves cache reads and writes out of its input
count, OpenAI, Mistral and Z.ai include them (so their input is split into uncached,
cached and cache-written, each at the model's own rate, else 0.1x and 1.25x of the
input rate), and Cumulo and a custom endpoint are billed at the input rate (Cumulo
splits by its upstream once the router bills cached tokens at their own rate, behind
`kRoutedBudgetLift`). A failed turn keeps a bounded
failure chain (at most three hosts, each host's first error and its attempt count,
160 bytes each): the owner notice names the first cause, and the turn-end row, the
error timeline row, the dream row, the routine record (`lastErr`) and
`device.status.lastFail` all carry it. The next turn's `[FALLBACK]` note says whose
turn switched, how long ago, the whole provider path and why.

Prompt v2: the system prompt has two variants, selected by the NVS `orchPromptV2`
flag (default off). v1 is the historical prompt. v2 compresses
the inlined orch_turn field docs to their essentials plus the rules a JSON schema
cannot enforce, de-duplicates the memory-tier explainer, and keeps every safety
rail; the wire schema (`ORCH_SCHEMA_BODY`) is unchanged. v2 is about a third
smaller (the default-fixture goldens `test/golden/orch_prompt_default.txt` and
`orch_prompt_v2_default.txt` are about 14.8 KB and 9.8 KB) and, in the A/B eval run
when v2 was introduced (`tools/eval_prompt_ab.py`), reached
task-success parity with v1 on Anthropic and OpenAI while billing roughly 1,200
to 1,900 fewer input tokens per turn. It stays OFF until enabled per device;
`composeInstructions` reads it from `ComposeInputs::promptV2` (wired from
`HarnessConfig::promptV2`). See `docs/proposals/harness-prompt-audit.md` for the
comparison and findings, and `tools/harness_prompt_cost.py` for the size
measurement.

Model routing: the head host is `orchHost` while it has a key, else the first
keyed provider in `providerPriority`, else the keyed router (Cumulo, then Z.ai),
else any other keyed provider (`resolveHeadHost`, the one rule the screen and the
web page also use). A pinned host whose key was removed is skipped, and the owner
is told once; with no key anywhere every turn answers "No AI provider has a key:
add one under Assistant > Models." without a request. The model can switch its own
host and model via the `orch_model` action (validated by the one model check, key
required; a miss answers `not in <provider> list cached <date>; refresh queued` and
queues one list refresh, at most one per provider an hour; a switch away from a head
that is paused, out of credit or over its budget is refused with the reason, since the
failover is already automatic and temporary, unless the owner asks again by name), but
keys, priority order and routing stay owner-only, both structurally (no setter
exists in the config contract) and by protected-key refusal. Failover walks up
to 2 keyed alternates; a fresh provider thread each time. A failed turn sends the
owner ONE message (switch notices are held and folded into it).

**One gate per provider for every paid seam.** Before any request, the chat head,
sub-agent spawns, embeddings, the photo caption step, speech, the download scan and
image generation ask the one per-provider park
(`nimbus::orch::providerPark().available(provider, seam, ...)`, or `peek` for a read
that may send nothing): a provider closed by a billing refusal or a refused key -
learned on any seam - or held by the pause switch gets no request. The Cumulo router
closes it only for the reasons its refusal contract says park
([provider wire](provider-wire.md)). The park lives in `lib/core` and is saved to a flash file by
the device (`src/agent/provider_gate.cpp`), so it survives a restart; the spend
ceiling freezes paid calls through the same pause switch.

**Daily spend warning.** Each provider has a default daily warning threshold: the
larger of 5 USD and three times its median spending day over the last 30 days of
the usage history (a fresh device: 5 USD), or the owner's own cents limit when one
is set. The turn's budget gate checks it, and when a day's spend at a provider
crosses it the owner gets one plain message for that provider that day (never from
a quiet background turn; the next owner turn says it).

## Lifecycle 4 - the self-driving layers

- **Compaction (the fold)** (see [compaction.md](compaction.md) and
  [architecture/fold-ladder.md](architecture/fold-ladder.md)): one shared fold
  driver on the tg_poll pump (the engine thread on the VN) summarizes a chat when the
  history after its anchor reaches the head's soft mark (the 48 KB default follows the
  model; on the 85 KB floor it is v5.1.1's 48 KB), after 200 messages, or when a shift
  hid rows; `/compact` and Summarize Now on demand. One rung a pass down the ladder
  (the head, the owner's priority list, the router only on a router-only device, the Sub
  model); a
  failed cycle retries 15 min, 1 h, 4 h, then every 12 h and never pauses for good;
  12 paid automatic requests a local day. A slice whose rows cannot be read yet waits
  and the anchor never moves past a row no summary took. Anchored summary, then the
  provider-thread reset, then the `## CONVERSATION SUMMARY` prompt section; state is
  rollback-safe on LittleFS.
- **Sub-agents** (see [sub-sessions.md](sub-sessions.md)): fire-and-forget
  background workers on provider fabrics; ≤6 tracked / 4 in-flight; polled
  ~15 s; once every sub has finished, one unattended synthesis turn reports
  (raw-delivery fallback 60 s after that drain, so results are never lost).
- **Routines**: owner- or agent-created `{schedule, prompt}`
  records fire scheduled turns. The fire decision is a pure, hard-coded gate
  table (enabled → approved → breaker → clock → due → chat-allowed → per-loop
  fires/tokens → device window/tokens) - never LLM-judged. At-most-once:
  `nextRun` advances and persists before the fire; no backfill after downtime.
  Auto-pause (with one owner alert) on repeated failures, daily token cap,
  chat de-allowlisting, or 5 identical results. Agent-created routines start
  unapproved and cannot fire until the owner approves, with one deliberate
  exception: a **one-time wakeup** (`wakeup.set`, schedule kind `once`,
  2 min–7 days out) is auto-approved, because it is a bounded single fire
  under the same daily fire/token governor, visible in `/loops` and the web
  Routines tab, and cancellable like any routine. It retires after firing
  (one 5-min retry if the turn itself failed, then an owner alert); at most
  4 armed at once; only an admin's conversation can arm one.
  **Delivery** (`nimbus/orch/loop_delivery.h`): a routine remembers the
  chat it was created from (an explicit `chat_id` wins, else the creating
  turn's chat: `web`, a Telegram chat, `voice`) and the fire is aimed at a
  resolved target, never `""`. No remembered chat means the owner's Telegram
  chat (owners list, else first allowlist entry) when a bot token is set, else
  the web chat. When the bot token is gone, the owner's own Telegram routine falls
  back to the web chat and the reply opens with a sentence saying so; any other
  unreachable Telegram chat (removed, tenant revoked, allowlist emptied, a
  member's chat) pauses, because a fallback turn runs as the web chat (admin).
  A loop's web-chat output is written to the web history and counted as an
  async delivery (`agent::deliverToWebChat`): an open web chat re-renders its
  history within its idle watch (every 30 s), and the output is never any turn's
  reply nor the legacy single-slot `GET /api/chat` reply (either would hand it to
  the owner's next message). A loop turn never fabricates a "Done.".
- **Dreaming**: a reserved, undeletable (pause-only) daily loop at 03:30 local
  (the device timezone - Settings → Mode & identity). The whole night is one pure
  pipeline, `dream::runNight` (`lib/harness/src/dream.cpp`, host-tested); the device
  glue only binds its seams. Stage 1 (no LLM): vector decay + expiry prune +
  windowed dedup + persist, at most once per local day, skipped entirely if the
  clock never synced. Stage 2: one quiet reflection turn over yesterday's episodic
  digest (the owner's conversation only: captured job-engine output and /clear
  markers are left out, as in every model-facing conversation read; the read
  pages past them, up to 192 rows) - distill up to 7 durable facts (0 is correct on
  a quiet day), groom the scratchpad, stay silent unless something needs the owner.
  The reflection sees and can call the memory tools only (`memory.write`, `update`,
  `search`, `pin`, `scratchpad`, `episodic`); its orch_turn `device`, `spawn`,
  `await` and `session_ops` fields are dropped. When embeddings are unavailable the
  turn is told so and does not try memory calls. On a provably quiet night (empty
  24 h digest AND the scratchpad unchanged since the last dream) the paid stage-2
  turn is skipped; stage 1 still runs.
  - **Outcome.** The `ev:dream` row says what happened: `ok` only when the turn ran
    and every memory write landed (with counts), `skipped`, `deferred`, `FAILED`
    (with the cause and the write counts), `degraded` (writes failed or memory was
    unavailable) or `skipped tonight` (deferred until the window closed).
  - **Retries.** A deferred or transiently failed reflection (low memory, timeout,
    network, rate limit, provider 5xx) retries 15 min later, up to 3 attempts, until
    07:00; key, credit, budget and parse failures wait for the next night. The
    breaker never disables the dream; after 3 failed nights in a row (then every 7)
    the owner is told once, at their next conversation, never at night.
  - **Quiet.** The dream messages nobody while it runs: its failure, provider-switch
    and billing notices are held, and so are its own reply, question and correction
    lines, with no sound or attention cue. Nothing is ever held for the owner's next
    message. Instead, when the night (or any background turn: a routine, a loop, a
    sub-agent result) has something worth telling the owner during quiet hours
    (22:00 until the owner's quiet-hours-end setting, default 09:00 local), it arms
    ONE self-wakeup through the routines scheduler: a one-time loop record, created
    without approval, at most one pending per kind, due at the end of quiet hours
    (within 24 h), listed with the routines and cancellable like any of them, gone
    after it fires, and persisted so it survives a restart. Its only job is one
    short turn with no tools that change anything: at wake time the note is
    re-checked - dropped when stale (its window passed, e.g. a brief about delays
    that ended) or already delivered by a routine - otherwise sent in plain words
    saying what it is and when it was noticed ("Noticed at HH:MM:"). The upkeep's
    firmware-worded facts (storage not saved, memory writes failed) ride the same
    path when the reflection itself said nothing. The failing-streak alert is the
    one thing still told at the owner's next conversation, never at night.
  - **A night the restart cut short.** The reflection turn writes an in-flight
    marker (night, attempt, start time) into `/data/dream.json` before it starts and
    clears it with its outcome. A boot that finds the marker without an outcome
    reconciles it: the night's verdict is fail ("the device restarted during this
    run"), one timeline row says the dream was interrupted, the failing-night streak
    and the routine's failure count move exactly once (the routine record's
    `lastErr` names the restart, never an older night's cause), and the interrupted attempt
    is one of the bounded retries - the routine runs again inside the night's window
    while it is still before 07:00 local. The reconcile waits until internal storage
    is mounted, the clock is set and the timeline and routine records are up. A state
    file that is torn, not JSON or larger than the state can be holds no marker
    (nothing is invented for it), a day stamp dated after today reads as never, and a
    marker older than the routine's last run (a later fire, on this firmware or
    another, already recorded that night) is cleared with no row and no count; one
    started before the clock was set has no comparable time and is reconciled.
  - **Gate.** 10 min quiet + no jobs + heap, deferring 15 min otherwise. Busy defers
    are capped at 12 per night; the heap floor is not.
  - **State.** `/data/dream.json` (LittleFS): the maintenance day, the quiet-night
    baseline, tonight's attempts, the failing-night streak and, while a reflection
    runs, its in-flight marker. The legacy NVS key
    `dreamScrHash` is read once to migrate and never written. The file is read once
    per boot; after that RAM holds the state and each save is for the next boot, so a
    file area that is not mounted or refuses writes cannot reset the attempt count,
    rerun maintenance or hide the streak. Such a night is not written at all when the
    file area is not mounted, and its row ends `state not saved (storage not mounted)`
    or `(storage write failed)`. `/api/state` reports `fsMounted` and the last night
    (`dream`), and the health report has an internal-storage row.
  - `DREAM` console forces it, including through the quiet-night skip. The console
    replies `DREAM queued`; the night runs on the turn task and logs
    `DREAM fired ok=<0|1> detail=<...>` when it ends.
- **Quiet background turns** (the dream): every
  engine notice - failure replies, provider-switch and budget notices - is held
  back and logged instead of sent; the outcome is recorded where the owner reads
  it later (the timeline row, the routine's last error). A turn's own reply still
  delivers if the model decides the owner must see it.
- **The update notice is a fixed string**
  (`nimbus/orch/unattended_policy.h` `updateNoticeText`, `TurnEngine::recordNotice`): "Updated to
  <version>" and the release's one-line note, with no model call and no tools (v5.2.0, lane RB-Q2a: the
  model-written post-update turn rewrote a day the owner had just corrected). The OTA hook stages the text;
  the poll task resolves the owner's chat, sends it and writes its history row at once, with no effect tags.
  On a device with routines it adds one line naming `/loop grants on`.
- **Background work waits for the owner** (`nimbus/orch/owner_first.h`): an automatic fold waits
  for two quiet minutes, but at most 6 turns or 10 minutes, then starts right
  after a reply went out (a fold a context overflow queued runs at once). A
  message that arrives while either is already running waits for it to finish
  (the turn task is single by design).
- **The unattended policy** (`nimbus/orch/unattended_policy.h`, v5.2.0, ruling 76): one table,
  one row per class of turn nobody asked for (routine, owner reminder, the assistant's own wakeup,
  self-wakeup, sub-agent report, the nightly upkeep, the update notice), says which classes of tool it
  may call; `docs/security.md` has the table. A refused call is answered with one plain "Not run: ..."
  line at the registry's access seam on every route; the turn is offered only what its row allows; its
  end-of-turn fields are cut to the row (logged); its requests carry the provider's built-ins only where
  the row may spend and nothing where it may not. Sub-agents such a turn starts carry no owner connector,
  attach, chat context or project, inside a run's caps (`JobEngine::setSpawnOrigin`, `beginRun`).
- **Unattended-turn rails** (routines, wakeups, the dream, sub-agent results
  synthesis alike): the caller rule table
  (`nimbus/orch/action_rules.h`) refuses them every owner-attended action - the main
  model, `reboot`, the voice, provider-routing, safety-override and device-name
  knobs, routines, skills, people, standing-rule changes, stopping sub-agents and the
  owner's connected tool servers - at both the apply layer and the tool-dispatch
  seam, because sub-agent results and routine prompts are untrusted-content
  injection surfaces. The same table refuses members, guests and the LAN endpoint
  everything that changes the owner's device.

## Lifecycle 5 - interrupts (what device life does to the harness)

- **Mode switch / restart**: hard restart, no teardown. Routines can't double-fire
  (persist-before-fire) and daily cost counters can't be reset (rollover is
  clock-gated), which is exactly why scheduled turns may not restart the device.
- **OTA install**: runs alongside the live poller (stopping it once crashed the
  device mid-install); the ring becomes the progress bar; voice is refused;
  every phase writes a timeline row; after the post-update boot validates, a
  staged system turn tells the model what changed (release notes persisted
  across the restart in NVS), bounded and once the owner has been quiet for two
  minutes (see the quiet background turns above).
- **Voice hold-to-talk**: blocks the main loop (WDT suspended around record and
  STT); `tg_poll` keeps running and the transcript is injected as a normal turn.
  A waiting injected turn (voice, web or serial) ends `tg_poll`'s idle waits
  early (`nimbus/net/tg_poll_sched.h`): it cuts a long-poll that is
  still waiting for its answer short (offset untouched, so nothing is lost),
  ends the inter-cycle pause or a poll-error backoff, and skips the next
  cycle's poll - never two running: the cycle after a skip checks Telegram
  first with an immediate getUpdates, so Telegram is never starved. Same
  single task and TLS session. Once `tg_poll` reaches one of those waits the
  turn starts within a few tens of milliseconds instead of waiting out the
  ~30 s long-poll; it still queues behind a running turn, outbound sends and
  a poll reconnect that are already under way.
- **Low-battery deep sleep**: full stop; config persisted; wake by the 5-minute
  timer. All
  loop state re-derives at the next boot (no backfill), and the first 10 min
  post-wake never dream.

## Layering

```
src/agent (device glue: NVS, TLS, FreeRTOS, sinks)      ~thin
   ↓ injected contracts
lib/harness (agent::) - POLICY + ORCHESTRATION           host-tested
   ↓ composes
lib/core (nimbus::orch::) - MECHANISMS                   host-tested
   (turn contract, head loop, tool registry, memory engines, journal, loops)
```

Rules: `lib/harness` never includes Arduino, `src/`, or device headers; every
device dependency arrives through a contract at construction. `lib/core` never
includes `lib/harness`. Model-visible strings live in the harness (or below)
and are golden-pinned - never device-side.

## The contract set

| Contract | Header | Device impl | Test fake |
|---|---|---|---|
| Platform (clock/heap/delay/PSRAM alloc) | `nimbus/harness/platform.h` | closures over `millis`/`ESP` (orchestrator.cpp) | `test/support/fake_platform.h` |
| HarnessConfig (provider/loop/budget/tts) | `nimbus/harness/config.h` | `src/agent/store_config.cpp` over NVS `store::` | `test/support/fake_config.h` |
| HttpTransport (whole-body request/response) | `nimbus/harness/http.h` | `src/agent/transport_tls.cpp` (TLS, arbiter, CA policy) | `test/support/fake_http.h` (scripted exchanges); `test/support/scripted_provider/` (a scripted model in each wire dialect) |
| Channel (send/speak/allowlist) | `nimbus/harness/channel.h` | Telegram + TTS sinks | `test/support/fake_channel.h` |
| Fabric (sub-agent backends) | `nimbus/harness/fabric.h` | provider adapters via `adapter_factory` | `test/support/fake_fabric.h` |
| Tools | `nimbus/orch/tool_registry.h` + one MCP-dispatch closure | `memory::handleMcp` (Lock + persist) | inline recorders |
| Hooks (lifecycle observers) | `nimbus/harness/hooks.h` | SFX / lastturn capture / log breadcrumbs | recorders |
| hlog (log seam) | `nimbus/harness/log.h` | `alog` ring (`GET /api/log`) | `LogCapture` |

**Security rails are structural**: `HarnessConfig` has NO setter for provider
keys, `providerPriority`, or `orchHost` (human-only surfaces). Who may run each
device action and config knob (the caller rule table, `nimbus/orch/action_rules.h`)
and risk-note composition are portable policy in `apply.cpp`, pinned by
`test_harness_apply` and `test_action_rules` - the injected execution closures
contain no policy branches.

## Threading invariant

The harness is **single-task by construction**: every entry point
(handleMessage, pump/pollJobs, injectScheduledTurn) must be called from ONE
context - on the device that is the `tg_poll` task. Cross-task writes ride the
staged-mailbox pattern; cross-task reads use fixed-buffer echoes. The memory
`Lock` (recursive FreeRTOS mutex) stays in `src/agent/memory_subsystem.cpp`;
the harness reaches tools only through the injected dispatch closure, which the
device wraps in the Lock (`withMemoryLock`).

## Test pyramid

- **Unit/contract (host)** - `test/test_harness_*` (Unity, `pio test -e native`):
  fabric routing, config boundary, compose + PROMPT GOLDENS
  (`test/golden/orch_prompt_*.txt`), apply security rails, jobs state machine,
  schema/field-docs goldens (`orch_schema.json`). Golden workflow:
  `GOLDEN_UPDATE=1 pio test -e native -f <suite>` re-blesses; a missing golden
  is a FAILURE; drift dumps to `test/golden/out/` for a plain `diff`.
- **Integration (host)** - full-turn Rig suites (compose → provider →
  parse → apply against scripted fakes) and the day-in-the-life timeline.
- **E2E (hardware)** - HIL L5/L8/L10 (live providers, real keys) and
  **L12 mock-LLM** (`tests/hil/mock_llm.py` + `test_l12_mock_llm.py`): the
  device's keyless custom adapter pointed at a LAN mock OpenAI server -
  deterministic on-device turns through the real lwIP/TLS-arbiter stack, no
  provider cost.

Shared fakes are header-only in `test/support/` (PlatformIO compiles each test
dir separately - include them relatively).

## Engine 2 seams (the rebuild)

The plain engine of the harness rebuild is built beside the current one and is not
linked into any device image yet.

The host builds (`pio test -e native`, nimbusd, harness-lab) compile both engines. A
device env compiles one: `tools/engine_filter.py`, a pre-script every `[s3]` env runs,
drops each source listed in `tools/engine/engine2_only.txt` from an engine-1 build (the
env option `custom_nimbus_engine`, 1 when absent). Leaving the files unreferenced is not
enough: the device link loads every lib/core and lib/harness object, and `--gc-sections`
keeps the unwind tables of one whose code it drops. A lane that adds a lib/core
`orch_tx_*`, lib/harness `plain_*` or `providers/plain_wire_*` source lists it there in
the same commit; `tools/release_gate/check_engine_filter.py` fails otherwise, and on any
built engine-1 `firmware.map` that names an engine-2 object.

The seams are declared up front so each lane fills one body:

- `lib/core/include/nimbus/orch/tx/` - the transcript: items and their line codec
  (`item.h`), the RAM arena (`arena.h`), the log on storage over a `TxFiles` interface
  (`log.h`), the stub rules (`rules.h`), profiles and budgets (`budget.h`), digests
  (`digest.h`) and migration (`migrate.h`). Every function returns a typed
  `tx::Status`; a body not written yet answers `Err::NotImplemented` with its own name.
- `lib/harness/include/nimbus/harness/plain/` - the engine (`engine.h`), one exchange
  (`exchange.h`), the per-dialect wire interface (`wire.h`, reached through
  `plain::wireFor`), request composition (`request.h`), guards, delivery, and one header
  per module that hooks in (resume, compaction, rails, media, routines, agents, truth,
  documents).
- `plain/hooks.h` - the six seams the engine calls: `beforeDispatch`, `afterResult`,
  `beforeDeliver`, `afterDeliver`, `idleStep`, `bootResume`. Modules register into a
  fixed `HookSlot`; a seam runs its handlers in slot order, never registration order,
  and a slot holds one handler.

### Transcript lines and the arena

Every transcript item is stored as one line and never rewritten (`orch_tx_item.cpp`):
a JSON object with short keys, the kind letter `k` first and the sequence number `q`
always, then only the kind's own fields that differ from their defaults, and last
`"z":"<crc32>"` over every byte before it. The tail has a fixed width, so a line cut by a
power loss, glued to the next append or damaged in one bit fails its crc and is dropped,
never repaired; a line is at most 60,000 B with its newline, and a longer one is refused
whole. Strings are kept byte for byte (renderers repair UTF-8), with control bytes
escaped so a line never holds a raw newline. Readers skip keys they do not know, so the
format only grows. `checkLine` reads a line's kind, `q`, turn and time without decoding
its text, for the restart fill and for scans. `messageStamp` writes the send-time stamp
every rendered message carries, with the year and the weekday (`[2026-11-17 Tue 13:02]`),
from the device's one stamp function (`orch::day::stamp`, `day_start.h`, which engine 1's
per-turn clock line uses too). Between midnight and the device's day start (setting
`day_starts`, default 04:00) it also names the day still running:
`[2026-11-18 Wed 00:30 (day 2026-11-17 until 04:00)]`. With no clock it says
`[time unknown]`, never a 1970 date.

`TxArena` (`orch_tx_arena.cpp`) is the RAM side of one chat: a single block from the
allocator it is given (PSRAM on the device, 576 KB for a chat, sized to the full profile), records of a 32-byte
header and the stored line growing up, a table of 4-byte offsets growing down. Appends,
the restart fill (`appendLine`), lookups by `q`, reads of a line and dropping the items a
compaction folded all work inside that block: `test/test_tx_arena` counts every
`operator new` and proves 1,000 appends take none. `get` decodes into the caller's item.
`TxArenaPool` keeps three chats resident, least recently used first out, and reuses each
slot's block. `test/test_tx_item` round-trips every kind with every field set and drops
every cut and every flipped bit of a line.

### The transcript log

`TxLog` (`orch_tx_log.cpp`) keeps a chat's transcript on storage under
`/mem/tx/<chatKey>/`: append-only segment files `seg-000001.jsonl` rolled at 1 MB,
`head.json`, and bulk payloads in `b/<turn>.txb`. Storing an item is exactly one append of
its whole line, and nothing in a segment is ever rewritten. A power cut that tears an
append leaves bytes that readers drop (no line end, or a failed crc); the next append
after a restart starts with `~` and a line end, so it is never glued onto them and a tail that lacked only its line end is never completed.
`head.json` is only a cache with its own crc: at open it is trusted when the line it says
ends the open segment is there, then the lines written after it are caught up; when it is
missing, torn or stale it is rebuilt from the segments (newest segment first, until the
newest digest, pointer move, owner message and turn open are found). A cut between its
temp write and the rename leaves a state open accepts.

The restart fill is one sequential read in 8 KB slices from just past the newest digest's
`thru` to the end of the open segment, into the chat's arena; when the arena cannot hold it
all, the oldest turns are given up and reported, so the newest stay. `store` is the persist
point the engine calls for every item: into the arena, then the arena's bytes to storage.
When storage refuses (no card, lock, memory floor), the item stays in RAM flagged not
stored and the turn goes on; it is never written later. Chat keys are lower-case letters, digits,
`_` and `-` only, and a bulk handle (`b/<turn>.txb@<offset>+<length>`) names a turn number
and offsets, never a path, so nothing reaches outside the chat's folder.
`test/test_tx_log` covers reopening, the head cache, a cut at every append, torn and
partial writes, the bounded fill and the paths over every byte.

`test/support/scripted_provider/` is the scripted model these suites run against. It
sits at the `HttpTransport` seam, answers each request with the next scripted step in
the request's dialect (Anthropic Messages, OpenAI Responses, chat completions), records
every request and reads it back into one view per dialect, with prefix and pairing
checks. Running out of script fails the request. The same JSON script drives the
reference loop through `scripted_provider.py`, so one script can run both arms of a
parity check offline. `test/test_scripted_provider` proves the old engine's parsers read
its answers; `test/test_plain_seams` pins the stubs and the hooks registry.

### What a request shows: the stub rule

`orch_tx_rules.cpp` (`rules.h`) holds the rules that decide what the model is shown. Each is
a pure function of stored facts (no clock, no file, no global state), so a restart renders
the same request, and none of them drops anything without a line that names it.

- **Stub texts** are computed once when a result is captured and stored on its `r` line
  (`stubText`). Internal results: `[stub <tool> r<q> <bytes>B <lines>L <HH:MM>] <first 200 B>
  ... (full: view id=r<q>)`, with `path=` and `sha8=` for file tools. Outside results (web,
  fetch, connectors): `[stub <tool>(<args, 120 B>) r<q> <bytes>B outside content, not shown:
  view id=r<q>]`, built from the call and the size only, so outside content cannot ride into
  later turns through a stub. Every stub is one line of valid UTF-8 of at most 400 B.
  `longArgStub`, `photoStub` and `omissionLine` write the other three shapes; `stubTime`,
  `photoWhen` and `omissionDates` write their times, with the year in every date.
- **The pointer** moves only at turn open (`decideTurnOpen`), forward, to the last closed
  turn: on a cold open (over 270 s since the last request, no clock, or after a restart), or
  on a warm open when whole internal bulk above it passes 24,576 B (8,192 B on 85,000 B
  heads). At or below it only tier-2 bulk and outside results render as stubs; receipts stay
  verbatim until compacted (`showResult`).
- **Superseded reads** (`supersededBy`): a successful file write marks earlier reads of the
  same path stale; no other tool supersedes anything, whatever it stores. Each stale read is
  listed once, in the `sup` list of an `x` item.
- **Photos** (`visiblePhotos`): heads with file references keep the last two photo turns for
  two more owner turns, at most 4, until an hour idle or a restart; inline heads (routed and
  other 85,000 B heads too) carry the newest 2 to the next owner turn only, within 270 s and
  200,000 B of base64. The rest render as `[photo p<ref> <date> not shown: view id=p<ref>]`.
- **The overflow ladder** (`nextRung`, one rung at a time): stub closed turns, stub this
  turn's older bulk, omit the oldest whole turns as one named line (`omissionSpan` never
  reaches the newest 6 owner turns), then the size guard.

`test/test_tx_stub_rule` checks each rule over its class (every byte value, every write tool
and ledger shape, the photo table per head kind, every split of turns for the ladder).

### One exchange: the body writer, the cap and the retry policy

`plain_exchange.cpp` (`exchange.h`) sends one request of a turn and reads its answer;
`providers/plain_body.h` is the writer every plain wire renders its body with.

- **The body writer** (`body::BodyWriter`) writes JSON straight into the body string, with no
  JSON document in between. Every string is UTF-8-repaired, escaped strictly (no raw byte
  under 0x20) and held to 60,000 B of text: over it, system and owner text refuses the body
  (`Over::Refuse`, nothing is sent) and tool results, arguments and replayed prose are cut on
  a character boundary with a visible `[cut: kept K of N bytes]` note (`Over::Clip`). Stored
  JSON (schemas, call arguments, signed thinking) goes through `raw()`: read by a strict JSON
  grammar (one object or array, valid escapes, matched brackets), else refused.
  Photos are spliced into a string at send time (`splice()`), never copied into it.
- **The output cap** is main's load-aware cap (`output_caps.h`) for the real body inside the
  time the turn has left; the caller's `maxTokens` is never what is sent.
- **The budget**: a body over the head's cap is never sent; the caller's ladder climbs a rung
  (told the body's size and the cap, `ClimbAsk`) and the body is rendered again, or the exchange
  ends with nothing sent.
- **The check**: no body leaves with a string over 60,000 B or with invalid UTF-8.
- **Errors, by class**: no answer, 5xx and 429 send the same request again after 2 s, then
  8 s (at most two retries, each only when its wait and a request fit the turn's wall clock);
  a context refusal climbs the ladder to rung 3 and sends once more; a billing or key refusal
  parks the host; a cut answer is asked once more at the retry cap, then delivered with its
  status line, and none of its calls is dispatched (a retry with no more room than the cut
  request is not sent); anything else is one plain line. A routed head follows the router's
  refusal contract, and a routed 401 to 403 that names no reason never parks the router. A retry renders the same stored state, so it
  never runs a tool again.
- **Who may answer instead** (`pickFallback`, `qualifiedTargets`): a fallback host answers
  only a fresh attempt with no effect run and the head parked, and only a head the
  qualification table qualifies for the engine; ladder targets the table does not qualify
  are skipped.

`test/test_plain_exchange` injects every error class through the scripted provider with a
scripted clock.

### The plain loop

`plain_engine.cpp` (`engine.h`) is the engine-2 turn: the simplest agent loop, with
durability around it and nothing that reads, edits or appends to the model's text.

- **Turn open**: a call an earlier turn stored with no result (the turn stopped while it ran)
  gets an "outcome unknown" result first, so every request stays paired, and is never run
  again; then the owner message (u), the turn header (t) and the stub pointer's move (x), all
  stored before the first request. The binding's `TurnSetup` gives the registry, notes, head
  and wire once; the system text and the tools are composed once and kept for every round.
- **The loop**: render the stored transcript, post it (`runExchange`: retries, the cap and the
  ladder live there; rung 2 follows the open turn's rounds, rung 3 omits only the oldest turns
  the body needs to drop, sized by what each turn renders, and climbs again while still over; every omissible one only after a
  provider's size refusal), store what the model said (k, a, c) before anything runs. No call: that
  text is the reply. Each call: `beforeDispatch`, an intent item for an effect tool (the tool
  runs only when the intent is stored), one dispatch, the result stored (tier 1 inline, tier 2
  to the bulk file, its stub computed once) before the next request, `afterResult`.
- **Limits** (`plain/guards.h`, `plain_guards.cpp`): the only guards, scaled to the head from
  `TurnSetup::storedRounds` and `TurnSetup::cost`: 16 rounds (a stored orchLoopRnds of 8 or less
  reads as 16, 9 to 32 is honoured); 240 s soft wall clock; prompt tokens summed over the turn's
  rounds, cached ones included, at most max(4 x the head's target, 16 x its request size at the
  profile trigger; 24,000 tokens when the head does not say); spend, priced per request from the
  head's rates (cache rates default to 0.1x and 1.25x of the input rate), at most max(0.50 USD,
  16 warm requests plus one cold). Checked in that order at each round boundary; a trip writes one
  `[device] limit reached (rounds|time|spend|size)` item and sends one last round with tool choice
  none; its text is delivered with one status line naming the guard, and a last round that only
  called tools ends with the limit line instead. The soft wall bounds every request, the last
  round may run to the hard wall. A head with no rates is not priced: the spend guard measures
  nothing there (the e item's USD stays 0), rounds, time and size still bound the turn. An answer
  that reports no usage at all still counts toward size and spend: a floor of the request's bytes
  over 3.5 at the input rate, with the event `usage:missing`. The last round takes no hold. A guard's
  last round that ends with calls only, or fails, ends with the limit line naming the guard
  ("The turn reached its <slug> limit before a reply."), cause `limit:<slug>`.
- **Identical-call suppression** (`installSuppression`, HookSlot::Suppression): inside one turn,
  a call to a pure tool (by the class of the tool a `more` call reaches) with the same arguments
  as an earlier successful call is answered "not run again ..." instead of running, while that
  earlier result stays whole after this round's ladder step (a receipt always; a bulk result only
  while it is inside the newest two rounds) and no call that is not pure has been made since (a
  write, a poll, the clock: the world may have moved). Never across turns or attempts of a turn,
  never for a call whose arguments are relative to now (since, before, recent, hours, days, ...),
  never for a tool that is not pure, never from an error or outside result.
- **Deferred effects**: a tool asks for a restart, an install or a head switch by setting
  `ToolOutput::deferred` (and `deferredArg`); the loop never acts on it. It is reported in
  `TurnOutcome::deferred` when the call's result was stored on the card as a success (its bit set
  only once its argument is copied), and the binding runs
  them in `Deferred` order (head switch, install, restart) after `runTurn` returns, the reply
  delivered and the d and e items stored.
- **Endings**: a failed exchange ends the turn with its one plain line; a cut answer is
  delivered with the cut line and its calls are answered "not run"; `beforeDeliver` may hold
  the reply once (one device item, one more round). Then delivery, the d item, `afterDeliver`,
  the superseded reads (x), the e item (cause, counts, tokens, per-exchange sizes, change
  points, prompt and manifest hashes) and head.json. A result pairs with the OLDEST open call of
  its id (the loop and every wire alike).
- **Through `more`**: every rule keyed on the tool (effect class and intent, outside content and
  taint, the recent-files rebuild, superseded reads) sees a `more` call as the tool it reaches with
  that tool's arguments; the stored c item keeps the model's own call, which the next request
  renders and the result pairs with. A router call that reaches nothing knowable reads as outside
  and as an effect.
- **Tier 1 at capture**: a string argument over 1,024 B is stored as its long-argument stub (the
  call runs with the whole argument; the whole arguments go to the bulk file), assistant text
  over 12,288 B is stored as a stub with the text in the bulk file while the delivered reply stays
  whole. Results pair with their calls by the call's q, never by id alone.
- **The truth seam**: `beforeDeliver` asks every handler, also on a held turn and on a cut answer;
  the loop takes a Hold at most once a turn and never on a cut, and sends a handler's status line
  (the failed-write receipt) when the turn has none of its own. A held reply whose extra round
  fails is delivered with the failure's line.
- **Nothing to deliver**: an answer with no text and no call (empty content, a refusal) gets the
  exchange's plain line for a person; a routine's empty text stays its silence.
- **The day start** (RB-REC5) rides the turn: `TurnSetup::dayStartMin` from the host fills
  `TurnCtx::dayStartMin`, and a wire passes `in.turn->dayStartMin` to `tx::messageStamp` for
  every stamp.
- **Compaction** (RB-C2) shares the engine's head state, digests and restart omission
  (`head()`, `digests()`, `omittedThru()`); before each turn the engine takes its next q from the
  largest of its own, head.nextQ and the arena's last q + 1, so a digest stored between turns
  never collides with the turn's first item.
- **Storage**: every item goes through `TxLog::store`, with one exception: the gap note owed for an
  episode is first shown in RAM (stored like any item, not stored on the card); once storage is
  back, one more gap note goes to the card only (`TxLog::append`) so the stored transcript names
  the hole. That card item has no arena twin; its q is taken from the chat's q space, so the card
  holds a q RAM never had (RB-C2 and RB-X1 must not pair card and arena by q). An item the card
  refused stays in RAM; the model sees one device note at the next round boundary and the owner
  one storage line, once per episode. With no card the chat runs from RAM and no effect tool runs.
- **A full arena** (ruling 60): the loop never calls the summarizer to make room. At turn open
  (free bytes under the turn's reserve, min(32 KB, a half of the arena)) and between rounds (the
  arena refused a store) the oldest closed resident turn leaves RAM, with any late result of it;
  never an item of the open turn. Storage keeps it; one omission line names every dropped turn
  until a digest covers them; `omittedThru()` rises to the last dropped q, so the compaction
  driver (idle step only) folds them from storage.
- **What the owner gets** is decided in one place at the end of a turn: the reply, else a reply
  held before a failed or empty extra round, else one plain line (the limit line when the last
  allowed round only called tools, the provider's line for an empty answer). At most one status
  line, by precedence: a failed-write receipt is never replaced; then the device's own failure;
  then the turn's end (provider, cut, limit); then the storage line.
- **Exceptions**: a tool handler that throws (anything but a refused allocation) is an error result
  for that one call and the turn goes on; nothing wider is caught.
- **Memory**: a refused allocation anywhere in the turn is a designed state: the reply in hand
  (moved out of the exchange before anything is stored, never copied on its way out: delivery
  reads it by pointer) goes out with the memory line, which was built at boot and needs no
  allocation; nothing already sent is sent again (delivery records each part as it goes out and a
  retry starts after it); when the reply's own step is refused again, the memory line goes alone.
  Then the d item (tried once) and afterDeliver (once, whatever the d item did), the e item and
  head.json where storage allows; no refusal leaves `runTurn`. An item over the stored line's
  limit has its own true line (an owner message its own, and one with more parts or update ids
  than a line holds another, cause `too-many-parts`, reported in `TurnOutcome::end.cause` since no e
  item can be stored), a full arena with nothing left to drop the memory line. The e item is tried
  once, also on the refusal path. A drop bounded by a dangling call keeps that call's whole turn. The d item
  and afterDeliver run before the superseded-read scan. Proof: an allocator hook refuses
  allocation k and every one after it, and every allocation of S bytes or more, over five turn
  shapes (`test_a_refused_allocation_from_any_point_on_still_reaches_the_owner`). The turn's state lives on the heap; `runTurn`'s own frame is 240 B on the device
  compiler. A refused allocation ends the turn with a status, never an abort.

`test/test_plain_conformance/engine_tests.h` drives scripted turns on each dialect's stand-in
wire (`test/support/tx_battery/stub_wire.h`, replaced by the real wire as each lands).

### Delivery, the turn-end item and the dossier by reference

`plain_deliver.cpp` (`plain/deliver.h`, lane RB-E3) is the seam between the loop and a chat.

- **Delivery** (`deliverTurn`): the reply goes out as `nimbus::orch::telegramParts` makes it (the
  markup sanitiser, then the split at 4,000 B, at most 8 parts): the function the device's Telegram
  send and nimbusd call, so the host suites see the device's bytes. Each part is one
  `Channel::send`, in order; nothing is added to or removed from the model's text. A reply with no
  `<` that fits one part goes out as the engine's own string, with no copy, so a refused allocation
  never costs a short reply. Progress is recorded as each part goes out and a retry starts after
  `fromPart`. A part the channel refuses ends the reply there. A binding sends each part with
  `telegramPart` (HTML, plain resend), never through `telegramParts` again.
- **The status-line class**: after the reply, at most one harness-written message, as its own
  message, and only under a name of `kAllStatusLines` (the closed list of PRD section 8 item 9); a
  line under any other name is never sent (`test_delivery_bytes` sweeps the whole table).
- **The e item** (`turnEndItem`): the turn's facts, bounded so the line always encodes (an e item
  that cannot be stored leaves its turn open): lists capped at 256 entries with `events:cut` and
  `exchanges:cut` naming the cut, the oldest references, then the oldest exchanges, shed first when
  the line would pass 60,000 B. An omission line repeated by later exchanges is kept once.
- **The dossier by reference**: the e item keeps each request as a reference, not a copy: the
  turn's prompt and manifest hashes and, per exchange, the item range it rendered (every item the
  engine held, oldest to newest, with their count), the stub pointer, the ladder's state and
  omission line, the output cap, tool choice none, the effort and an fnv1a-32 of the path, body and
  spliced bytes (never a header). `rebuildRequest` copies exactly that range, line for line, from
  any arena that holds it (the live one, or one filled from the card), renders it through the same
  wire and checks the hash: the result is the request as sent, byte for byte. It refuses
  (NotFound) when the system text or tools no longer hash to the stored ones or the items miss any
  of the range, and (Torn) when the render differs; it never renders less (owner ruling 81). A full
  arena with nothing closed left to drop sheds the references (`shedReference`, event
  `dossier:shed`) before it would refuse the e item: they never cost a resident turn.
- **Progress on a long turn** (owner ruling 82 item 6; `ProgressReporter`, `installProgress`): a
  turn that runs long says it is still working and on what ("Still working (45 s, step 4): ran
  read_file."). The first line goes out after 20 s with no visible update; where the channel can
  edit, the same message is edited at most every 10 s and gets a final form ("Worked 74 s (9
  steps).") when the turn ends; where it cannot, a new line at most every 60 s and at most three a
  turn. Progress lines are their own class: they go through the binding's `ProgressDeps`, never
  through `deliverTurn`, are never stored as transcript items, never count as the answer or as
  the one status line after a turn, and never shorten a turn's work. `installProgress` ticks on
  each tool result and closes after delivery; the engine ticks the provider wait and a soft guard
  reached while the turn still progresses.

### Crash resume: intents, one resume, stored-not-sent replies

`plain_resume.cpp` (`plain/resume.h`, lane RB-X1) registers in the `bootResume` seam
(`HookSlot::Resume`); the rules it reads are portable (`nimbus/orch/intents.h`,
`lib/core/src/orch_intents.cpp`). The loop's own rule makes them possible: an effect call runs
only once its intent item (i) is stored after its call (c), and every result is stored before
the next request leaves. At boot, after the engine filled the arena from the card:

1. **Every call with no stored result is answered, and never run again.** Its intent is stored:
   "outcome unknown" (it may have taken effect). No intent (an effect that never started, or a
   pure or read call): "no result: the device restarted before this call finished; it changed
   nothing" (`texts::tool_results::kRestartedBeforeRun`). The model reads which and decides.
2. **The newest turn gets one action** (`intents::bootAction`):

| What is stored for the newest turn | Action |
|---|---|
| its e item | nothing |
| its d item, no e item | close it: the e item only (cause `resume:closed`) |
| the final answer (a, fin), no d item, an attempt left | send it with no model call, then d and e (cause and event `resume:sent-stored`); any turn, a routine's included |
| no final answer, a turn no person started (a routine, an agent report, a notice) | close it, never resumed |
| no final answer, a person's turn | one automatic resume: trigger `resume`, the same turn number, attempt + 1, through the engine's own `runTurn` (tainted when the turn already read outside content; its own items are never dropped to make room) |
| no attempt left, or the stored answer is only its stub | a person's turn: the plain crash notice (`nimbus::orch::crashNotice`) as the one status line, then the e item (cause `resume:notice`); any other: close it |

An attempt is left while fewer than one was made, or when exactly one was made and the stored
answer is that resume's own: at most one resume and one boot send per turn, then the notice.

- **The counter is written ahead**: before a resume or a send the attempt goes to head.json
  (`open_turn`, `attempts`), so a cut inside the resume itself is counted and the next boot
  sends the notice. When head.json cannot be written the boot sends the notice: never a resume
  it cannot count. A boot send the channel refused (nothing reached the owner) gives its attempt
  back and leaves the turn open; so does a refused notice: a later boot tries again. The RTC
  record is not used (a power cut empties it) and its layout is unchanged.
- **Little room**: with under 4 KB free in the arena after the fill, the boot stores no result in
  it (the engine's own turn open answers the open calls, dropping and naming closed turns as it
  does) and its d and e items go to the card only.
- **Twice, named**: the boot cannot tell a cut just before the send from one just after it when
  the d item did not reach the card. A reply that went out just before such a cut goes out again;
  the e item's event `resume:sent-stored` names every boot send.
- **Everything stored is carried** (owner ruling 81): the resumed turn renders the whole arena,
  so its first request carries every item stored before the cut (`test_tx_crash` asserts it at
  every append of the turn, clean and torn).
- **Deferred effects** (restart, install, head switch) run only after a turn's reply is sent and
  its e item stored, so a restart they cause is never resumed, and the boot never runs a deferred
  effect of a turn it closes. A resumed turn's own deferred effects come back in
  `ResumeReport::outcome.deferred` (`ResumeDeps::done`), which the binding runs after boot.
- The items the boot stores take their q above head.json's next q, the arena and the log (a
  card-only gap note keeps its q), and the engine counts a turn the boot closed as the last
  closed turn at its next open (the cold open moves the stub pointer to it).

### The Anthropic wire: what a request carries

`providers/plain_wire_anthropic.cpp` renders one Messages API request from the stored
transcript; the response half is RB-W1b's. Until it lands `renderWireFor` hands out the render
half (the property suites use it) and `wireFor`, which the engine uses, answers only for a
complete wire. Render is a
pure function of `RenderInput`: the same items and facts give the same bytes every time and
after a restart.

- **Envelope**: `model`, `max_tokens` (required: a render with no output cap is refused),
  `output_config.effort` when the head sets one, the three system blocks as given (an empty
  block is left out), the tools exactly as given, `tool_choice` auto, or none on the guard's
  last round. Headers: `x-api-key` (a routed head sends a bearer token instead), the API
  version, and the Files beta only when a photo goes by file id. No key rides a cleartext
  socket.
- **Messages** come from the items after the newest digest's `thru`, in stored order. An
  owner message opens its own user message: its stamped text, then each photo, as pixels
  when the open turn's `t` item lists it as visible, else its stub line. Assistant text,
  calls and the open turn's signed thinking go in the assistant message; results and device
  notes in the user message after it. Closed turns' thinking, whitespace-only text and the
  bookkeeping items (`t i x s d e`) are not sent. A block joins the message before it only
  in the same role and turn, so a later item never rewrites a message already sent.
- **Pairing, whatever was stored**: a call is sent only with its one result and a result
  only after its call; dangling calls, orphan or second results and repeated ids are left
  out; the results of a round open the next user message, and anything stored between a
  call and its result waits until they are sent. Arguments that are not one JSON object (a
  cut call) are sent as `{}`. An id the API cannot take is sent in a mapped form, the same on
  the call and its result.
- **Four cache points**: the end of the notes block (the core when there are no notes), the
  end of the system, the last block of the previous turn and the moving last block; never on
  a thinking block. Bytes before a point do not change when a later item is appended.
- **Stubs and the ladder**: results render whole or as their stored stub by `tx::showResult`.
  The pointer is the highest of `RenderInput::stubPointer`, the newest `x` item's and the
  engine's `LadderState::closedThrough` (rung 1); `stubThrough` is rung 2; rung 3 leaves out
  every whole turn after `thru` whose items are all at or below `omitThrough` (never the open
  turn) and sends `omissionLine` (the engine's `tx::omissionLine` text) in their place, only
  when that line is set. The wire writes no text of its own.
- A refused allocation answers `TooLarge` with nothing rendered (ruling 30).

`test/test_plain_wire_anthropic` checks every body against a strict local model of the
API's request shape (counter-tested), over random transcripts with stored faults.

### The battery's property suites (layer L1)

Nine suites state what the plain engine must keep true before it exists:
`test_tx_prefix` (request N+1 extends request N except at a named change point),
`test_tx_pairing` (every call keeps exactly one result), `test_tx_coverage` (no stored
item is dropped silently; compaction leaves no row uncovered), `test_tx_stub_rule`,
`test_tx_log`, `test_tx_crash` (one reply, no effect twice), `test_tx_overhead` (bytes
around messages are bounded), `test_plain_conformance` (the loop per dialect) and
`test_delivery_bytes` (the reply arrives as the model wrote it).

A case that waits for an implementation starts with
`TXB_WAITS_FOR("PENDING RB-<lane>", <dependency lanes>)` from
`test/support/tx_battery/pending.h`: while a sentinel body of one of those lanes still
answers `NotImplemented`, the case reports `TEST_IGNORE_MESSAGE("PENDING RB-<lane>")` and
is listed in `test/pending_roster.txt`. Once every body it needs exists the case runs by
itself, and `tools/release_gate/check_pending_roster.py` fails until its roster line is
removed, so the roster only shrinks. Built with `-DTXB_FORCE_PENDING`, every pending case
fails at its first stub, naming it. Each property is a pure checker in
`test/support/tx_battery/props.h`, counter-tested on hand-built good and bad inputs (and a
reference renderer with ten wrong variants in `toy_render.h`), so each one is shown to go
red. The rest of `test/support/tx_battery/` is plumbing (a scripted chat through the
engine in `driver.h`, cuttable storage in `mem_files.h`, a fake tool registry in
`world.h`) that the implementing lanes complete; they do not edit the properties.

`tools/battery2.sh --tier free` runs the whole native suite, the roster gate over it, the
host builds and the static gates, each with its real exit status, and lists gates whose
lane has not landed as `PENDING <lane>`; the `live` and `bench` tiers say which lane
builds them. CI runs the roster gate after `pio test -e native` (`checks.yml`).

### Tool classes and `more`

Every tool has a class in the registration spec (`lib/core/include/nimbus/orch/tool_groups.h`):
its group (empty for the always-on list, otherwise the group behind `more`), the taint
of its output and its effect class (`pure`, `read` or `effect`). The engine's rails read
the class, never the tool's name: a turn that saw an outside result is tainted, only a
pure tool may be answered by identical-call suppression, and only an effect tool gets an
intent item first. `ToolRegistry::classOf(name)` is the one query. It reads a class
declared with `setClass` (a tool registered at runtime), else the class table in
`lib/core/src/orch_tool_groups.cpp` for the group and effect class and the taint table
(`orch_tool_taint.cpp`) for the taint. A tool with no row reads as group `connectors`,
outside content, with effects.

`more({group, name, args})` lists a group's tools with their arguments, lists the groups
when called with nothing, or calls one tool by name. `registerMore(reg)` adds it; nothing
in the old engine does yet. A call through it is a full `ToolRegistry::dispatch` with
the caller's own Principal (policy, access rules with the real arguments, live gates,
the handler's checks and the unknown-argument note), and an admin-only tool is neither
listed nor callable for a non-admin, so a person can never reach through `more` a tool
they could not call directly (`test/test_tool_groups` proves it for every registered
tool and caller class). A group's listing is at most 2,048 B; the tools index
(`toolsIndex`) is at most 512 B and names each tool with its required arguments; an
unknown name gets back the callable names, at most 640 B. The before-dispatch seam reads
`callClass` or `moreTarget` to see the tool a `more` call reaches.

#### The old engine's request ceiling (RB-Q3b)

The old engine keeps every tool in its tools array in v5.2.0. RB-Q3b measured hiding sixteen admin
tools behind `more` (2,400 live episodes on claude-haiku-5-5 and gpt-5.6-luna): only two groups met
the 95% first-call bar, so ruling 65 kept the manifest as it is; the parity runner measures `more`
for engine 2.

The request's weight is held by `tools/release_gate/check_prompt_ceiling.py`: the native suite
`test_e1_request_ceiling` sends an owner text turn of a synthetic twin of a busy owner device
(`test/support/fixtures/lumi_state_prompt`) through the real engine and Anthropic adapter and
pins the figures in `test/golden/e1_request_ceiling.txt`. The gate holds five of them at the
measured figure plus 128 B: prompt (the system blocks and the per-turn input), prompt_demand (the
same at the largest context budget an owner can set, where nothing is clipped), text (prompt and
history), tools (registry tool declarations) and tools_wire (the tools array as sent); the prompt
also stays under its 30 KB cap. It proves every description and
schema in the fixture's `device_tools.json` matches the source's `add(` argument exactly (literals
fixed, run-time pieces as holes) and that every tool the device registers is held. A lane that adds
bytes on purpose adds `tools/release_gate/prompt_ceiling.d/<lane>.json` (`lane`, `why`, and
`<figure>_bytes` for each figure it grows; over 1,024 B a `ruling`) in the same commit.

### Digests, their archive, recent files and the chatsum mirror

`lib/core/include/nimbus/orch/tx/digest.h` (body `orch_tx_digest.cpp`, lane RB-C1) holds
what keeps a long conversation in the request without the old summary. A compaction
writes one digest per local day as an s item, one line of at most 768 B that starts with
the device's day header (`digestText`: "Tue 17 Nov 2026: ..."). The digest block is the
newest digests that fit the profile's cap (full, the default, and standard and wide 6,144 B;
the owner's smaller tight 2,048 and lean 4,096), oldest first, one per line (`digestBlock`). It is a pure function of the
digests: it stays byte-identical until a compaction appends a digest at its end, and
only a move to the archive changes its start.

`settleDigests` moves the oldest digests that no longer fit to
`/mem/tx/<chatKey>/digests-YYYY-MM.md` (the month of the digest's day), one line each,
oldest first, and stops at the first refused write; what did not move stays live and
moves at the next settle. A line is appended only when the month file does not hold it
yet, and a torn last line is closed before the next one, so a cut at any step leaves
every digest in the block or in the archive, once. After a restart `loadDigests` reads
the segments newest first until it reaches the newest archived digest (the first digest
it meets that is the last whole line of a month file, so a clock set back cannot fool
it); a chat with no digest reads nothing.

The recent-files list (`recentFilesList`, at most 600 B) is the device's own: path and
last write, newest first. `RecentFilesBlock` rebuilds it at a cold turn open and at the
close of a turn that moved or deleted a file (`changesFileSet`), and keeps it byte for
byte otherwise. A rollback to v5.1.1 still has a summary: `mirrorChatsum` writes this
chat's record of `/data/chatsum.txt` in the v5.1.1 FoldStore format, with the newest
digests that fit 4,096 B as its summary, the counters, failure count and pause at 0, and
a row pointer ("m%08x") that never moves back; other chats' records stay byte for byte.

### Compaction: when, the summarizer, the swap

`lib/harness/include/nimbus/harness/plain/compact.h` (body `plain_compact.cpp`, lane
RB-C2) turns the oldest whole turns into day digests when the transcript after thru
outgrows its budget. `CompactDriver::step` is one unit of idle work (registered on the
idle seam's Compaction slot by `installCompaction`): at most one summarizer exchange and
its swap. It never runs while an owner message waits.

When: the bytes a request renders of the transcript after thru (each owner, assistant, call,
result and device-note item as it shows, a result whole or as its stub, plus its wire framing;
not the stored lines, which also hold each result's stub and only a bulk result's handle) are
above H, the whole prompt of the last turn's last request (the e item's `lp`: input plus cache
reads and writes in each provider's meaning, never the turn's sum) reached the head's due mark in
tokens (`tx::budgetFor`: the aim, about 100,000 tokens, less the next request's growth; see
[transcript budgets](reference/transcript-budgets.md)), or `/compact` asks. No hour makes it due:
the night fold under H is gone (owner ruling 81, ruling 83 item 8); a quiet hour only runs a
compaction that is already due. The marks are the head's (`CompactDeps::headKey`) at its stored
profile, full when none is stored; with no head named nothing is compacted automatically; an automatic step also needs 120 s of
quiet and a request left in its idle window (two per window; a window ends at a turn or
after an hour). What (`planSlice`): whole turns from thru forward, all of one local day
(the day starts at 04:00 by default), at most 24 KB of summarizer input, and only while
what stays after the slice is at least L and holds at least the head's v5.1.2 keep of owner
and assistant text (at full; owner ruling 81). Never a turn of the owner's current day (unless
that day alone is above H, then only its oldest half), never the newest six owner turns,
and never a turn after an open turn, an intent with no result or a running agent job: thru
never jumps over a turn.

Tools never eat the dialogue (owner ruling 81, ruling 91 table/F1). When a due slice is held
only by the text keep (a tool-heavy chat whose text is under v5.1.2's keep), no request is sent:
the results of the oldest closed turns render as their stubs from then on, oldest first, until
what a request renders is at L (`planToolFold`; one x item, why `tools`, lists them; each stub
keeps its view pointer). The arena's room is the one real limit besides the request: while it
has under 128 KB (or a quarter of it) free, a compaction is due and a slice may leave under L and
under the text keep, until it has freed twice that room, with one log line naming the text kept;
so the engine never has to drop whole turns from RAM with no digest. Turns that did leave RAM (a
full arena's drop while compaction could not run, the restart fill's give-up) are sliced first:
read back from the card (at most 96 KB a step) and summarized like a manual run, one day and one
input at a time; only when the card cannot be read does the slice merely name them.

The summarizer gets owner and assistant text verbatim with the local time and one
bracketed line per tool result that names the tool and nothing else (`[file change]`,
`[file work]`, `[outside content read]`, `[<tool> done]`); never a call's arguments, a
result's bytes or a device note. A turn that read outside content has its assistant text
inside the `<data>` frame, and its digest is stored inside the frame, so the block shows
it framed. The rungs come from the binding: the head host's small model twice, then each
other keyed provider in the owner's order, waiting 10 min, 30 min, 2 h, then 6 h after each
failure; at most six attempts start a day on the device (`/data/compact_ctl.txt`, written
before the request), each capped at 400 output tokens and 45 s. An answer is taken only
when it was not cut and holds at least 120 B for an input of 2 KB or more; otherwise only
the failure count changes. There is no pause state.

The swap keeps RB-C1's order: the s item is stored first (a refused store leaves thru
where it was), then thru moves and the arena drops what the digest covers, then
`settleDigests`, `writeHead` and the chatsum mirror. A cut at any write leaves every item
either after thru or under exactly one stored digest. `/compact` (`requestManual`) skips
the quiet wait, the backoff and the cap (it is counted), takes at most eight requests and
is answered once. The device binding is `src/agent/plain_fold_device.cpp` (engine-2
images only); the hosted one is `hostedTextIO` in `nimbusd/src/hosted_fold.h`.

### Migration into the transcript and the re-entry rule

`lib/core/include/nimbus/orch/tx/migrate.h` (body `orch_tx_migrate.cpp`, lane RB-MG1a) moves
one chat's conversation from the old engine's store into `/mem/tx/<chatKey>/` when engine 2
starts, and brings it up to date at every later start. The old store (the day streams and
`/data/chatsum.txt`) is only read: the binding hands in a `RowSource` over the chat's rows and
the chat's FoldStore record (`OldFold`: the summary and its two pointers).

- Under the old summary's pointer (thru): one anchor note, then digest 0 covering it. Digest 0
  is one small-model call through the compaction driver (`plain::digest0Asker`: the old summary
  as one closed turn, its input built by `planSlice`, the summarizer text, the driver's own
  acceptance). It is kept only when it holds no path-like token, no figure that a tool row of
  those rows holds and no owner row does, and at most 768 B per day covered. Otherwise the old
  summary is exported to `export-pre-v2/summary-<day>-<chatKey>.md` and the anchor note stands
  in as the omission line, naming that file. Digest 0 is asked at most twice in all.
- After the pointer: one import batch. Owner and assistant rows become text items (u with the
  row id in m, a), one turn per owner message, closed by an e item; tool, trace and log rows
  are not replayed. The newest rows that fit the import budget (60,000 B by default) are
  imported and the older ones are named in the batch's note. The rows between the two old
  pointers are the oldest after thru, so the compaction driver folds them first.
- The re-entry rule: `migrated.json` records the newest row accounted for. A later start
  imports only the rows after it that engine 2 did not write (its own day-stream rows carry
  the tag `e2`), as one more batch behind one note: "[device] N turns between t1 and t2 were
  answered by the earlier engine; their tool work is not shown." A start with nothing new
  writes nothing.
- Resumable at every step: each step is planned into `migrated.json` (temp and rename, crc)
  before it acts, and its items are regenerated from the plan and compared line for line with
  what the segments hold, so a reset at any write goes on after the last stored line and never
  stores a row twice. A refused write or allocation stops the step with a status; the next
  call goes on. The binding runs the migration before the chat's engine opens its log and lets
  the engine store only after it answered Done.
- `dryRun` reports counts and anomaly classes only (rows, conversation and tool rows, days,
  rows under the pointer and between the pointers, rows to import and to name), and writes
  and asks nothing.

### Request composition and the text catalog

Every model-facing string of engine 2 lives in `lib/harness/include/nimbus/harness/plain/texts.h`,
in one namespace per class (core, device notes, stubs, tool results, probe, summarizer, tool
descriptions). `tools/release_gate/gen_model_text_catalog.py` extracts them into
`test/golden/model_text_catalog.txt` with a byte ceiling per class (PRD section 4), and the
no-prompt-patch gate fails any change to a class without a ledger entry. A plain engine file
holds no prose of its own.

`lib/harness/src/plain_request.cpp` composes a request from `plain::RequestFacts`:

- `composeSystem` writes the three system blocks. The core (at most 3,072 B) is identity
  and channel, time zone, how to work, what you see, outside content, the recall line, the
  model line and the tools index (at most 512 B). No paragraph names a use case: what a
  person tracks and how is their own note or skill (ruling 41). The notes block is the skills index (at most 1,024 B, whole lines, with a
  true count of the rest) and the pinned notes (at most 4,096 B, whole lines, so core and
  notes stay within 8,192 B). The digests block is the digest block and the recent-files
  list. There is no clock in it: every message carries its own send time.
- `toolManifest` lists the always-on tools in one fixed order for every device and person:
  read_file, write_file, append_file, edit_file, list_files, note, search, web_search, view,
  send, schedule, calc, agent_start, then `more` (13 plus more). A tool is listed only when it
  is registered, always on in the class table and callable by the caller, at most 14 tools in
  10,240 B. delete_file is declared in `texts.h` too (`kBehindMore`) and listed behind more. write_file's schema states its host's content cap
  (12,000 B, or 4,096 B on a 60 s host).
- A sentence that names a tool appears only when that caller can call the tool
  (`uncallableNames`), so a guest is never told to schedule.
- The same facts give the same bytes: nothing else is read (no turn number, round, clock or
  taint), and each fact moves only its own block, so the provider's cache keeps hitting.
- A refused allocation fails the call with `TooLarge` and every output empty, so no partial
  system text or tools array is ever sent (ruling 30).

A binding registers each always-on tool with the declared bytes,
`reg.add(d->name, d->description, handler, plain::declSchema(*d, shortHost))` with
`d = plain::toolDecl(name)`; the class table decides what is always on, so the tools index
never lists a tool that is also in the array. `test/test_plain_request` proves the ceilings
for every role and host on a synthetic Lumi-shaped state, that no use-case word reaches any
text a model sees, and
writes the measured figures to `test/golden/plain_prompt_core.txt`, which
`tools/release_gate/check_plain_ceilings.py` gates.

### Plain file tools and reserved paths

`read_file`, `write_file`, `append_file`, `edit_file` and `list_files` are a library
(`lib/core/include/nimbus/orch/file_tools.h`, `path_rules.h`); a binding supplies the card
through `FileToolsIo` and registers the tools with the manifest's descriptions and schemas.
Each does what its name says:

- `read_file(path, offset, limit)` returns `<path> size=<B> lines=<n> sha8=<8 hex> bytes <a>-<b>`,
  a line break, exactly bytes `a` to `b`, and `next_offset=<b>` on its own line when more
  remains. Following `next_offset` rebuilds the file byte for byte at every read cap; a cut
  falls after a line end when one sits in the page's second half, else on a UTF-8 boundary.
- `write_file(path, content, if_exists)` creates a file; an existing one is replaced only with
  `if_exists=replace`. `append_file(path, text)` only appends. `edit_file(path, old, new, all)`
  replaces exact text; zero matches, or several without `all=true`, are errors. Every write
  states what changed (size, `sha8`, the bytes replaced or added).
- `list_files(prefix, after)` lists newest first, 40 at a time, and ends with `more: N`.
- An argument the tool does not take is an error naming the ones it does; nothing is written.

A path is exactly the store's two segments, `project/name`. A leading `/`, an empty segment,
`.`, `..` or a third segment is an error; inside a segment each character the store or FAT
refuses is replaced one for one (a Latin-1 letter folds to ASCII), and an over-long name
keeps its extension and gains an 8-hex hash. Results state the stored path. Reserved paths
(matched without letter case): `skills/<id>.md` reads the approved skill and `skill_save`
writes it; `export-pre-v2/*` is the read-only export the migration wrote; `inbox/*` and
`agents/*` are written by the device and agent jobs. A write there names the tool that owns
it. A path is reserved only because something other than the file tools writes it: a log or
table the person keeps, in any project, is an ordinary file. Ownership, shared files and the
per-person byte quota hold for every tool (`test/test_file_tools`).

Card time: every raw card op runs inside one `FileToolsIo::locked()` hold, holding at most
one op: reads take 8 KB per hold, part-file writes 4 KB, an append one op of at most the
content cap (12,000 B), the swap shares the commit hold with the index entry update, and
the index rewrite has a hold of its own. No hold carries the room check, the write, the
rename and the index rewrite together, which is what took `files::saveText` to 162 ms.

### General file aids (library)

Tracking of any kind lives in ordinary CSV files the owner keeps. Three general aids make
that reliable on small models; they are one library for both engines and nimbusd
(`lib/core/include/nimbus/orch/file_aids.h`), and the bindings only move bytes, so every host
gives the same result text for the same file:

- A CSV scan behind `read_file` `column`/`from`/`to`/`sum`. A row matches when its field is in
  the range (numbers compare as numbers; otherwise by prefix, so `2026-11-19` is a day,
  `2026-11` a month, and a key of another form is "not compared", never quietly out of range).
  The named columns are totalled as exact decimals (int64 thousandths, half away from zero
  past three places; past the int64 range is an error, never a partial total). The result
  shows the header and the matching rows as `L<n> <row>`, then one tail line: `rows <m> of
  <n>`, the sums, and every row or cell left out of a total by reason with its lines (blank,
  not a number, rounded, not compared, a field count unlike the header's, too long, empty
  lines, identical-line groups, a torn last line, a row cut inside a quoted field).
- A line pick behind `edit_file` `line`: the match that starts at that line and covers whole
  lines is the one replaced, so a stale number fails with the current match lines and never
  touches another row. Every result names the lines it changed, the lines that moved and where
  `old` still occurs. A removal (`new` empty) takes its line end.
- The facts an append states: a newline is written first when the file does not end in one,
  and an appended line identical to one of the file's last 64 lines is named (`; identical to
  line 41`); it is still appended.

No size cap: the scan and the edit read 4 KB per card call (one lock hold each) in bounded
working memory from `workingAlloc`; a refused block ends the call with a plain error and
changes nothing, and an edit swaps its part file in only when the second pass saw the file the
first pass planned on (`test/test_file_tools_sum`, `test_file_edit_line`,
`test_file_append_facts`).

The old engine's `files.read` and `files.edit` (device and nimbusd) run the same scan and
edit (`file_aids_e1.h`): `files.read` keeps `tail`, `sum` and `date` (the day total, on the
running day by default) and gains `column`, `from`, `to`; `files.edit` keeps its first-match
default, `count` and `all`, and gains `line` and the current line numbers. On the device the
scan gives the tool call's card lock up and takes it per 4 KB read
(`memory::withCardReleased`); the shared fixtures give `test/golden/file_aids_e1_results.txt`
on both hosts (`test_file_aids_e1`, `test_hosted_file_aids`).

## Extension points

- **New provider backend**: subclass `ManagedAgentAdapter` + one
  `adapter_factory` line + config entry. The head-turn side registers a
  `ProviderTurnFn`.
- **New tool**: `memory::registry().add(...)` - it surfaces automatically to
  the model (tool loop), the LAN MCP server (`POST /mcp`), and `/api/tools`.
  Give it a row in the class table (`lib/core/src/orch_tool_groups.cpp`: group and
  effect class), a taint row (`orch_tool_taint.cpp`) and an access rule
  (`orch_action_rules.cpp`); `test_tool_groups`, `test_tool_taint` and
  `test_action_rules` fail a registered name without them.
  A tool whose handler waits on the network must be listed in
  `nimbus::sys::toolWaitsOnNetwork` (`lib/core/include/nimbus/sys/web_guard.h`; a
  connected tool server's `mcp.*` tools are covered by their prefix): the dispatcher
  then runs it outside its card lock and the LAN endpoint refuses it.
  `test_web_guard` fails a registration that calls a network seam without the entry.
- **Hooks**: fill a slot in the `Hooks` struct at `begin()` - observers only;
  they must never block or mutate the turn.
- **Skills**: `skill.list`/`skill.get` capsules; SD-stored dynamic skills under
  `/mem/skills/<id>/SKILL.md` inject at spawn via `Directive.skill`. Since
  v4.0.0 the model can also WRITE them (`skill.save`/`skill.delete`): the device
  stamps origin and approval itself, an agent-written capsule stays inert until
  the owner approves it (web or Telegram `/skill approve <id>`), and built-in
  ids cannot be shadowed. Approval is asynchronous by design - a skill saved
  during a run is an investment in future runs, never a step in the current one.
  Since v4.1 every capsule may carry a one-line `desc:` in its front matter, and
  the assistant sees an ambient `[SKILLS]` index (id + description) in every
  turn - it reads the matching playbook with `skill.get` before starting a task
  the index covers, so recipes live in editable skills instead of prompt text.
  Built-ins ship a default description; an SD capsule with the same id
  overrides it (recipe fixes never need a firmware update).

## Invariants that must never regress

1. Turn contract (`orch_schema.h`) and `/api/*` are additive-only - the schema
   golden (`test/test_harness_goldens`) enforces this for the turn contract.
2. No retry/failover after any tool executed in a turn (side-effect replay).
3. Recall, synthesis, and the tool loop are fail-open - a memory/provider
   failure degrades the turn, never crashes or restarts the device.
4. `takeFreshResults` is the single reset point of the synthesis clock.
5. Steady-state internal-SRAM footprint does not grow: big buffers go
   through `Platform::allocLarge` (PSRAM) or stay on the device side.
