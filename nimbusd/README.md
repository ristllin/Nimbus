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
| `src/daemon_http.h` | `agent::HttpTransport` over one persistent, connection-reusing curl handle; bodies never retained; keeps only the bounded error headers a request asks for (a 429's `x-ratelimit-*` quota window) |
| `src/posix_files.h` | disk-backed `files.*` / `artifact.save` (real bytes + `FileStore` index, `validSegment` gate) |
| `src/daemon_config.h` | `HarnessConfig` inputs from env + an optional config file (env wins); secret masking |
| `src/telegram.h` | Telegram channel over the portable `nimbus::tg::parseUpdates` + offset arithmetic; durable offset |
| `src/tg_access.h` | who may talk over Telegram: the device's fail-closed allowlist + approval queue + the shared RBAC `TenantStore`, durable on the instance volume |
| `src/tg_inbound.h` | routes each getUpdates batch through `tg_access.h`: approved text becomes a turn, anyone else is queued for approval and told so |
| `src/tg_web.h` | the device's `/api/telegram*` + `/api/tenant` routes over `tg_access.h`, so the web app's Telegram panel manages a hosted instance unchanged |
| `src/rig.h` | `NimbusdRig` - the composition, rehydrating all stores on construct and flushing after each turn |
| `src/router_head.h` | the Cumulo head's view of a router refusal (`RouterTap`), the honest copy for each kind and the ladder that answers a refused pick, named from the rule the device's cumulo head shares (`lib/harness/.../router_refusal.h`) |
| `src/hosted_settings.h` | the owner's settings (durable, thread-safe) and the apply-or-refuse rule for every field the web app posts |
| `src/embed_check.h` | the embedding settings' live check: run on the engine thread, never awaited past 5 s, its result kept for the next Verify or Save |
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
/data/mem/embed.txt          the embedding provider/model/width saved from the web app
/data/mem/settings.json      the owner's settings from the web app (routing, directive, time zone, name, tool loop)
/data/mem/submodels.txt      per-provider sub-agent model picks
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
headers). A turn the provider refused is retried once, after `--settle`, only
when the reply names a window that can reopen by then (per-minute, or none
named); a spent daily quota or a plan that allows no such request is recorded,
not retried. It recognizes those replies by the engine's own 429 copy
(`lib/harness/src/rate_limit.cpp`), which `tools/test_vn_connectors_e2e.py`
pins as part of `make test`.

## Configuration

Secrets come from the environment (a mounted Secret in k8s); non-secret settings
may also live in `<data>/config.env`. Env always wins. Nothing secret is logged
(masked to 4 chars).

| Key | Meaning |
|---|---|
| `OPENAI_API_KEY` / `ANTHROPIC_API_KEY` / `MISTRAL_API_KEY` | BYOK provider keys (Phase 0 key mode) |
| `CUMULO_API_KEY` | the Cumulo router key: with no BYOK key it runs the whole assistant (see The Cumulo router head) |
| `Z_AI_TOKEN` | the Z.ai (GLM) BYOK key |
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
| `NIMBUSD_EMBEDDINGS` | `1` (default) long-term memory embeddings on, `0` off |
| `NIMBUSD_EMBED_HOST` / `NIMBUSD_EMBED_MODEL` / `NIMBUSD_EMBED_DIMS` | the default embedding provider (`mistral`, `openai` or `cumulo`), model and width; the web app's embedding settings, once saved, win |

**Embeddings on a one-key instance (CUM-540).** Each embedding call resolves its
route from the current keys: a direct key for the embedding provider wins; without
one, the Cumulo key carries the call through the router on the same upstream and
model (`/router/mistral/v1/embeddings` for `mistral-embed`), so the vectors stay
comparable if a direct key is added later. The `cumulo` provider is the device's
router route (`/router/openai/v1/embeddings`). `/api/mem/stats` reports
`embedAvailable` and the route (`direct`, `router` or `none`) truthfully, and the
web app's embedding settings work as on the device: `POST /api/mem/embedverify`
makes one real call and saves nothing, `POST /api/mem/embedcfg` refuses a model
switch while memories exist unless the owner erases them (`reset=1`).

