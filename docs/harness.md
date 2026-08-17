# The Agentic Harness (`lib/harness`)

The Orchestrator's policy/orchestration layer as a standalone, host-testable
PlatformIO library. This is the machine entry point for working on the agent
loop - read it before touching `lib/harness/` or `src/agent/`.

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

1. **`otaupd::bootGuard()`** - FIRST line: burns one A/B boot attempt, self-
   reverts a bad OTA image after 3 boots (before any driver can crash it).
2. Board bring-up (`solide::begin`: SD, NVS, e-ink/LED/encoder tasks, battery),
   then two memory reroutes that everything downstream depends on: **mbedTLS →
   PSRAM** (TLS handshakes don't fit internal SRAM) and **malloc ≥128 B →
   PSRAM** (String/JSON churn).
3. Task watchdog configured (8 s, panic) but the main loop subscribes only at
   the END of setup - a slow SD/Wi-Fi boot must not watchdog-loop the device.
4. Config + mode from NVS (mode is boot-resolved; switching = reboot). In
   Orchestrator mode the BT controller RAM is released and BLE never starts.
5. Orchestrator starts Wi-Fi (STA + setup AP; a TFT drops the AP while STA is
   connected). Notifier instead keeps Wi-Fi off and starts BLE. Then
   **`memory::begin()`** (inside `beginWeb`) loads the vector DB from SD/LittleFS
   into PSRAM, scratchpad +
   retrieval config from NVS, and the episodic **hydrate** - day-stream JSONL
   files on SD are indexed and the last 512 messages rebuilt into the PSRAM hot
   ring, so per-chat context survives reboot with zero flash reads at runtime.
   A boot timeline row (`ev:boot` with reset reason + OTA result) is captured.
6. **`orchestratorBegin()`**: provider fabric + adapters, sinks, the job
   **journal restore from NVS** (active sub-agents re-attach and resume
   polling), running-memory load (LittleFS), Local Loops load
   (`/data/loops.json`, every `nextRun` recomputed from now), the dream loop
   ensured (insert-if-missing; an owner pause survives), and finally
   `telegram::begin` spawns **`tg_poll`** - the task every turn runs on. The
   task spawns even with no Telegram token, because web/serial/voice turns are
   injected through the same queue.
7. SNTP arms on the GOT_IP event (tz from NVS). Until first sync: the prompt
   says the time is UNKNOWN, wall-clock loops can't fire, dream maintenance is
   skipped, and pre-sync episodic rows are held and restamped at first sync.

## Lifecycle 2 - who runs what (task topology)

| task | watchdog | runs |
|---|---|---|
| main loop (core 1) | **8 s panic WDT** | rendering, ring, encoder, battery/thermal/OTA ticks, staged device actions, `reapStuckTurn`, retention prune (6 h) |
| **`tg_poll`** (core 0) | **none - deliberate** (its long-poll blocks ~25 s) | EVERYTHING agentic, single-writer: Telegram long-poll, every LLM turn (owner, synthesis, scheduled, post-OTA), the loops tick (20 s gate), the dream firing, the sub-agent dispatch/poll pump (~15 s), staged-mutation drains |
| AsyncTCP | none | all `/api/*` + `/mcp` handlers - they **stage** mutations and read snapshots, never touch agentic state directly |
| epd / leds / enc / sfx | none | rendering + input + audio |

The single-writer rule is the concurrency model: anything that wants to mutate
orchestrator state from another task goes through a staged mailbox (spinlock'd
slot or PSRAM queue) drained on `tg_poll`, and anything that wants to read it
gets a snapshot rebuilt on `tg_poll`. Cross-task device actions (LED, config)
stage the other way - `tg_poll` → main loop. Because turns run synchronously on
`tg_poll`, a wedged turn freezes Telegram/loops/`/update` - which is why the
head-loop caps + per-round socket-budget clamp (F25) ARE the safety, plus a
main-loop reaper that collapses the ring arc after `deadline+120 s`.

## Lifecycle 3 - one turn (summary; full detail in turn-anatomy.md)

```
entry (owner msg | sub-agents finished | loop fired | post-OTA)   [tg_poll]
  → heap gate (28-30 K floors) → episodic user row (durable-first)
  → vector recall (embed + composite score + MMR)
  → composeInstructions: role/contract → identity+time → directive+running-memory
    → capabilities (live manifest + tool list) → running sessions
    → RECENT CONVERSATION (per-chat, ≤12 msgs/3000 B) → scratchpad → recall → memory-howto
  → input: [MEMORY RESULTS] + [FRESH RESULTS] + [ACTIVE SESSIONS] + providers/models
    /connectors digest + [CHANNEL]/[USER] (or [SYSTEM]/[SCHEDULED LOOP])
  → host pick (orchHost || first of providerPriority) + per-chat conv chain
  → budget gate → head tool loop (≤12 rounds / 600 s wall clock / heap re-gate
    / byte budgets; each round's socket timeout clamped to the REMAINING budget)
      round: model → tool calls → registry dispatch (memory.*, web.search,
      device.control, files.*, loop.*, session terminate…) → results fed back;
      thinking + merged tool rows captured to episodic (glass box)
  → terminal orch_turn → parse (lenient) → applyTurn:
      memory (per-chat) → device[] (validated; protected keys refused;
      scheduled-turn rails) → mem_write/mem_query → session_ops spawn/terminate
      → reply/ask delivery → episodic assistant rows
  → retry/failover ladder ONLY if zero tools executed (side-effect replay ban)
  → usage summed across rounds+failovers → ledger (attribution: turn/synthesis/loop:<id>)
```

Model routing: the head host comes from `orchHost` (or the first
`providerPriority` entry); the model can switch its OWN host/model via the
`orch_model` action (validated against the choice list, key required), but
keys, priority order, and routing stay owner-only - structurally (no setter
exists in the config contract) and by protected-key refusal. Failover walks up
to 2 keyed alternates with an owner notice; a fresh provider thread each time.

## Lifecycle 4 - the self-driving layers

- **Compaction (the fold)** (see [memory.md](memory.md)): per-chat
  byte/message counters trigger a background summarization on the tg_poll pump
  (~48 KB default, `/compact` on demand) - anchored summary → provider-thread
  reset → the `## CONVERSATION SUMMARY` prompt section; deferrals never burn
  the breaker; state is rollback-safe on LittleFS.
- **Sub-agents** (see [sub-sessions.md](sub-sessions.md)): fire-and-forget
  background workers on provider fabrics; ≤6 tracked / 4 in-flight; polled
  ~15 s; results coalesce 3 s into one unattended synthesis turn (60 s raw-
  delivery fallback so results are never lost).
- **Local Loops (routines)**: owner- or agent-created `{schedule, prompt}`
  records fire scheduled turns. The fire decision is a pure, hard-coded gate
  table (enabled → approved → breaker → clock → due → chat-allowed → per-loop
  fires/tokens → device window/tokens) - never LLM-judged. At-most-once:
  `nextRun` advances and persists BEFORE the fire; no backfill after downtime.
  Auto-pause (with one owner alert) on repeated failures, daily token cap,
  chat de-allowlisting, or 5 identical results. Agent-created loops start
  UNAPPROVED and cannot fire until the owner approves - with one deliberate
  exception: a **one-time wakeup** (`wakeup.set`, schedule kind `once`,
  2 min–7 days out) is auto-approved, because it is a bounded single fire
  under the same daily fire/token governor, visible in `/loops` and the web
  Routines tab, and cancellable like any routine. It retires after firing
  (one 5-min retry if the turn itself failed, then an owner alert); at most
  4 armed at once; only an admin's conversation can arm one.
- **Dreaming**: a reserved, undeletable (pause-only) daily loop at 03:30 local
  (the device timezone - Settings → Mode & identity). Stage 1 (no LLM): vector
  decay + expiry prune + windowed dedup - skipped entirely if the clock never
  synced. Stage 2: one unattended reflection turn over yesterday's episodic
  digest - distill up to 7 durable facts (0 is correct on a quiet day), groom
  the scratchpad, stay silent unless something needs the owner. On a provably
  quiet night (empty 24 h digest AND the scratchpad unchanged since the last
  dream, fnv64 baseline in NVS `dreamScrHash`) the PAID stage-2 turn is
  skipped - stage 1 still runs. Gated on 10 min quiet + no jobs + heap; defers
  15 min otherwise. `DREAM` console forces it, including through the skip.
- **Scheduled-turn rails** (loops, dream, synthesis, post-OTA alike): refuse
  reboot, ttsOn, devName, sleepOvr/brightOvr, and loop.create - enforced at
  the apply layer AND the tool-dispatch seam, because sub-agent results and
  loop prompts are untrusted-content injection surfaces.

## Lifecycle 5 - interrupts (what device life does to the harness)

- **Mode switch / reboot**: hard restart, no teardown. Loops can't double-fire
  (persist-before-fire) and daily cost counters can't be wiped (rollover is
  clock-gated) - which is exactly why scheduled turns may not reboot.
- **OTA install**: runs ALONGSIDE the live poller (stopping it once crashed the
  device mid-install); the ring becomes the progress bar; voice is refused;
  every phase writes a timeline row; after the post-update boot validates, a
  staged system turn tells the model what changed (release notes persisted
  across the reboot in NVS).
- **Voice hold-to-talk**: blocks the MAIN loop (WDT suspended around record +
  STT) - `tg_poll` keeps running; the transcript is injected as a normal turn.
- **Low-battery deep sleep**: full stop; config persisted; wake by knob. All
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
refusals (reboot/ttsOn/devName/sleepOvr/brightOvr) and risk-note composition
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

1. Turn contract (`orch_schema.h`) and `/api/*` are additive-only (Rule 5) -
   the schema golden + api-surface gate enforce this.
2. No retry/failover after any tool executed in a turn (side-effect replay).
3. Recall, synthesis, and the tool loop are fail-open - a memory/provider
   failure degrades the turn, never crashes or reboots the device.
4. `takeFreshResults` is the single reset point of the synthesis clock.
5. Steady-state internal-SRAM footprint does not grow (Rule 4): big buffers go
   through `Platform::allocLarge` (PSRAM) or stay on the device side.
