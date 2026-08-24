# Changelog

Release notes for recent Nimbus firmware releases, newest first. Every release
ships as a signed image on the public
[nimbus-fw-releases](https://github.com/ristllin/nimbus-fw-releases)
repository; a device on Orchestrator mode sees it on its daily check and
installs on your say-so ([how updates work](ota.md)).

## Unreleased

- Removed the deprecated e-ink display and rotary-knob code paths. Both shipping
  configurations are color touch panels (the Solide S3 with its LED ring, and the
  Freenove CYD all-in-one). E-ink devices stay on their final firmware (typed OTA
  delivers them no further updates).

## v4.1.0 - Orchestrator self-knowledge, honest capacity, real file delivery

Everything the assistant tells you about itself is now generated from live,
validated device state - never hardcoded prose - and it can finally produce and
deliver real files. Origin: the owner's two-sub-agent task ("collect AI news →
PDF → Telegram; find financial news → email draft") failed end-to-end on
v4.0.0; every feature below traces to a root cause of that failure or to the
owner's follow-up review of the composed prompt.

### 1. Dynamic, validated self-knowledge (the composed prompt)
- The provider/connector catalog is **generated from the live device state**:
  each provider is marked *available, VERIFIED* / *key present, not yet
  verified* / *REJECTED* from the real verify cache - never a checkbox printed
  as fact. Custom MCP servers get an honest "capabilities not catalogued" line.
- A new **[SUB-AGENT CAPABILITIES]** block states, per keyed provider, what a
  spawned agent actually gets - and the universal rule the failed task broke:
  **sub-agents gather (and may run their provider's connectors server-side);
  the head does every owner-delivery step.** It also answers "can a sub-agent
  code?" (anthropic sandbox: writes AND runs code; openai/mistral: Python via
  code_interpreter; nothing pushes to a repository).
- **Every prompt surface reads live state**: the role line carries the device's
  own name (a renamed device no longer introduces itself twice under two
  names); the identity block names *who this turn's message is from* and their
  role (multi-tenant honesty - not everyone is "your owner"); the hardware
  manifest reflects the fitted display, the live SD tier, the real battery
  hardware, and the actual LED count.
- **Per-connector credential state**: the catalog marks a failed OAuth sign-in
  ("sign-in FAILED - tell the owner") and a missing credential ("NO credential
  - not usable"), fed from the one credential choke point. Enabled is a
  checkbox; these markers are evidence.
- **Capability validation is a user setting** (Capabilities → Models → Tool
  use): off / passive (default) / active. Passive marks a provider verified
  when a real turn succeeds; active adds a periodic free re-verify
  (`GET /v1/models`, ~$0) so a revoked key is caught.

### 2. On-board documentation (`docs.list` / `docs.search` / `docs.read`)
The device carries a generated pack of its own documentation (14 curated docs,
169 sections, ~195 KB of flash rodata) and the prompt tells the model to check
it before claiming what it can or cannot do. The pack is regenerated from
`docs/` at build time and ships inside `firmware.bin` - **OTA updates the
device's self-knowledge automatically and it can never version-skew.**

### 3. Complete live self-state (`device.status`)
One tool call now answers every "what is your current state" question: firmware
+ update engine + slot + auto-update, time + timezone + clock sync, heap/PSRAM,
SD (including live *lost* state) + internal flash, battery, ring posture +
battery mode (user and effective), memory counts **vs live caps** (vectors,
episodic, scratchpad), file-store usage + free, **sub-agent capacity**
(running/queued vs limits), real billed token usage, and every settable knob's
current value. `tenant.list` shows usage beside quota; `files.list` leads with
totals; the scratchpad view shows fill vs caps.

### 4. Scratchpad as a first-class response field
The model's persistent working memory (`active` / `short` / `mid` / `long`) is
now a top-level `orch_turn` response field - a free write with no tool round -
that survives across turns and restarts. The prompt renders it every turn with
the write instruction beside the data. It was an admin-only tool nobody
triggered; now multi-step plans persist. (Admin-gated: the global scratchpad
renders into every prompt, so non-admin writes are refused.)

### 5. Honest throughput - `[SPAWN CAPACITY]` (no concurrency, by design)
"6 spawn slots free" implied a hard ceiling; the model under-spawned deep runs.
The prompt now states the true model: N startable this turn, running
**sequentially** (single task, one TLS connection at a time), draining over
successive waves - *not a hard total limit*. A deeper pending queue (12,
decoupled from the concurrency window) accepts a full wave up front. A >60 s
Mistral sub now reports an honest timeout instead of the false "couldn't
start". ⛔ Sub-agent concurrency was investigated and permanently rejected -
hardware-proven unstable (see the development guide).

### 6. Provider file delivery - the PDF path
A sub-agent that generates a file in its provider sandbox now has that file
**captured to the SD artifact store and delivered over Telegram**:
- **OpenAI** code_interpreter (container files) - **including PDF** (verified
  live end-to-end at the API level).
- **Mistral** code_interpreter (files API) - images/text (their sandbox
  currently errors on PDF/CSV output; the capture is type-agnostic and needs
  no change when they fix it).
Downloads stream to SD under the TLS arbiter (the OTA pattern), are
Content-Length-verified (a truncated download is refused, never registered as
saved), and respect the file store's ownership boundary.

### 7. Ring: the orchestrator arc persists while children run
The head's blue arc used to collapse the instant a fan-out turn returned. A
host-tested reconciler on the always-alive main loop keeps it lit while
sub-agents run, bridges the synchronous-dispatch window, clears exactly once
when the last child finishes, and latches off if the system wedges (a dead
task can't pulse "working" forever).

### 8. Clock & timezone, dream honesty, memory dashboard (merged sibling work)
Owner-set timezone with device clock + sync badge on the web UI; routines adopt
a timezone edit budget-neutrally; the nightly dream skips its paid reflection
turn on quiet nights and respects an honest fact quota; the memory dashboard
gained lifecycle columns (created / expires-in / last-used), browse pagination,
Delete All (typed confirm), file content search, SD-wipe danger controls, and
photo-aware `files.send`.

### Main architectural decisions

1. **⛔ No on-device sub-agent concurrency - permanent.** Investigated at the
   owner's prompt; hardware evidence (the tlsSlots=2 A/B failure, recurring
   poll-task stack overflows, a watchdog reset from contending work, ~22.5 KB
   largest contiguous internal SRAM) closed it. Serial single-task/single-TLS
   is the design, not a limitation to fix. Throughput improves only via honest
   wording, queue depth, and remote-side parallelism (openai/anthropic).
2. **Self-knowledge is generated, never asserted.** Anything the prompt claims
   must come from a live seam (verify cache, HAL health, storage tier, tenant
   table, driver constants) - the same "advertised == callable" principle
   extended to "described == measured". Golden tests pin every prompt surface.
3. **Docs ride the firmware image.** Embedded generated pack (rodata) over
   LittleFS images or SD sync: zero RAM cost, works with no card, and OTA can
   never ship firmware whose self-description lags its behavior.
4. **Delivery is the head's job; provider sandboxes are hands.** Sub-agents
   return text plus captured file references; only the head touches the owner
   (Telegram/files/memory). This is the decomposition contract the prompt now
   teaches, and the file-capture path enforces it mechanically.
5. **Honesty gates at the data path.** Truncated downloads refuse to register;
   cross-tenant overwrites refuse at begin AND at the destructive rename; a
   transient verify failure cannot demote a proven credential; every "not
   captured / not usable / sign-in failed" state is surfaced instead of
   silently dropped.

### Verification
Host: 1164 native test cases green (grew from 1117 at v4.0.0). Device:
`esp32s3` + `test` builds green (~60% app slot after the docs pack). The full
diff was reviewed for regressions before tagging. Website docs regenerated and
building clean.

## v4.0.0 - Skills authoring · Deep research · Canonical wire · Deep history

### New
- **Skills authoring**: the assistant can draft its own reusable skills
  (`skill.save`/`skill.delete`). Every assistant-written skill arrives pending
  your approval (web Capabilities → Skills, or `/skill approve <id>` in
  Telegram) and does nothing until approved. Built-in skills can't be
  overwritten; you can delete any skill, the assistant only its own.
- **Deep research** (built-in skill): multi-wave sub-agent fan-out into a
  per-run project on the SD card - each sub-agent's findings auto-save as a
  document, a final large-model writer gets the strongest docs attached, and
  the report is delivered to your Telegram.
- **Sub-agent attachments & projects**: a spawned sub-agent can carry up to 4
  on-device documents (spliced in by the firmware) and a project tag that
  auto-saves its full result - multi-stage runs plan from what actually landed.
- **Deep history**: conversation search now reaches months back - past what is
  loaded at startup - with honest paging ("searched back to a date; older
  history exists"). "I don't remember" now means it actually isn't there.
- **Mid-turn provider failover**: a turn that loses its provider partway
  through continues on the next verified provider, carrying its work
  (Settings toggle "Switch providers mid-turn", default on).

### Changed
- All three providers' tool loops now replay a single device-owned transcript
  (stateless wire) - eliminates the OpenAI chain-poisoning failure class.
- Prompt honesty: delivery claims ("sent", "spoken") require a tool result;
  memory saves are phrased as intent until confirmed; the spawn-slot budget is
  visible to the model so it says when it can't start everything.
- `/api/chat` `pending` now means a turn is actually running.

### Fixed
- Episodic search silently returned nothing from any day-file over 128 KB
  (tail-read offsets were indexed wrong) - a busy month of history was
  invisible while the RAM cache quietly served recent rows.
- Cold-history paging: byte cursors no longer re-emit recent rows or strand
  deep history on the shipped configuration.
- Attach reads now enforce the same per-person file boundary as `files.read`.
- Skill approval holds its lock across the read-modify-write, so a concurrent
  assistant save can't slip an unreviewed body into an approved skill.

## Older releases

Release history before v4.0.0 lives on the
[releases page](https://github.com/ristllin/nimbus-fw-releases/releases).