That call can take the whole 30 s embed timeout, and one HTTP thread serves every
request (`/healthz` included), so it runs on the engine thread and Verify and Save
wait for it at most 5 s (`src/embed_check.h`). Past that, Verify answers `202
{"ok":false,"checking":true,"error":"Still checking this model. ..."}` and Save
answers `503` with the same body: nothing is saved, the call goes on, `GET
/api/mem/embedcfg` and `/api/mem/stats` show it as `check` (`checking`, then `ok`
with the `width` the model returned, or `failed` with the `error`), and the next
Verify or Save of the same candidate answers from that result at once. A check
waiting behind a turn is still "checking", not lost (it counts as lost only 90 s
after it starts), and one a newer check replaced makes no call when its turn
comes, so one candidate is never paid for twice. A width is
saved only as the check read it: Dimensions 0 takes the model's own width, and a
set width the model does not return (Mistral ignores a requested width) is refused
with the real one. A save still queued past the bound answers `503` ("Still
saving"), never a 2xx, since the engine re-checks the memory lock when it runs. On
start, an empty vector store takes the saved width, and vectors of another width
than the saved embedding config (a restart inside a model switch) are copied aside
as `vectors.bin.mismatch-<time>` so memory starts empty, said in the log, instead of
comparing vectors that cannot be compared; if that copy cannot be written, nothing
is erased and the log says so.

## Settings on a hosted instance (CUM-543)

Every field the web app posts to `POST /api/orch` or `POST /api/config` either
applies or is refused; a page never shows "Saved" over a dropped write.

| Applies (persisted on the instance volume, read back by GET, used from the next turn) | |
|---|---|
| `orchHost`, `provPrio`, `subPrio` | primary provider and the fallback orders (unknown names are dropped; an empty order is refused) |
| `sysPrompt` | the directive (up to 1500 characters; blank = the shipped default) |
| `orchLoop` | the head tool loop |
| `devName`, `devTz` (`/api/config`) | the name and the time zone (a POSIX string with zone names of 3 to 16 characters, or a zone name such as `Europe/London`; `/api/state` clock and the prompt's clock follow it at once) |
| `clearConv`, `clearMem`, `convReset` | Clear conversation (a `/clear` boundary: the next turn carries no earlier conversation, the history is kept), erase the running notes, reset provider threads |
| keys, `orchM_<slug>`, `subM_<slug>`, `tgAllow` | provider keys (the device's own key rule and sentences: spaces around a key are dropped and it is stored without them, and a key with a space, a line break or another hidden character inside, or of a length that provider never issues, is refused; the instance's secrets file holds provider keys only, and a key an earlier build stored with spaces around it loads trimmed and is rewritten trimmed), head and sub-agent model picks, the Telegram allowlist |

The device knobs a hosted instance does not offer (tool-loop limits, TLS slots,
mid-turn failover, trace, the custom endpoint) are reported by `GET /api/orch` at
their hosted values, so the page's whole-form **Save Changes** round-trips them as a
no-op (a blank number field is "unchanged"); posting any other value, or any field not listed above (sound, voice, ring,
display, battery, mode, budgets, a typed bot or Tavily key), answers
`400 {"ok":false,"error":"Not available on a hosted Nimbus: <field>"}` and nothing
in that post applies, and a value the instance volume cannot store changes nothing
(500). Env values (`NIMBUSD_ORCH_HOST`, `NIMBUSD_PRIORITY`, `TZ`, ...) are the
defaults until the owner saves a value in the app; clearing the time zone returns to
the operator's `TZ` (else UTC). A spawn's model is the owner's to choose: an approved
member's spawn runs the configured sub-agent model.

**What the assistant is told about the instance (CUM-542).** Every head's prompt
carries the real clock in the owner's time zone, the engine version and the hosted
truth: a cloud instance that is always online, with no screen, microphone, speaker,
LED ring or battery (the composer's desk-device lines are replaced for every head
this daemon registers; `tests/test_hosted_prompt.cpp` pins the wording).

**Files, routines and the rest.** The Files panel (`/api/files/list`, `upload`,
`dl`, `rm`, `rmproject`) works on the instance disk: it is the same store the
assistant's `files.*` tools read, so an upload is readable by the assistant (one
upload up to 1 MB; downloads are served as `application/octet-stream`). An upload
the store refuses (256 files, or the store is full) writes nothing to the disk. Routines,
skills, wake-ups and URL downloads have no hosted engine yet: their lists answer an
honest empty GET and a write answers `400 "Not available on a hosted Nimbus yet:
<feature>"`.

**Commands.** `/clear` (then `/clear yes`) clears the conversation exactly like the
button, `/help` lists the commands, and any other slash command gets a fixed reply:
none of them reaches the model or costs a turn.

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
| `GET /api/lastturn`, `/api/log`, `/api/errlog/recent?bytes=` | the last turn's anatomy (prompt, input, raw output, tool rounds; key-shaped text (the sk-, sk_, cumulo_sk_ and tvly- prefixes, and a long token named a key or a token, as in "my key is ..." or `NAME_KEY=...`) and every key the instance holds masked (a copy cut short by a clipped line too, and another key that starts the same way, such as a second Anthropic key, masked whole), again as it is served, so a key saved after the turn is masked too), the bounded engine log (1000 lines, key-shaped text masked), and its failure lines (device diagnostics, CUM-546). `/api/orch` usage (`sessIn`/`sessOut`/`turns`, `lastIn`/`lastOut`) and `/api/state` `turnCount` keep counting across a key or model save |
| `GET/POST /api/connectors` | the connector registry (device contract); each configured entry carries the derived `auth` the badge reads |
| `GET /api/connectors/catalog` | the model-facing `[PROVIDERS & CONNECTORS]` block exactly as the next turn gets it (text) |
| `POST /api/verify` | verifies a keyed provider in the background the device's way (one unmetered read of its model list; the router's `GET /router/models` for Cumulo): `GET /api/orch` then reports `verify` 1 / 0 / -1 with `vfyReason` (`nocredits`, `router_outdated`), and the harvested list is the model picker's catalog. Also run at start and after a key save (queued right after the key applies, so it always checks the new key, even when an earlier check was still waiting); a turn that answered on a head marks it verified. `provider=mistral` also re-runs the workspace probe |
| `GET /api/models` | the harvested model catalog per provider (device shape; Cumulo ids are routable `<upstream>/<model>` selectors grouped by upstream) |
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

