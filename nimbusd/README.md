# nimbusd - the hosted Nimbus daemon

A **virtual nimbus**: the same Nimbus orchestrator the desk device runs (turn
engine, memory, tools, provider adapters, Telegram persona), running as a Linux
process in a container instead of on an ESP32. To cumulo it is just a device.
This directory is Phase 0 of the Virtual Nimbus plan
(`../../cumulo/docs-internal/virtual-nimbus-plan.md`): the daemon itself, before
the relay sidecar (Phase 1) and provisioning/billing (Phase 2).

## What is the real code, and what is new

Everything above the transport seam is the **same portable engine** the firmware
compiles - `lib/core` (turn contract, memory engines, tool registry, RBAC) and
`lib/harness` (turn engine, provider adapters, compose/apply). nimbusd is a
composition: it wires that engine to durable POSIX stores and a real network
transport, exactly as `tools/harness-lab` wires it to RAM and libcurl for host
testing. A bug reproduced here is a bug on the board, and vice versa.

New code lives only in this directory:

| File | What it is |
|---|---|
| `src/posix_fs.h` | `PosixEpiFs` (the `EpiFs` byte-file seam over a POSIX tree) + `fsutil` atomic tmp->rename writer |
| `src/posix_platform.h` | `agent::Platform` with **cgroup v2 heap accounting** so the engine sheds load before the kernel OOM-kills the pod |
| `src/daemon_http.h` | `agent::HttpTransport` over one persistent, connection-reusing curl handle; bodies never retained |
| `src/posix_files.h` | disk-backed `files.*` / `artifact.save` (real bytes + `FileStore` index, `validSegment` gate) |
| `src/daemon_config.h` | `HarnessConfig` inputs from env + an optional config file (env wins); secret masking |
| `src/telegram.h` | Telegram channel over the portable `nimbus::tg::parseUpdates` + offset arithmetic; durable offset |
| `src/tg_access.h` | who may talk over Telegram: the device's fail-closed allowlist + approval queue + the shared RBAC `TenantStore`, durable on the instance volume |
| `src/tg_inbound.h` | routes each getUpdates batch through `tg_access.h`: approved text becomes a turn, anyone else is queued for approval and told so |
| `src/tg_web.h` | the device's `/api/telegram*` + `/api/tenant` routes over `tg_access.h`, so the web app's Telegram panel manages a hosted instance unchanged |
| `src/rig.h` | `NimbusdRig` - the composition, rehydrating all stores on construct and flushing after each turn |
| `src/engine_thread.h` | the concurrency layer: one engine thread + request mailbox + a snapshot for lock-free reads |
| `src/http_control.h` | the 127.0.0.1-only control surface (serves the web app + logo, health, replies, message, MCP, backup; delegates `/api/*` to `WebApi`), web-token gated |
| `src/web_api.h` | the `/api/*` surface the web app calls, answered with honest Virtual Nimbus semantics (real chat/memory/providers; hardware panels honestly virtual) |
| `src/web_ui.h` | assembles the served page and seeds the tunnel sign-in token ahead of the app script |
| `src/webui_page.h` | GENERATED (`tools/gen_webui.py`): the device's web app fragments, byte-parity with `tools/webui_page.snapshot`, plus the brand logo |
| `src/reply_buffer.h` | the bounded, thread-safe ring of recent chat entries (seq + timestamp + role) that backs `GET /api/replies` and `/api/chat` |
| `src/main.cpp` | the daemon entrypoint (config, threads, Telegram poll, graceful shutdown) |

## Durable data layout (`/data` by default)

```
/data/mem/vectors.bin        associative (vector) memory - atomic snapshot
/data/mem/episodic/*.jsonl   episodic append-log day-streams (durable per write)
/data/mem/scratchpad.txt     the model's working-memory tiers
/data/mem/memconfig.txt      retrieval/decay knobs
/data/mem/files/             file artifacts + .index
/data/mem/telegram.txt       Telegram allowlist, display names, consumed legacy seed and the
                             RBAC roles + limits, in ONE atomically written file
/data/tg_offset              Telegram long-poll offset (no re-delivery on restart)
```

