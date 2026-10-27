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
      boundaries, budget derived from the head's window) → scratchpad → recall → memory-howto
  → input: [MEMORY RESULTS] + [FRESH RESULTS] + [ACTIVE SESSIONS] + providers/models
    /connectors digest + [CHANNEL]/[USER] (or [SYSTEM]/[SCHEDULED LOOP])
  → host pick (resolveHeadHost: a keyed orchHost, else the first keyed provider) + per-chat conv chain
  → budget gate → head tool loop (≤8 rounds / 600 s wall clock / heap re-gate
    / byte budgets; each round's socket timeout clamped to the REMAINING budget;
    a round whose request would outgrow the 85 KB body budget shrinks the
    arguments of calls that already ran, folds the older tool rounds to one-line
    stubs and runs again, so a long loop or a big write still answers)
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
count, OpenAI includes them (so its input is split into uncached, cached and
cache-written, each at the model's own rate, else 0.1x and 1.25x of the input
rate), other providers are billed at the input rate. A failed turn keeps a bounded
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
queues one list refresh, at most one per provider an hour), but
keys, priority order and routing stay owner-only, both structurally (no setter
exists in the config contract) and by protected-key refusal. Failover walks up
to 2 keyed alternates; a fresh provider thread each time. A failed turn sends the
owner ONE message (switch notices are held and folded into it).

**One gate per provider for every paid seam.** Before any request, the chat head,
sub-agent spawns, embeddings, the photo caption step, speech, the download scan and
image generation ask the one per-provider park
(`nimbus::orch::providerPark().available(provider, seam, ...)`): a provider closed
by a billing refusal or a refused key - learned on any seam - or held by the pause
switch gets no request. The park lives in `lib/core` and is saved to a flash file by
the device (`src/agent/provider_gate.cpp`), so it survives a restart; the spend
ceiling freezes paid calls through the same pause switch.

**Daily spend warning.** Each provider has a default daily warning threshold: the
larger of 5 USD and three times its median spending day over the last 30 days of
the usage history (a fresh device: 5 USD), or the owner's own cents limit when one
is set. The turn's budget gate checks it, and when a day's spend at a provider
crosses it the owner gets one plain message for that provider that day (never from
a quiet background turn; the next owner turn says it).

## Lifecycle 4 - the self-driving layers

- **Compaction (the fold)** (see [memory.md](memory.md)): per-chat
  byte/message counters trigger a background summarization on the tg_poll pump
  (~48 KB default, `/compact` on demand) - anchored summary → provider-thread
  reset → the `## CONVERSATION SUMMARY` prompt section; deferrals never burn
  the breaker; state is rollback-safe on LittleFS.
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
    lines, with no sound or attention cue. Anything it had for the owner goes out
    once, with the streak alert when there is one, when the owner's next turn ends
    (RAM only: a restart before then drops it).
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
- **Scheduled-turn rails** (loops, dream, synthesis, post-OTA alike): refuse
  `reboot`, `ttsOn`, `devName`, `sleepOvr`/`brightOvr` and `loop.create`, enforced
  at both the apply layer and the tool-dispatch seam, because sub-agent results
  and routine prompts are untrusted-content injection surfaces.

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
| HttpTransport (whole-body request/response) | `nimbus/harness/http.h` | `src/agent/transport_tls.cpp` (TLS, arbiter, CA policy) | `test/support/fake_http.h` (scripted exchanges) |
| Channel (send/speak/allowlist) | `nimbus/harness/channel.h` | Telegram + TTS sinks | `test/support/fake_channel.h` |
| Fabric (sub-agent backends) | `nimbus/harness/fabric.h` | provider adapters via `adapter_factory` | `test/support/fake_fabric.h` |
| Tools | `nimbus/orch/tool_registry.h` + one MCP-dispatch closure | `memory::handleMcp` (Lock + persist) | inline recorders |
| Hooks (lifecycle observers) | `nimbus/harness/hooks.h` | SFX / lastturn capture / log breadcrumbs | recorders |
| hlog (log seam) | `nimbus/harness/log.h` | `alog` ring (`GET /api/log`) | `LogCapture` |

**Security rails are structural**: `HarnessConfig` has NO setter for provider
keys, `providerPriority`, or `orchHost` (human-only surfaces). Scheduled-turn
refusals (`reboot`/`ttsOn`/`devName`/`sleepOvr`/`brightOvr`) and risk-note composition
are portable policy in `apply.cpp`, pinned by `test_harness_apply` - the
injected execution closures contain no policy branches.

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

## Extension points

- **New provider backend**: subclass `ManagedAgentAdapter` + one
  `adapter_factory` line + config entry. The head-turn side registers a
  `ProviderTurnFn`.
- **New tool**: `memory::registry().add(...)` - it surfaces automatically to
  the model (tool loop), the LAN MCP server (`POST /mcp`), and `/api/tools`.
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
