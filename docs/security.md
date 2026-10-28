# Security Posture

How the device protects its network surfaces (web UI, MCP, Bluetooth, setup network,
outbound TLS), what is verified on hardware, and which hardening items remain open.
Read it before exposing a device on a shared network or auditing a release.

## Threat model

A personal desk device on the owner's home LAN. Primary adversaries: another host on
the same LAN, anyone in RF range of an active Orchestrator setup AP
(`Nimbus-setup`) or the Notifier Bluetooth radio, and an attacker on the internet path to
the providers (evil-twin AP, rogue router, DNS/ARP spoof). Not an internet-exposed
server - findings are weighted by real reachability.

## In place (implemented + on-device verified)

- **Per-device web/MCP auth token.** `store::webAuthToken()` (96-bit hardware-random,
  NVS, no setter). `net::webAuthOk()` checks `X-Nimbus-Token` (header or form field,
  never a `?t=` query param) in constant time and fails closed. Gates **every** route, reads included - `GET /api/state`,
  `/api/log`, `/api/connect`, `/api/sdprobe` and the LAN `POST /mcp` among them. The
  only ungated responses are the ones the gate itself needs: `GET /` (the static
  shell, which carries no device data - every value is a placeholder filled in later by
  a gated fetch), `GET /app.js` (that shell's script, served as its own response so
  each fits the cloud link's buffer; static, from flash, no device data) and
  `GET /logo.svg` (the gate's own artwork). Two deliberate bootstrap
  exceptions exist, both **unprovisioned-only**: `/savewifi` (first provision), and the
  AP-interface token handout on `GET /` + the captive-portal catch-all, which `302`s to
  `/?t=<token>` for a peer on the setup AP while no `staSsid` is saved (detailed under
  *Token handout is unprovisioned-only* in the Setup AP password section below). Once Wi-Fi is configured, neither fires.
  Delivered automatically via the **Sign-in QR**: `configUrl()` embeds a single-use
  `?c=` code (never the durable token), which the browser exchanges once over `POST
  /api/signin/exchange` for the token. The token
  is not a separate onboarding step. Requiring it also defeats CSRF. Verified:
  no/wrong token → 401, valid → 200.
  ⚠ **No auth decision is made from the request's `Host`, `Origin`, `Referer`, or client
  IP.** Reaching the device by raw IP is therefore not a way around the gate. It can
  *look* like one: the browser stores the token per ORIGIN, so a browser that identified
  at `http://<ip>` is silently authenticated there while `http://<name>.local` - a
  different origin - still shows the gate. Same device, same gate, two storage buckets.
- **Body-consuming routes gate before they buffer.** The two routes that accept a raw
  request body, `POST /api/mem/import` and the LAN `POST /mcp`,
  decide auth on the first body chunk before any payload buffer exists: an
  unauthenticated caller costs a tiny header and every byte is dropped (no internal-heap
  growth from the LAN). An authenticated body accumulates in PSRAM behind a hard cap
  (512 KB import, 64 KB MCP) and is refused with `413` past it, never truncated.
  Both caps are verified on hardware.
- **Secret redaction in logs.** Every line written to the agent log ring (served by
  the token-gated `GET /api/log`) passes through `core::LogRing::redact` at the one
  `logring::put` choke point. Two layers: the provider keys and the Telegram bot
  token are registered as exact secrets at boot (masked wherever they appear), and a
  heuristic backstop strips `Bearer <token>`, `api_key=`/`"key":"..."` values, and
  `user:pass@host` URL credentials, so a provider error body echoed into the log
  cannot carry a key onto the ring. Host-tested (`test_logring`). The endpoint is
  also token-gated, so redaction is defense in depth rather than the only barrier.
- **Guest moderation gates (owner opt-in, default off).** Three independent checks
  that screen non-admin traffic only; the owner (Telegram owner, web, serial, voice)
  is never classified. Each runs one classifier call per screened item (the Cumulo
  moderation endpoint on a Cumulo key, else Mistral moderation on the user's key),
  so each is a switch with a stated cost. The fail behavior is fixed per gate and
  host-tested (`test_orch_moderation`):
  - **Inbound guest text** (pre-turn): fail-CLOSED. A message that cannot be
    classified is not answered, and a flagged message never starts a paid turn.
  - **Outbound replies to guests**: fail-OPEN. A flagged reply is held back, but a
    classifier outage never silences the assistant (it delivers unscreened).
  - **Injection screen on fetched world content**: fail-OPEN with marking. A
    heuristic (plus the classifier) flags content that looks like a hidden
    instruction and MARKS it untrusted so the model treats it as data; it never
    blocks. The decision policy is the pure `nimbus::orch` moderation core; the
    device classifier is `src/agent/adapters/moderation`.
- **Crash reports never carry stack bytes or registers.** A panic writes a core dump
  (every task's stack) to the coredump flash partition. The boot that reads it logs
  only the crashing task's name, program counters, the fault cause and address, the
  panic text and the image's ELF hash; a stack or register can hold a provider key
  or the bot token in transit, so neither is ever printed (host-tested with a planted
  pattern, `test_crash_forensics`). There is no route that downloads the raw dump:
  it leaves the device only over USB on a bench, and a factory reset erases it. The
  release ELF files needed to decode a dump are kept as CI artifacts of the source
  repository, never next to the published images; they contain no secret.
- **Safety activity log and scoped allowlist (privacy-first).** Every verdict the
  scanner reaches (a blocked guest message, a suspected injection in fetched
  content) is recorded to a bounded, newest-first local log the owner reviews under
  Assistant, Safety, Activity. The log is a ring of the 64 most recent entries;
  each excerpt is redacted through the same secret set the durable log uses and
  clamped to 512 bytes, so it cannot carry a key. It is persisted on the card or
  flash tier the durable error log uses, never on NVS, and its bound and ordering
  are host-tested (`test_safety_activity`). From that list the owner can:
  - **Approve (unblock), always scoped.** Approving adds one allow-rule scoped to a
    specific sender, content class, or content pattern. There is no global off
    switch: the allow scope has no "everything" value, and an empty target is
    refused, so an approval can never become a blanket disable (host-tested as the
    class). "This type" (content class) is offered only for a genuine narrow category:
    an injection entry records the specific pattern it tripped, so trusting the type
    trusts that one pattern, not the whole scan. A moderation-classifier block carries
    no sub-category, so it can only be approved by sender or by exact content, never
    by type (which would silence the gate). The scanner then honors the rule as a real
    unblock. The allowlist is inspectable and revocable from the same tab (Allowed items).
  - **Dismiss.** Clear an entry without changing what the scanner does next time.
  - **Report to Cumulo, subscription-gated, never automatic.** Nothing is ever
    auto-reported. A report is possible only when the device holds a Cumulo key and
    the account is entitled; without a key the action states "Reporting needs a
    Cumulo subscription." and spends no network. The send never runs on the web
    request: the tab queues it and a short-lived worker task performs the one TLS
    POST (the cloud_mint pattern), so a report can never stall the web surface or the
    loop watchdog; the tab polls for the outcome. The payload carries exactly these
    fields and nothing else (host-tested to that exact set): the device id, the report
    time, the verdict, the rule, the channel, the redacted excerpt (at most 512 bytes),
    and a meta object with the firmware version and the source gate. The decision policy, the allowlist, and
    the report payload builder are the pure `nimbus::orch` safety-activity core; the
    device seam is `src/agent/safety_activity_store` and the web surface is
    `src/net/safety_routes` (`/api/safety*`, token-gated).
- **Danger-zone actions are typed-confirm gated, each distinct.** Erase Storage
  (`ERASE STORAGE`), Factory Reset (`FACTORY RESET`), and full-card Format
  (`FORMAT CARD`) each require their OWN exact phrase, so one confirmation can never
  trigger a heavier action than the owner meant (`nimbus::orch::confirmOk`, one
  source of truth, host-tested as a matrix so a new destructive action cannot ship
  ungated). All three are token-gated POSTs and are deferred to the main loop (never
  erased on the web task). **Factory Reset preserves only the board's physical
  identity** (display model, mount orientation, touch calibration, update slug) so a
  reset device restarts on the right driver instead of a screen it cannot recover; it
  scraps everything the owner set, including the device name, and the unit re-onboards
  fresh with a new name. With "erase storage" it also runs the full private-data
  sweep in the same flow - memories, chat state, routines, logs, leftover audio,
  and any crash dump, across internal flash and the SD card. The authoritative
  path list lives in `agent::memory::eraseAllPrivateData()`
  (src/agent/memory_subsystem.cpp); prose lists drift, so this doc deliberately
  does not repeat it. Deleting a file only unlinks it, so the full reset also
  erases every block of the internal file area (the flash partition that holds
  routines, chat state, and logs) and lays down an empty file system
  (`agent::memory::scrubInternalFs()`, order pinned by `nimbus/orch/reset_plan.h`),
  so even a raw read of that partition finds nothing of the previous owner (NVS
  and the crash-dump area are erased as well). That erase runs before the
  settings are erased: if it cannot finish, the reset stops and says so, and the
  device keeps its settings, so it never reads factory-fresh over data that
  survived. **The SD card is different.** The reset deletes Nimbus's
  files on the card but does not overwrite the card, so recovery software can
  still read deleted data from it, and **Format Card** rebuilds the card's file
  system without overwriting the old data either. To hand a device to someone
  else, remove the card and keep it, or erase the card on a computer with a
  format that overwrites the whole card (for example the overwrite option of the
  SD Association's SD Memory Card Formatter). The erase
  REFUSES - keeping config, telling the owner to reseat the card and try again -
  whenever private data could survive somewhere it cannot reach: a card with
  memories that is not mounted (pulled since boot, or absent at boot according to
  the boot-time card probe), or any delete of existing data failing. The web page
  pre-checks the same predicate so it never claims "erased" over a refusal. The
  device menu offers the same action as Settings > Reset > Factory reset (always
  the full sweep); on glass the gate is a dedicated confirm screen that defaults
  to Cancel, and its "Erase everything" row is tap-armed - the first tap only
  selects it, a second deliberate tap fires - so a doubled or bounced tap can
  never erase the device (the touch counterpart of the web's typed phrase).
  Full-card Format rewrites the whole card and is offered only when a card is
  physically present, so the control is never a knob that can only fail; with no card
  the action refuses honestly.
- **Telegram allowlist fails CLOSED.** An empty allowlist rejects all chats (was
  fail-open = allow-all); the poll task warns loudly if a token is set with no allowlist.
- **Shared-engine mutex.** `memory::Lock` (recursive) serializes VectorMemory /
  Scratchpad / episodic access across the AsyncTCP web/MCP task + the Telegram turn
  task (fixes an iterator-invalidation / heap-corruption race). Never held across a
  blocking TLS embed.
- **Notifier Bluetooth link is bonded and encrypted.** In Notifier mode the ring/screen are
  driven only over Bluetooth. The GATT server requires a **bonded, encrypted** link (LE
  Secure Connections, Just Works) to write frames (`WRITE_ENC` on FRAME) or read/write
  CONFIG - so an unbonded central in RF range can connect and read the ungated STATUS
  telemetry (protocol/fw version + a seq counter) but **cannot paint the ring or
  read/write config**. This closed the v1 open-radio hole (anyone in range could paint
  the ring). Just Works (not MITM/passkey - macOS won't surface a passkey dialog for a
  background-CLI-paired custom peripheral) drops only active-MITM
  protection during the one-time bond; the link stays ECDH-encrypted (passive-sniff
  safe). Bonds persist in NVS. Verified on hardware: unbonded write rejected; bonded
  frame applies; bond survives a restart.

## Who may run each action

Every action the assistant can take is checked against who asked, not only against
what was asked. Before v5.0.0 a device action was checked for a valid model, a keyed
provider or a protected key, and never for the caller: a Telegram member, a
public-mode guest or a LAN `/mcp` client could move the owner's main model, stop the
owner's sub-agents or play audio in the owner's room, and a routine or a sub-agent
results turn could move the main model or stop sub-agents.

**The caller.** Each check reads three facts:

- **Role**: the tenant role of the chat the turn runs for (the owner's chats, the
  web chat, on-device voice and the serial console are the owner; a member is a
  user; a guest is a guest; a public-mode stranger is a user or a guest; a revoked or
  never-approved chat is unknown), or the LAN endpoint's own identity (one device
  sign-in code, no per-caller identity, user rights in its own namespace). A chat
  with no role yet takes the Telegram owner rule only while the tenant table has no
  admin at all, so a newly onboarded owner is the owner from the first message and
  position on the allowlist never makes anyone else one. The owner-only commands
  (`/model`, `/budget`, `/update`, `/mcp`, ...) read the same role.
- **Unattended**: nobody is writing this turn right now. Routines, wakeups, the
  nightly dream, the turn that summarizes sub-agent results, and the device's own
  post-update turn are unattended. A routine, a wakeup and the post-update turn run
  on the owner's chat; a results summary runs on the chat that started the
  sub-agents, under that chat's role, so the role alone cannot tell the owner's
  automatic turns apart from the owner.
- **LAN**: the call came over `POST /mcp`.

**The rules.** One table, one row per action (`lib/core/src/orch_action_rules.cpp`):

| Rule | Owner turn | Member | Guest | Routine | Results summary | System turn | LAN `/mcp` | Serial |
|---|---|---|---|---|---|---|---|---|
| open | yes | yes | yes | yes | yes | yes | yes | yes |
| turn (an approved chat) | yes | yes | yes | yes | yes | yes | no | yes |
| owner | yes | no | no | yes | yes | yes | no | yes |
| owner, attended | yes | no | no | no | no | no | no | yes |

The three unattended classes (a routine or wakeup, a results summary of the owner's
sub-agents, the system turn) share one column: the table treats every owner turn
nobody is writing alike, as the device's scheduled-turn rails always have. A results
summary of a member's sub-agents runs as that member. A revoked or never-approved chat
(role unknown) is refused everything but the open actions.

"Open" actions read or write the caller's own data, so the handler draws the line
(its own namespace and quota, see [People and privacy](./people-and-privacy.md)).
"Owner" keeps the owner's own routines working (a routine may dim the ring at night
or play an alarm). "Owner, attended" is for anything that moves money, identity,
access or physical risk: only the owner, in a message they write.

| Rule | Actions |
|---|---|
| owner, attended | device actions: `orch_model` (the main model), `reboot`; config knobs: `priority`, `ttsVoice`, `ttsProv`, `ttsOn`, `sttProv`, `sleepOvr`, `brightOvr`, `devName`; tools: `memory.config` with `action: update`, `prefs.edit` add / replace / remove, `loop.create`, `loop.cancel`, `skill.save`, `skill.delete`, `tenant.list`, `tenant.set_role`, `tenant.set_quota`, `session.terminate` (and the turn's terminate op), `session.tell`, `device.selftest` with `audible: true`, `reply.telegram` or `files.send` to a chat other than the one the turn runs for (a `files.send` naming no chat from the web chat, voice or serial goes to the owner's chat, so it counts as another chat), `files.share` by the owner (it can share any file with everyone; stopping a share stays open), every connected tool server's tool (`mcp.<server>.<tool>`) |
| owner | device actions: `led`, `lights`, `tts`, a `config` element as a whole; config knobs: `ledBrightness`, `posture`, `profile`, `theme`, `attnHoldMs`, `sfxLvlN`, `sfxLvlO`, `sfxVol`, `sfxTheme`; tools: `device.control`, `device.status` (it carries the spend and budgets), `device.log`, `media.play`, `media.pause`, `media.stop`, `reply.speak`, `models.list` with `refresh: true`, `memory.scratchpad` (and the turn's `scratchpad` field), `prefs.edit` list, `loop.list`, `wakeup.set`; the needs-you light a turn's question raises |
| turn | `web.search`, `image.generate`, `image.look`, `reply.telegram` and `files.send` to the caller's own chat, `session.spawn`, the turn's spawn ops |
| open | `memory.write`, `memory.search`, `memory.update`, `memory.pin`, `memory.episodic`, `memory.archive`, `memory.config` view, `results.get`, `results.list`, `session.poll`, `session.list`, `models.list`, `artifact.save`, `files.append`, `files.fetch`, `files.list`, `files.pdf`, `files.read`, `files.search`, `files.share` (a member's own files, and stopping a share), `files.stat`, `food.portions` (the food coach's plan arithmetic; stateless, it writes nothing), `docs.list`, `docs.read`, `docs.search`, `skill.get`, `skill.list`, `system.health`, `device.selftest`, `ota.status`, `media.list` |

**Where it is enforced.** Two seams, every route:

- `applyDeviceAction` (`lib/harness/src/apply.cpp`) is the one envelope for the
  turn's end-of-turn `device[]` actions and the mid-turn `device.control` tool. It
  checks the action as a whole first (before any key or model list is consulted, so
  a refused caller learns nothing about them), then strips each config knob the
  caller may not set. The `device.control` handler uses the tool call's own caller,
  never the chat a concurrent turn happens to be running.
- `ToolRegistry::dispatch` (`lib/core/src/orch_tool_registry.cpp`) checks every
  tool call through the access resolver `installActionRules` installs, on the turn's
  tool loop, the LAN endpoint and the test seams, in both modes. A member's or a
  guest's turn is not offered a tool it would be refused.

**Sub-agents belong to the chat that started them.** A sub-agent inherits the role
of the turn that started it (`nimbus/orch/connector_grant.h`, `JobEngine`). One the
owner started carries the owner's connectors (remote tool servers, Gmail, Calendar,
Drive, Studio connectors); one a member or a guest started carries the provider's
built-in tools only (web search, the code sandbox), exactly like a request over the
shared router, and a connector of the owner's named as its task is refused with the
reason. Every request a member's or a guest's own turn makes is held to the same
rule. Its finished result is read only by that chat's turns: the owner's turns read
every result of the owner's sub-agents (one person across channels), a member's or a
guest's turn only its own chat's, so member-written text never reaches an owner turn
and the owner's results never reach a member. The summary turn runs per chat under
that chat's role, and the raw fallback goes to each chat with its own results. The
running sub-agents a turn is shown (`[ACTIVE SESSIONS]`, `session.list`,
`session.poll` by id) follow the same line: the owner sees every one, a member or a
guest only its own, the LAN endpoint none. The results store was already scoped this
way (`results.get`, `results.list`).

**A refusal is never silent.** The head reads it: a refused tool call or
`device.control` call fails with the reason, and a refused end-of-turn action adds a
`[DEVICE] not applied: ...` line to the next turn. The person reads it too: a member
or a guest gets one line on the reply ("Only the device's owner can change the main
model."), and an automatic turn of the owner's says on its reply what it left as it
was ("The main model stays as it is: ...", "An automatic turn cannot change the
speaking voice, so it was left as it was."). A refused action backs no claim, so a
reply that says it switched gets the correction line. A member's or a guest's question reaches them in
the reply and does not light the owner's needs-you state.

**A new action without a rule fails the build's tests.** `test/test_action_rules`
holds the expected class table (every action x the eight caller classes above) and
fails on any tool the firmware registers, any device action or config knob the turn
schema offers, and any knob the validator can set that has no row; it also drives
the dispatch seam for every registered tool and every class. At runtime a name with
no row is refused to everyone but the owner writing now.
`test/test_harness_apply` drives every device action and every config knob through
`applyDeviceAction` for each class.

**The device sign-in code is the owner's.** The LAN `/mcp` endpoint gets user
rights so a tool client on the network cannot act as the owner through it, but the
same code also opens the web chat, which runs as the owner. Treat the code as the
owner's password.

**What is not covered yet** (tracked for the next release):

- "Owner, attended" proves the owner is writing, not that an instruction came from
  the owner: a turn of the owner's that read outside content (a web page, a file, a
  sub-agent result) can still be steered into an owner-attended action. Skills and
  standing rules already wait for approval in that case; the other owner-attended
  actions do not yet.
- Members and guests spend the owner's provider budget on their own turns (chat,
  `web.search`, images, sub-agents), within the spend ceilings.
- The unattended column does not tell an owner's routine from a summary of the
  owner's own sub-agents fed by outside text, so such a summary keeps the owner's
  everyday actions (the ring, the speaker, music, a wakeup, the scratchpad).
- A chat approved from the pending list gets no tenant row until someone sets its
  role, so it resolves by the allowlist (a user once the table has an admin).
- A Mistral head conversation pins the connectors it started with: a chat whose role
  changes keeps its open conversation's connectors until that conversation is reset.
- A routine that runs on the web chat or voice and names the owner's Telegram chat
  as a target counts that as another chat, which an automatic turn cannot message.
- `tools/list` over `/mcp` still lists the whole registry; a call outside the LAN
  column is refused.

## Hardening record and open items

Each item states what changed, where it lives in the code, and what is still open.

### Telegram archive on the SD card (v4.5.14, bench verification pending)

The device keeps every Telegram message and file it receives or sends under `/mem/tg`
on the SD card ([memory.md, section 4](memory.md#4-the-telegram-archive-sd-card)).
That is the owner's full conversation history, photos and voice notes included, so it
gets the same care as the rest of `/mem`:

- **No secrets on the card.** The raw update JSON never carries the bot token (it is
  only in the request line). Every record the archive writes (inbound updates,
  outbound text and captions, index lines) is still scrubbed of the exact values the
  log ring masks (the provider keys and the bot token registered at boot) plus the
  bot token in use, so a key pasted into a chat lands on the card as `[redacted]`.
  Media files are stored as received. File downloads use the token in the request
  only.
- **Owner token on every route.** `GET /api/tg/archive`, `GET /api/tg/archive/item`,
  `GET /api/tg/archive/zip` and `POST /api/tg/archive/import` need the device
  sign-in token. Chat, day and file
  names are validated against a strict pattern before any path is built, so a request
  cannot leave `/mem/tg`. Files are served as downloads unless they are bitmaps or
  plain text, with `nosniff` and a sandboxing content security policy, the same rule
  as the web page's Files list (Memory, then Files).
- **Strangers cannot spend the card.** A chat that is not on the allowlist (anyone
  who found the bot, or a group or channel it was added to) is saved as text only,
  and its media is never downloaded. Its updates are bounded, and a spent budget
  denies (nothing is evicted to make room):
  - at most 50 updates per stranger chat per day;
  - at most 200 updates and 2 MB per day across all stranger chats together;
  - at most 32 stranger chat folders on the card; a further stranger's updates are
    recorded as one `not archived: stranger limit` line each in the `0` folder,
    inside the daily budget;
  - at most 64 stranger chats counted per day; past that, only a counter moves.

  Under storage pressure every stranger day is deleted before any day of an
  allowlisted chat, and the newest allowlisted day is never deleted, so stranger
  data can never push the owner's history out.
- **Read-only from the web.** The Files list (Memory, then Files) shows saved media as
  read-only `telegram-<chat>` folders; delete requests for them are refused. Only the
  storage-pressure policy (oldest days first, owner told once) and the factory reset
  remove archive files.
- **Import is bounded.** The restore upload accepts only `r<16 hex>.<ext>` names under
  a valid chat and day, at most 20 MB per file, one upload at a time, and never
  overwrites an existing file.
- **Erase.** A factory reset with storage erase deletes the whole `/mem` tree,
  archive included. As everywhere on the card, files are deleted, not overwritten; a
  computer format of the card is the way to make them unrecoverable.

### Outbound TLS certificate validation (done, default on)

Every outbound HTTPS client goes through the
single `tlsSetup()` seam (`src/sys/net_util.h`) instead of calling
`WiFiClientSecure::setInsecure()` directly. When `store::tlsVerify()` is true (NVS
`tlsVerify`, **default true**), `tlsSetup()` calls `setCACertBundle()` with the IDF's
embedded ~200-root bundle (`CONFIG_MBEDTLS_CERTIFICATE_BUNDLE_DEFAULT_FULL`, referenced
via the `_binary_x509_crt_bundle_{start,end}` linker symbols) - so the **server
certificate is validated**. An attacker on the network path (evil-twin AP the device
auto-joins, rogue router, DNS/ARP spoof) can no longer MITM a provider connection to
harvest the `Authorization: Bearer <provider-key>` headers or the Telegram bot token.
Every outbound HTTPS client (provider adapters, Telegram, connectors, voice, embeddings,
moderation, web fetch, sound sync, firmware updates) uses this seam. Two clients sit
outside it and validate unconditionally: the cloud relay, described next, and the
cloud key-mint POST (`src/net/cloud_mint.cpp`), which carries the device pairing
credential.

**Escape hatch:** clearing the "Validate provider TLS certificates" checkbox on the Assistant tab
(`POST /api/orch` with `tlsVerify=0`, surfaced in `/api/state`, backed by `store::setTlsVerify(false)`)
falls back to `setInsecure()` - for a
self-signed custom `orchHost` or a provider whose root isn't in the bundle (a connection
ERROR, not a security hole). ⚠ **Known limitation:** the flag is GLOBAL, not per-provider
- turning it off to accommodate a self-signed host also drops validation for the cloud
providers. Per-provider granularity is the follow-up if a self-hosted + cloud mix ever
needs both at once.

### Cloud relay socket always validates, never setInsecure (done)

The cloud relay client (`src/net/relay_client.cpp`, see [cloud-relay.md](cloud-relay.md))
does NOT go through `tlsSetup()`, so the `tlsVerify=0` escape hatch above can never weaken
it: `relayTlsSetup()` always calls `setCACertBundle()`. The relay is the remote-access
security boundary, so its connection is validated unconditionally. Tunneled requests are
already authenticated by the service (owner + subscription + ownership) before they reach
the device; the device injects its own LAN access token onto the loopback replay and that
token never leaves the device.

**Follow-up (hardening):** the endpoint is Cloudflare-fronted, so the relay currently
trusts any CA-bundle-valid certificate for `app.cumulo-nimbus.ai`. Pinning the issuing
CA / SPKI (in addition to the bundle check) would narrow trust to Cloudflare's issuer;
deferred because leaf certificates rotate on a ~90-day cycle and a bad pin bricks remote
access until an OTA. Bundle-root validation is the MVP posture.

⚠ **On-device verification is the gate, not the build.** A green link only proves the
bundle symbol resolved. Each provider must be confirmed to still CONNECT with validation
on (trigger `provider_verify` per provider → `result=1 verified`, plus one real turn) -
a provider whose root isn't bundled fails as a connect error until the flag is flipped.

### Tunnel secret containment: canonicalize, deny, scrub (done)

The loopback replay stamps a valid LAN token onto every tunneled request, so any endpoint
whose response reflects a durable secret must never be served over the tunnel. That
containment now lives in one portable, host-tested place
(`lib/core/.../cloud/tunnel_guard.*`, `test/test_tunnel_guard`) with three layers:

- **Canonicalize before the check.** The local web server percent-decodes and strips the
  query from a path before it routes, so the guard first reduces the raw tunneled path to
  the same form (percent-decode, drop the query, strip a trailing slash). A request for
  `/api/%63onnect` now resolves to `/api/connect` and is caught, where a raw string compare
  would have missed it and leaked the token and setup-AP password.
- **Deny the secret endpoints.** The canonical path is refused (403, the handler never
  runs) for `/api/connect`, `/api/token/regen`, and the sign-in endpoints
  `/api/signin/code` and `/api/signin/exchange`. The sign-in pair was the gap that let a
  hostile relay read a single-use code and exchange it for the durable token.
- **Scrub as a backstop.** Any tunneled JSON response has the `token` and `apPass` field
  values stripped before it is framed back, so even a future un-denied endpoint cannot
  carry those secrets off the device. (The sign-in `code` field is contained by the
  denylist instead of the scrubber, because that field name is also the legitimate
  cloud-pairing code in the relay-status response.)

The remote owner is already authenticated by the service and reaches the device on its own
network for anything the denylist withholds, so remote use is unaffected.

### Connector OAuth /oauth/go requires a per-flow launch key (done)

The MCP connector OAuth 2.1 flow (see the connector sign-in in the outbound MCP docs)
parks at consent for up to an hour with a state-bearing authorize URL ready to hand out.
`/oauth/go` cannot be token-gated - the owner opens it fresh on a phone - so an
unauthenticated caller used to read the CSRF `state` straight out of its 302 `Location`.
A LAN peer could then race the callback with their OWN provider account and bind their
refresh token to the owner's connector (account fixation).

Now the device mints a per-flow, 128-bit **launch key** at the authenticated flow start
and carries it ONLY on the owner's verify URL / web-UI QR (both owner-only paths).
`/oauth/go` returns the authorize URL only for a caller that presents that exact key
(`?k=`); a keyless or wrong-key request - any unauthenticated LAN peer - is sent to the
home page and never sees `state`. The gate decision is the pure, host-tested
`nimbus::orch::mcp::oauth::launchAuthorized` (fail-closed on an empty published or
presented key; `test_mcp_oauth` tables the caller classes). With `state` no longer
leakable, the callback's existing state-equality check is a real gate again.

### Outbound moderation exemption is provenance-based (done)

The outbound-reply screen ("Check replies sent to guests") skips the device's own
deterministic system copy so it never pays a classifier call on known-safe strings. That
exemption used to key on the reply TEXT (`startsWith(deviceName)`), which a guest could
prompt-inject the model into reproducing - beginning its reply with the device name turned
the screen off for that reply. The exemption is now an out-of-band provenance flag set by
the code path that emits genuine device copy, never inferred from the text; guest-steerable
content (a model reply, a job result) is always screened. The rule lives in the pure
`nimbus::orch::outboundExempt` (no text argument by construction, so no content can satisfy
it; host-tested in `test_orch_moderation`).

### Access token out of URLs (setup-network first-run redirect still open)

**Status: LAN paths and the device-screen QR are fixed; the setup-network first-run redirect is pending.** The
durable token is no longer carried in any LAN-facing URL, and the device-screen
Sign-in QR carries a single-use `?c=` code. What changed:

- **One-time exchange code.** A Sign-in QR / cross-origin link / Wi-Fi-handoff link now
  carries a short, single-use, TTL-bounded code (`?c=<code>`), not the token. The page
  POSTs it to `POST /api/signin/exchange` and receives the token in the response body,
  stores it client-side, and strips the URL. The code table is
  `nimbus::SigninCodes` (host-unit-tested, `test/test_signin_codes`); it is single-use
  and expires (default 2 min), so a copy of the link in synced history is inert once
  used or expired. `/api/connect` returns `?c=` links; `GET /api/signin/code` mints a
  fresh code for a caller that renders a QR.
- **Longer window for hand entry.** The scan path keeps the 2-minute TTL (a
  scan is instant), but the "Show code" fallback, for anyone who cannot scan, mints its
  own single-use code with a 10-minute TTL and shows a live mm:ss countdown, re-minting
  and repainting the moment it expires. So a code being read off the screen and typed
  into another machine has time to be entered, and a stale code never sits on screen as
  if valid. The web sign-in gate exchanges the typed code the same way the `?c=` link
  does, so the durable token is never entered by hand. It trims and lowercases what
  was typed (codes are lowercase hex; a phone keyboard capitalizes the first letter)
  and the field turns off autocapitalize and autocorrect.
- **A shown code is never evicted.** The table holds eight codes. A mint
  takes a free, used, or expired slot first and, only when every slot still holds a
  redeemable code, evicts the one closest to its own expiry. Before this the slots
  were reused round-robin, so the Sign-in QR's 2-minute scan codes (re-minted about
  every 90 s) could overwrite a 10-minute code the owner was still typing. The
  screen also replaces a code as soon as it was redeemed or evicted, not only when it
  expires.
- **Header-only downloads.** `/api/files/dl` is fetched as a Blob and handed to the
  browser via `URL.createObjectURL`, so the token rides the `X-Nimbus-Token` header and
  no `?t=`/`&t=` appears in a download link or the image preview.
- **API routes reject `?t=`.** `webAuthOk` reads the token only from the header or a
  form field, never a query param, so a leaked `?t=` deep link is inert on every API
  route.
- **Legacy links still work, with a hint.** The page still accepts a legacy `?t=` link
  once (existing QR codes / bookmarks), stores the token, strips it, and shows a one-time
  migration hint pointing the owner at the Sign-in QR.

**Device-screen QR (wired):** `configUrl()`/`setupUrl()` in `main.cpp` now
embed a single-use `?c=` code from `net::panelSigninCode()`. It mints off the same
`nimbus::SigninCodes` table the exchange endpoint redeems from - spinlock-guarded for
this main-task caller - and caches the code (re-minting only near expiry) so the QR
image stays stable across panel repaints yet is always live when scanned. So a scan
signs the browser in with no typing; a live phone scan is the one step not yet
verified on hardware.

**Still to do:** the unprovisioned setup-AP redirect and the captive-portal catch-all
still hand out `/?t=<token>` (the `GET /` AP branch and `onNotFound` in
`src/net/webui.cpp`). Switching them
to `/?c=<code>` needs hardware-in-the-loop verification of first-run + the Wi-Fi handoff.
Until then the client accepts the legacy `?t=` on those paths and shows the migration
hint. `POST /api/token/regen` remains the revocation lever.

The historical analysis that motivated this work is kept below.

**Why stripping the token from the address bar was not enough.** The navigation is
committed to the browser's history database *before* any script runs, so `replaceState`
cannot unwrite it - it only edits the current session-history entry. That left a
`http://<ip>/?t=<token>` record in history, and Chrome/Edge/Firefox **sync history and
bookmarks across machines while localStorage is never synced**. A second computer signed
into the same browser profile could therefore omnibox-autocomplete a token-bearing URL
and silently authenticate. Two related leaks are also closed: `/api/files/dl` no longer
opens a token-bearing tab, and the durable token is not placed in a URL for the clipboard
to sync.

**Revocation.** `POST /api/token/regen` rotates the token and invalidates every previously-issued code and link, including any sitting in synced history.

### Setup AP password - per-device random (fixed 2026-08-10)

The setup network password is now **generated per device**: on the first boot with
no stored value the device mints a 10-character random passphrase (hardware RNG,
lowercase + digits with the ambiguous `0/o/1/l` removed) and persists it in NVS
(`agent::store::setupApPass`). The fleet-wide shipped `nimbus1234` is gone from the
air - `NIMBUS_AP_PASS` survives in the source only as the fallback for a device
whose NVS is unusable. The passphrase appears **on the device's setup screen**
(printed, and embedded in the setup screen's Wi-Fi join QR so a phone camera joins
directly) and, for a signed-in owner, as **Setup Wi-Fi password** under
**Settings → Connectivity** (from the token-gated `GET /api/connect`). It is never
advertised over mDNS or the open `/api/state`, and never printed over serial. It
regenerates only on factory reset (`nvs_flash_erase` erases the whole namespace).

**Migration:** a device updated over OTA has no stored passphrase, so it generates
one on the first boot after the update. Its setup network only matters after a
factory reset or when Wi-Fi is lost - from this update on, the setup network
password is **the one shown on the device's screen**, not `nimbus1234`.

The setup AP exists only in **Orchestrator** mode; Notifier keeps Wi-Fi off. An
Orchestrator drops the setup AP after STA gets an IP (to protect the panel from the
beacon train) and restores it whenever STA is down. A new or disconnected
Orchestrator therefore still exposes the setup AP - now gated on the per-device
passphrase.

**TFT first-run handoff is bounded and authenticated.** `/savewifi` arms a maximum
20-second AP+STA overlap instead of allowing `GOT_IP` to cut off the response. The
wizard obtains the exact DHCP address, constructs a token-bearing continuation URL,
then acknowledges it with token-gated `POST /api/wifi/handoff`; the main task waits
four more seconds for that response to flush before stopping the AP. The browser
probes the exact IP and resumes at the provider step after the host rejoins its LAN.
It does not rely on potentially ambiguous mDNS. If the acknowledgement never arrives,
the 20-second fallback still protects the RF-sensitive TFT.

The station link and setup AP are reported as separate facts. In particular,
`STA connected + AP off` is the expected TFT steady state and must never be presented
as “Wi-Fi setup is down.” ConfigQr uses the reachable station URL in that state.
When the station is down but the setup AP is up (home Wi-Fi lost), ConfigQr instead
shows the setup network's name, its current password, and a Wi-Fi-join QR, so a
locked-out owner can rejoin from the device screen without needing the LAN it cannot
reach. SetupInfo shows the same AP-first credentials during first-run onboarding.
The password is on-glass only in both cases: it is never logged, never sent over
serial, and the `/api/connect` endpoint that carries it is blocked and redacted on
the tunnel.

**Join screens carry a sign-in code once Wi-Fi is set up.** A
provisioned device no longer signs AP peers in on its own (see below), so a person who
joins the setup network lands on the sign-in gate. The join screens the owner opens on
a provisioned Orchestrator therefore show a real device sign-in code beside the network
name and password: the locked-out Sign-in QR opened from the device menu and the Setup
screen. On both, tapping the code card opens the same code in large digits with its
countdown (the Sign-in code view, as under **Settings > Connectivity > Device sign-in
code**). Opened from the Setup screen, its back arrow returns to Setup, and it closes
when the Setup screen's time is up, like Setup itself. That code is the
10-minute hand-entry code from the same single-use table the gate redeems from, so what
the screen shows is what the gate accepts. The Sign-in QR that repeated failed sign-ins
bring up on their own never shows a code, since unauthenticated traffic can trigger it.
**Publish Setup Network** on the web page (**Connectivity > Recovery**), or **Publish
setup network** on the device (**Settings > Connectivity > Wi-Fi**), now shows that
Setup screen on the device and confirms on the ring, and the web button says the page
will disconnect when it is open over the home Wi-Fi. The Setup screen stays up for the code's 10 minutes, or until
Wi-Fi reconnects, then the status screen returns. First-run SetupInfo keeps its
numbered onboarding steps and shows no code: an unprovisioned device signs the owner in
automatically.

**Token handout is unprovisioned-only.** The AP interface auto-supplies the device
token (`GET /?t=<token>` redirect, and the captive-portal catch-all) so first-run
setup is one tap - but ONLY while the device is unprovisioned (no `staSsid` saved),
mirroring the `/savewifi` bootstrap exception. Once Wi-Fi is configured, an AP peer that
knows the AP password lands on the token-less identify gate like any LAN peer, so
RF range alone no longer grants full control. (Before this, the AP handed the
full-control token to anyone who joined with the then-shipped `nimbus1234`.)

## Reporting-only (not acted on)

Low-severity + deliberate-tradeoff items (multipart `Content-Length` ceiling, the
MCP-abort buffer leak, a few `snprintf`/`toCharArray` bounds nits, large-function
simplify candidates in `main.cpp`/`orchestrator.cpp`). Not fixed: refactoring working,
heavily-tested device code carries more regression risk than value.