**Memory survives restart.** The episodic append-log is durable per message; the
whole-file stores are flushed (atomic tmp->rename) after every turn and on a
clean shutdown, and rehydrated on construction. This is proven offline by
`tests/test_posix_stores.cpp` and `tests/test_rig.cpp`, and live (with real
embeddings) by the `restart` scenario.

## Build and test

```bash
make test        # offline unit/T2 suite (no keys, no network) - the CI marker
make daemon      # build ./build/nimbusd
make scenarios   # build ./build/nimbusd-scenarios (PAID to run - see below)
make check       # compile-only anti-rot gate
```

Host builds need ArduinoJson (populate `.pio/libdeps/native` once with
`pio test -e native` at the repo root) and a sibling `../solide-drivers` checkout
(the portable headers). The Docker build fetches both itself.

### Docker

```bash
docker build -f nimbusd/Dockerfile -t nimbusd:dev .   # context = repo root
docker run --rm -e TELEGRAM_BOT_TOKEN=... -e MISTRAL_API_KEY=... \
  -e NIMBUSD_WEB_TOKEN=... -v nimbus-data:/data nimbusd:dev
```

The build stage runs `make test`, so a broken build never produces an image.

### Connectors e2e (live, paid)

`tools/vn_connectors_e2e.py` drives a running instance over its HTTP surface the
way the web app does: it configures Studio connectors (gcal, notion, slack on
Mistral), runs the workspace probe via `POST /api/verify` and checks that the
connector badge, the Capabilities row and the model catalog agree; then it asks
"What is on my calendar today?" and asserts the reply names an event that one
independent Mistral call with the calendar connector sees. Secrets come only from
the environment (`NIMBUSD_WEB_TOKEN`, `MISTRAL_ORACLE_KEY`) and every evidence
file is redacted. `--skip-bc` runs the free configuration + probe step only. It
waits between paid calls (`--settle`) because a Mistral key's Studio connectors
have their own per-minute and per-day request quota (`x-ratelimit-*-custom-*`
headers).

## Configuration

Secrets come from the environment (a mounted Secret in k8s); non-secret settings
may also live in `<data>/config.env`. Env always wins. Nothing secret is logged
(masked to 4 chars).

| Key | Meaning |
|---|---|
| `OPENAI_API_KEY` / `ANTHROPIC_API_KEY` / `MISTRAL_API_KEY` | BYOK provider keys (Phase 0 key mode) |
| `TAVILY_API_KEY` | web.search (the router does not proxy it) |
| `TELEGRAM_BOT_TOKEN` | the instance's bot (validate with `nimbusd --getme`) |
| `NIMBUSD_TG_CHAT_ID` | **legacy**, optional: a one-time seed of an EMPTY Telegram allowlist (see Telegram access below). It is no longer a gate |
| `NIMBUSD_WEB_TOKEN` | required to reach the control surface (the sidecar injects it) |
| `NIMBUSD_DATA_DIR` | durable store root (default `/data`) |
| `NIMBUSD_CONTROL_ADDR` / `NIMBUSD_CONTROL_PORT` | control surface bind (default `127.0.0.1:8787`) |
| `NIMBUSD_DEVICE_NAME`, `NIMBUSD_PRIORITY`, `TZ` | display name, provider failover order, timezone |
| `NIMBUSD_SUB_PRIORITY` | sub-agent provider order (device `subPrio`); unset = same as `NIMBUSD_PRIORITY` |
| `NIMBUSD_ORCH_HOST` | pin the head provider (device `orchHost`); unset = the first keyed provider in `NIMBUSD_PRIORITY` |
| `NIMBUSD_TOOL_LOOP` | `1` (default) head tool loop, `0` single-shot head turns (device `orchLoop`) |

## Telegram access (the device model)

A hosted instance trusts **no** Telegram chat by default, exactly like the desk
device. With a bot token set and nobody approved, every chat is refused: it gets
no turn, one polite message saying the owner has to approve it (at most once per
10 minutes per chat), and it waits in the approval queue (the newest 5, RAM only).
The owner approves people in the instance's own web app (**Assistant, Connectors,
Telegram**): one tap on a waiting sender, or **Add** by chat ID. The allowlist and
the roles are stored on the instance volume, so they survive restarts and ride
`GET /backup`.