## The Cumulo router head (device parity, CUM-539)

A Cumulo-only instance runs the whole assistant over the router, exactly like a
device holding only a Cumulo key (CUM-242). The head model selector is split by
the shared `resolveRouterRoute` rule (`"<upstream>/<model>"`, a bare id is
`openai`), and the SELECTED upstream's native head runs through
`/router/<upstream>` with the Cumulo key as the bearer: the Anthropic Messages
wire for `anthropic/...`, the OpenAI Responses wire for `openai/...` and bare ids,
Mistral for `mistral/...`, each with its real tool loop (the registry tools ride
every round) when the tool loop is on. `zai/...` and any upstream the native wire
does not cover keep the single-shot chat-completions path with `json_object`;
`zai/...`, like the direct Z.ai head (`Z_AI_TOKEN`), also carries the device's
Z.ai policy: `reasoning_effort: "low"` and a 50 s head budget.
The head advertises provider built-in tools only; the owner's private connectors
never ride the shared router key.

A router refusal never leaves the assistant without an answer (`src/router_head.h`
reads the router's own reason from the refused request; the device's cumulo head
answers by the same shared rule):

| Router answer | What happens |
|---|---|
| The picked model is not offered (`model_not_priced`, 404, or the upstream's own 400 "Invalid model" for an id it does not run) | The same turn is answered on the last router model that worked, else (none, or it is refused too) on the router default, each tried once and never the same model twice (`openai/gpt-5.6` is the default `gpt-5.6`); the reply opens with a line naming the refused model and the fix, and once that answer lands the pick moves to the model in use, so GET /api/orch shows it and the next turn answers |
| That upstream is paused or its route is not allowed (`funding_cap_reached`, `provider_disabled`, `endpoint_not_allowed`) | The turn is answered on the router default with a line saying so; the pick is kept |
| A provider built-in tool is not offered on a paying key (`native_tool_not_metered`) | "That didn't finish - the Cumulo router does not run provider built-in tools on this account. Remove them under Connectors." |
| Credits or a plan are missing (402) | "That didn't finish - the Cumulo router needs credits or a plan for this account. Add credits in the portal." |
| Rate limited (429) | the engine's rate-limit reply |

Only a refusal of an attempt's first request is answered again on another model,
so a tool that already ran is never repeated; a refusal no model can answer gets
its own honest cause, and the selector it names is replaced by "that model" when
the sentence would not fit the engine's 100-character cut, so the fix at its end
is always whole. The context fold runs through the same head: it is answered
on the fallback with no notice in the summary and never moves the pick. Mistral
over the router starts a new conversation every turn (the router admits only `POST
/v1/conversations`), and a conversation marker is handed back only to the upstream
that wrote it, so a pick switch never sends another upstream's id (or a tool loop's
stateless marker) as OpenAI's `previous_response_id`. Host-tested offline in
`tests/test_router_head.cpp`.

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

The instance's own Cumulo key counts as a keyed provider for the engine (CUM-541,
device parity): the head's `[AVAILABLE MODELS]` lists `cumulo`, and the router
keys the instance holds (Cumulo, then Z.ai) are appended after the configured
sub-agent order, so on a one-key instance a spawn that names no provider runs on
the router, while a keyed BYOK provider in the order still wins. The key stays out
of the BYOK-only paths (connector credentials, embeddings by direct key).

A head that calls a configured connector or a field of its reply as if it were a
tool gets the device's way back (CUM-538): the connector is named as one of its
provider's sub-agent jobs with the spawn to use instead, and a spawn field carries
the real remaining spawn room (`tests/test_tool_hints.cpp`).

Every spawn runs the configured sub model for its provider (no live model catalog
to validate a per-spawn pick against), and a spawn on an unkeyed provider is
answered with an honest "couldn't start" message; one the provider refuses with
HTTP 429 also names the quota window, read the same way as a head turn's 429.
Not wired yet on a hosted
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
