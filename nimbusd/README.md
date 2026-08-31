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

## Configuration

Secrets come from the environment (a mounted Secret in k8s); non-secret settings
may also live in `<data>/config.env`. Env always wins. Nothing secret is logged
(masked to 4 chars).

| Key | Meaning |
|---|---|
| `OPENAI_API_KEY` / `ANTHROPIC_API_KEY` / `MISTRAL_API_KEY` | BYOK provider keys (Phase 0 key mode) |
| `TAVILY_API_KEY` | web.search (the router does not proxy it) |
| `TELEGRAM_BOT_TOKEN` | the instance's bot (validate with `nimbusd --getme`) |
| `NIMBUSD_TG_CHAT_ID` | restrict the channel to one chat (auth gate on chat.id) |
| `NIMBUSD_WEB_TOKEN` | required to reach the control surface (the sidecar injects it) |
| `NIMBUSD_DATA_DIR` | durable store root (default `/data`) |
| `NIMBUSD_CONTROL_ADDR` / `NIMBUSD_CONTROL_PORT` | control surface bind (default `127.0.0.1:8787`) |
| `NIMBUSD_DEVICE_NAME`, `NIMBUSD_PRIORITY`, `TZ` | display name, provider failover order, timezone |

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

## Sub-agent fan-out: composed fabric-less in Phase 0 (documented decision)

The device fans sub-agents out through four provider adapters
(`src/agent/adapters/{openai,anthropic,mistral,custom}_adapter.cpp`) built on
`WiFiClientSecure`. Those are **device-only** today. Porting them onto the
daemon's `HttpTransport` (the "Fabric port" work item in plan §3.1) is **not done
in Phase 0**: nimbusd registers no fabric loop (`ProviderHosts::fabric` is
unset), so a turn runs its tool loop on a single head provider and does not
dispatch parallel sub-agents.

This is the plan's sanctioned Phase 0 option ("sub-agent fan-out working (Fabric
port) **or explicitly deferred with the engine composed fabric-less**"). The
head turn, tool loop, memory, web.search, and Telegram persona are all fully
functional without it. The Fabric port is tracked for Phase 2. It is recorded in
ADR 0003.

## The scenario suite (live, paid)

`make scenarios` builds `build/nimbusd-scenarios`, which runs the harness-lab
outcome scenarios **against the daemon composition** (real provider turns), plus
a daemon-only `restart` scenario (write a fact, tear the rig down, rebuild on the
same data dir, recall). Provider keys come from the env or a dotenv file
(`--env`, `NIMBUS_ENV_FILE`, `~/.env`). Scenarios with no key **skip loudly** - a
silent skip reads as a pass. A full run is ~100K input tokens.