- **Roles are the existing RBAC.** An approved chat is a **user**; admin is only an
  explicit grant (click a chip's role to cycle admin, user, guest, or `POST
  /api/tenant`). This differs from the device, which adopts the first approved chat
  as admin: on a hosted instance the owner lives in the web app, and that rule would
  hand owner rights to whoever was approved first. A user or guest runs as
  themselves: their own memory namespace, their own recall, a prompt that says who
  is speaking, and no access to the owner's files. An approved chat of any role can
  use the owner's connectors and start sub-agents: approving someone is granting
  that.
- **One namespace's sub-agent work at a time.** The shared job engine keeps one pool
  of sub-agent results and folds it into whoever speaks next, so a hosted instance
  keeps queued, running and unreported work inside one data namespace: a spawn
  from another namespace is refused with the reason, and a member's turn from
  another namespace is held on the engine thread until that work is reported (the
  owner's own turns never wait).
- **Revoking** (role `unknown` through `POST /api/tenant`) or removing a chat takes
  effect on its next message; a revoked chat is told its access was removed, and a
  removed chat's role is erased (re-approving starts it as a user). Work it left
  running is not reported to it.
- **No open access.** The device's "Open access" switch is refused on a hosted
  instance: a public bot would spend the owner's keys and reach the owner's
  connectors.
- **`NIMBUSD_TG_CHAT_ID` is legacy.** If set, it seeds an EMPTY allowlist once (that
  chat becomes the admin, since the operator named it as the owner's) and is then
  consumed: a list the owner already manages is left alone, and a seeded chat the
  owner removes does not come back on restart.
- The web chat and the token-gated control API are the owner's own surfaces and
  always run as the admin; a Telegram chat id posted to `POST /api/message` is held
  to the same allowlist as the Telegram poll.

The routing and every rule above are host-tested offline in
`tests/test_tg_access.cpp` and `tests/test_sub_fabric.cpp`.

## Control surface (the seam the Phase-1 sidecar forwards to)

Bound to **loopback only**. The data routes require the web token
(`X-Nimbus-Token` header, `Authorization: Bearer <token>`, or `?token=`); only the
app shell (`/`, `/index.html`), the brand logo (`/logo.svg`), `/healthz` and the
pre-auth sign-in exchange are ungated. The relay sidecar injects the token on
every forwarded request, so the browser reaches all of these through the tunnel.

| Method + path | Purpose |
|---|---|
| `GET /` and `GET /index.html` | the assembled Nimbus web app (ungated shell; seeds the tunnel sign-in token) |
| `GET /logo.svg` | the brand mark (ungated; a logo is not sensitive) |
| `GET /healthz` / `GET /readyz` | liveness (ungated) / readiness (200 iff the engine thread is running) |
| `GET /api/state`, `/api/health`, `/api/orch` | Home / Assistant snapshots, served from the engine snapshot + immutable config (never enter the engine) |
| `POST /api/chat` + `GET /api/chat` | send a turn, then poll for the reply (the web app's chat surface) |
| `GET /api/replies?after=<seq>` / `POST /api/message` | the CUM-263 reply ring + enqueue-a-turn contract (kept stable) |
| `GET/PUT /api/mem/*`, `GET /api/tools` | memory dashboard, dispatched onto the engine thread (single-context safe) |
| `GET /api/themes`, `/api/qr`, `/api/docs/search` | pure/static surfaces (no engine, no hardware) |
| hardware panels (`/api/audio/*`, `/api/wifi`, `/api/ota/*`, ...) | honest "not on a hosted instance" - never a faked value or a dead control |
| `POST /mcp`, `GET /backup` | JSON-RPC to the tool registry / a consistent tar of the mem tree |
| `GET/POST /api/connectors` | the connector registry (device contract); each configured entry carries the derived `auth` the badge reads |
| `GET /api/connectors/catalog` | the model-facing `[PROVIDERS & CONNECTORS]` block exactly as the next turn gets it (text) |
| `POST /api/verify` | `provider=mistral` re-runs the Mistral workspace probe in the background; other providers are acknowledged |
| `GET /api/telegram`, `POST /api/telegram/{add,approve,deny,remove,rename,role,public}` | the Telegram allowlist + approval queue (device routes and shapes); `public` (open access) is refused on a hosted instance |
| `GET/POST /api/tenant` | RBAC roles + limits per approved chat (device routes, device guards) |

**Mistral Studio connectors (device parity).** A Studio connector (gcal, notion,
slack, ...) has no stored credential: it is usable once the Mistral key's workspace
lists it as active in `GET /v1/connectors` (the shared rule in
`lib/core/.../connectors_wire.h`; `is_authenticated` is only a hint, since it reads
false for connectors that work). The device runs that probe after each Mistral
verify; the hosted page has no verify button, so the daemon runs it at startup,
after a Mistral key is saved, after a connectors write while the workspace is still
unchecked, and on `POST /api/verify provider=mistral`. It is skipped (no request)
without a Mistral key or an enabled Studio connector, and until an answer lands a
Studio connector reads not usable (fail-closed). The catalog, the `/api/connectors`
badges and the `/api/tools` connector rows all read one auth-stamped view, and the
catalog names the head the turn really runs on (the first keyed provider, or the
`NIMBUSD_ORCH_HOST` pin). Studio connectors never ride a Mistral HEAD turn here
(`headProviderDeps`): the head's single-shot turn always carries the strict
structured-output schema, and Mistral does not answer a call that combines it with
a Studio connector (closed at 60 s, measured), so they run on a spawned Mistral
sub-agent (free text), and the catalog and `/api/tools` report them sub-agent-only.

**The web app (CUM-265).** `GET /` serves the device's own single-page app - the
exact fragment bytes the device serves (`tools/gen_webui.py` assembles them from
`include/web/ui_*.h`, byte-parity with `tools/webui_page.snapshot`) - so a Virtual
Nimbus is recognizably the same product, not a bare chat page. The shell is
ungated because it holds no instance data; every byte it shows comes from the
gated `/api/*` routes. **Tunnel sign-in:** the app gates its views on a per-device
token in the browser, which inside the tunnel there is no device screen to scan
for, so the served page seeds this instance's web token ahead of the app script -
the tunnel equivalent of the device's first-run auto sign-in. **Honest virtual
semantics:** the assistant panels (chat, memory, providers, usage, tools) are the
real engine; the hardware panels (ring, display, touch, battery, audio, Wi-Fi, ESP
OTA) report their honest virtual truth and never fabricate a reading or leave a
dead control, and software update is the platform rolling the instance image, not
an ESP OTA. A keyless instance still says so plainly rather than sitting silent.

## Sub-agent fan-out (device parity)

A spawned sub-agent is how the device runs a provider's connectors off its own
turn (a Mistral sub carries the Studio connectors server-side) and how it farms out
research. The hosted instance now runs the same machinery: the portable
`JobEngine` (queue, one dispatch per pump, round-robin poll, the synthesis turn)
over a fabric of five adapters (`src/sub_fabric.h`: anthropic, openai, mistral,
zai, cumulo) that call the same `providers::*Dispatch/Poll/Cancel` wire functions
the device adapters wrap, each resolving the instance's current key, model and
connectors at call time. The engine thread pumps the jobs after every task and on
every idle tick, so dispatches and the synthesis turn stay serialized with turns
(the device's one-work-slot rule). The job journal lives on the instance volume
(`<data>/mem/journal/j<slot>.json`), so an unfinished job re-attaches after a
restart instead of being dispatched again.

Every spawn runs the configured sub model for its provider (no live model catalog
to validate a per-spawn pick against), and a spawn on an unkeyed provider is
answered with an honest "couldn't start" message. Not wired yet on a hosted
instance: skill capsules, document attachments, auto-saving a sub's result into a
project, and provider file capture (the engine passes the task through and notes
attachments instead of splicing them).

This replaces the Phase 0 "fabric-less" composition, under which the JobEngine
had no fabric and every spawn was dropped without a message, so a Mistral Studio
connector could never run under the default tool loop.

## The scenario suite (live, paid)

`make scenarios` builds `build/nimbusd-scenarios`, which runs the harness-lab
outcome scenarios **against the daemon composition** (real provider turns), plus
a daemon-only `restart` scenario (write a fact, tear the rig down, rebuild on the
same data dir, recall). Provider keys come from the env or a dotenv file
(`--env`, `NIMBUS_ENV_FILE`, `~/.env`). Scenarios with no key **skip loudly** - a
silent skip reads as a pass. A full run is ~100K input tokens.
