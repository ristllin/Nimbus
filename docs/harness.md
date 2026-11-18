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
    rounds, then recall, the food playbook and attached skills, history before
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
- **Quiet background turns** (the dream, the post-update awareness turn): every
  engine notice - failure replies, provider-switch and budget notices - is held
  back and logged instead of sent; the outcome is recorded where the owner reads
  it later (the timeline row, the routine's last error). A turn's own reply still
  delivers if the model decides the owner must see it.
- **The post-update awareness turn is bounded and waits for the owner**
  (`TurnEngine::injectBoundedTurn`, `nimbus/orch/owner_first.h`): at most two
  tool rounds and a 4 KB tool-output budget, and single-shot (no tool loop)
  while the head runs on a fallback provider. It starts only after two minutes
  with no turn and two minutes after it was staged; an automatic fold also waits
  for two quiet minutes, but at most 6 turns or 10 minutes, then starts right
  after a reply went out (a fold a context overflow queued runs at once). A
  message that arrives while either is already running waits for it to finish
  (the turn task is single by design).
- **Unattended-turn rails** (routines, wakeups, the dream, sub-agent results
  synthesis, the post-update system turn alike): the caller rule table
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
every rendered message carries, with the year (`[Tue 17 Nov 2026 13:02]`).

`TxArena` (`orch_tx_arena.cpp`) is the RAM side of one chat: a single block from the
allocator it is given (PSRAM on the device, 384 KB for a chat), records of a 32-byte
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
  same path stale; a `ledger_write` marks earlier `ledger_day` views of each day it touches
  (by its `day` fields, its receipt's day lines, or the row ids its ops name). Each stale
  read is listed once, in the `sup` list of an `x` item.
- **Photos** (`visiblePhotos`): heads with file references keep the last two photo turns for
  two more owner turns, at most 4, until an hour idle or a restart; inline heads (routed and
  other 85,000 B heads too) carry the newest 2 to the next owner turn only, within 270 s and
  200,000 B of base64. The rest render as `[photo p<ref> <date> not shown: view id=p<ref>]`.
- **The overflow ladder** (`nextRung`, one rung at a time): stub closed turns, stub this
  turn's older bulk, omit the oldest whole turns as one named line (`omissionSpan` never
  reaches the newest 6 owner turns), then the size guard.

`test/test_tx_stub_rule` checks each rule over its class (every byte value, every write tool
and ledger shape, the photo table per head kind, every split of turns for the ladder).

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

### Digests, their archive, recent files and the chatsum mirror

`lib/core/include/nimbus/orch/tx/digest.h` (body `orch_tx_digest.cpp`, lane RB-C1) holds
what keeps a long conversation in the request without the old summary. A compaction
writes one digest per local day as an s item, one line of at most 768 B that starts with
the device's day header (`digestText`: "Tue 17 Nov 2026: ..."). The digest block is the
newest digests that fit the profile's cap (tight 2,048, lean 4,096, standard and wide
6,144 B), oldest first, one per line (`digestBlock`). It is a pure function of the
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
  food paragraph when the food capability is on, the model line and the tools index (at
  most 512 B). The notes block is the skills index (at most 1,024 B, whole lines, with a
  true count of the rest) and the pinned notes (at most 4,096 B, whole lines, so core and
  notes stay within 8,192 B). The digests block is the digest block and the recent-files
  list. There is no clock in it: every message carries its own send time.
- `toolManifest` lists the always-on tools in a fixed order: read_file, write_file,
  append_file, edit_file, list_files, note, search, web_search, view, send, schedule, the
  pack (ledger_write and ledger_day with the food capability, else agent_start and
  delete_file), then `more`. A tool is listed only when it is registered and the caller may
  call it, at most 14 tools in 10,240 B. write_file's schema states its host's content cap
  (12,000 B, or 4,096 B on a 60 s host).
- A sentence that names a tool appears only when that caller can call the tool
  (`uncallableNames`), so a guest is never told to schedule.
- The same facts give the same bytes: nothing else is read (no turn number, round, clock or
  taint), and each fact moves only its own block, so the provider's cache keeps hitting.

A binding registers each always-on tool with the declared bytes,
`reg.add(d->name, d->description, handler, plain::declSchema(*d, shortHost))` with
`d = plain::toolDecl(name)`, and calls `plain::applyToolPack(reg, food)` after registering,
so the tools index never lists a tool that is also in the array. `test/test_plain_request`
proves the ceilings for every role, pack and host on a synthetic Lumi-shaped state and
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
writes it; `food/food-log*.csv` is written by `ledger_write`; `food-pre-v2/*` and
`export-pre-v2/*` are read-only archives; `inbox/*` and `agents/*` are written by the device
and agent jobs. A write there names the tool that owns it. Ownership, shared files and the
per-person byte quota hold for every tool (`test/test_file_tools`).

Card time: every raw card op runs inside one `FileToolsIo::locked()` hold, holding at most
one op: reads take 8 KB per hold, part-file writes 4 KB, an append one op of at most the
content cap (12,000 B), the swap shares the commit hold with the index entry update, and
the index rewrite has a hold of its own. No hold carries the room check, the write, the
rename and the index rewrite together, which is what took `files::saveText` to 162 ms.

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
