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
| `src/http_control.h` | the 127.0.0.1-only control surface (the chat page, health, state, replies, message, MCP), web-token gated |
| `src/reply_buffer.h` | the bounded, thread-safe ring of recent chat entries (seq + timestamp + role) that backs `GET /api/replies` |
| `src/chat_page.h` | the self-contained web chat page served at `/` (inline CSS/JS, no external resources) |
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

Bound to **loopback only**. Every route requires the web token
(`Authorization: Bearer <token>` or `?token=`) except `/healthz` and the static
chat page (`/`, `/index.html`). The relay sidecar injects the token on every
forwarded request, so the browser reaches all of these through the tunnel.

| Method + path | Purpose |
|---|---|
| `GET /` and `GET /index.html` | the web chat page (ungated; a static shell that carries no data) |
| `GET /healthz` | liveness (ungated; the kubelet probe) |
| `GET /readyz` | readiness (200 iff the engine thread is running) |
| `GET /api/state` | JSON snapshot (turn count, vectors, tokens, provider-configured flag, uptime) - served without entering the engine |
| `GET /api/replies?after=<seq>` | the recent chat entries newer than `<seq>` (the page polls this every ~2s) |
| `POST /api/message` `{"chat_id","text"}` | enqueue a turn (202) |
| `POST /mcp` | JSON-RPC to the tool registry, dispatched on the engine thread |

**The web chat page.** `GET /` serves one self-contained HTML document (inline
CSS and JS, no external scripts, styles, fonts, or images) so it renders over the
tunnel, which reaches no origin but this daemon and forces `nosniff`. The composer
POSTs `/api/message`; the page then polls `/api/replies` for new entries and
renders them. The page itself is ungated because it holds no instance data - every
byte it shows comes from the gated `/api/*` routes. Honest states: a keyless
instance says so plainly (the engine returns a verbatim "no provider" reply and
the page shows a standing notice) rather than sitting silent.

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
