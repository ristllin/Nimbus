# Nimbus Security Posture + TODOs

Security model for the device's network surfaces, and the open items deferred by the
owner. This page is the reference for the auth model and open items.

## Threat model

A personal desk device on the owner's home LAN. Primary adversaries: another host on
the same LAN, anyone in RF range of an active Orchestrator setup AP
(`Nimbus-setup`) or the Notifier BLE radio, and an attacker on the internet path to
the providers (evil-twin AP, rogue router, DNS/ARP spoof). Not an internet-exposed
server - findings are weighted by real reachability.

## In place (implemented + on-device verified)

- **Per-device web/MCP auth token.** `store::webAuthToken()` (96-bit hardware-random,
  NVS, no setter). `net::webAuthOk()` checks `X-Nimbus-Token` (header or form field,
  never a `?t=` query param since CUM-45) constant-time
  and fails closed. Gates **every** route, reads included - `GET /api/state`,
  `/api/log`, `/api/connect`, `/api/sdprobe` and the LAN `POST /mcp` among them. The
  only two ungated responses are the ones the gate itself needs: `GET /` (the static
  shell, which carries no device data - every value is a placeholder filled in later by
  a gated fetch) and `GET /logo.svg` (the gate's own artwork). Two deliberate bootstrap
  exceptions exist, both **unprovisioned-only**: `/savewifi` (first provision), and the
  AP-interface token handout on `GET /` + the captive-portal catch-all, which `302`s to
  `/?t=<token>` for a peer on the setup AP while no `staSsid` is saved (detailed under
  *Setup AP + shipped password* below). Once Wi-Fi is configured, neither fires.
  Delivered automatically via the **Sign-in QR** (`configUrl()` embeds `?t=`); the
  token is not a separate onboarding step. Requiring it also defeats CSRF. Verified:
  no/wrong token → 401, valid → 200.
  ⚠ **No auth decision is made from the request's `Host`, `Origin`, `Referer`, or client
  IP.** Reaching the device by raw IP is therefore not a way around the gate. It can
  *look* like one: the browser stores the token per ORIGIN, so a browser that identified
  at `http://<ip>` is silently authenticated there while `http://<name>.local` - a
  different origin - still shows the gate. Same device, same gate, two storage buckets.
- **Secret redaction in logs.** Every line written to the agent log ring (served by
  the token-gated `GET /api/log`) passes through `core::LogRing::redact` at the one
  `logring::put` choke point. Two layers: the provider keys and the Telegram bot
  token are registered as exact secrets at boot (masked wherever they appear), and a
  heuristic backstop strips `Bearer <token>`, `api_key=`/`"key":"..."` values, and
  `user:pass@host` URL credentials, so a provider error body echoed into the log
  cannot carry a key onto the ring. Host-tested (`test_logring`). The endpoint is
  also token-gated, so redaction is defence-in-depth rather than the only barrier.
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
- **Danger-zone actions are typed-confirm gated, each distinct.** Erase Storage
  (`ERASE STORAGE`), Factory Reset (`FACTORY RESET`), and full-card Format
  (`FORMAT CARD`) each require their OWN exact phrase, so one confirmation can never
  trigger a heavier action than the owner meant (`nimbus::orch::confirmOk`, one
  source of truth, host-tested). All three are token-gated POSTs and are deferred to
  the main loop (never erased on the web task). **Factory Reset preserves the device
  identity** (the user-visible name) across the wipe and can optionally erase the SD
  card in the same flow; everything else (keys, token, bonds, config) is fresh.
  Full-card format needs a board-support driver primitive that does not exist yet,
  so `/api/sdformat` reports that honestly until it lands.
- **Telegram allowlist fails CLOSED.** An empty allowlist rejects all chats (was
  fail-open = allow-all); the poll task warns loudly if a token is set with no allowlist.
- **Shared-engine mutex.** `memory::Lock` (recursive) serializes VectorMemory /
  Scratchpad / episodic access across the AsyncTCP web/MCP task + the Telegram turn
  task (fixes an iterator-invalidation / heap-corruption race). Never held across a
  blocking TLS embed.
- **Notifier BLE link is bonded + encrypted.** In Notifier mode the ring/screen are
  driven only over BLE. The GATT server requires a **bonded, encrypted** link (LE
  Secure Connections, Just Works) to write frames (`WRITE_ENC` on FRAME) or read/write
  CONFIG - so an unbonded central in RF range can connect and read the ungated STATUS
  telemetry (protocol/fw version + a seq counter) but **cannot paint the ring or
  read/write config**. This closed the v1 open-radio hole (anyone in range could paint
  the ring). Just Works (not MITM/passkey - macOS won't surface a passkey dialog for a
  background-CLI-paired custom peripheral) drops only active-MITM
  protection during the one-time bond; the link stays ECDH-encrypted (passive-sniff
  safe). Bonds persist in NVS. Verified on hardware: unbonded write rejected; bonded
  frame applies; bond survives reboot.

## Open TODOs (owner-deferred 2026-07-04)

### DONE(security): outbound TLS certificate validation - CA bundle

**Status: IMPLEMENTED (default ON).** Every outbound HTTPS client now goes through the
single `tlsSetup()` seam (`src/agent/net_util.h`) instead of calling
`WiFiClientSecure::setInsecure()` directly. When `store::tlsVerify()` is true (NVS
`tlsVerify`, **default true**), `tlsSetup()` calls `setCACertBundle()` with the IDF's
embedded ~200-root bundle (`CONFIG_MBEDTLS_CERTIFICATE_BUNDLE_DEFAULT_FULL`, referenced
via the `_binary_x509_crt_bundle_{start,end}` linker symbols) - so the **server
certificate is validated**. An attacker on the network path (evil-twin AP the device
auto-joins, rogue router, DNS/ARP spoof) can no longer MITM a provider connection to
harvest the `Authorization: Bearer <provider-key>` headers or the Telegram bot token.
All 13 clients converted: `telegram.cpp`, `openai_adapter`, `anthropic_adapter`,
`mistral_adapter`, `custom_adapter`, `embeddings`, `tavily`, `tts_voices`, `audio_tts`,
`http_multipart`, `provider_verify`, `connectors`, `sfx_sync`.

**Escape hatch:** `store::setTlsVerify(false)` (the web UI checkbox (Capabilities → Models) → `POST /api/orch`
`tlsVerify=0`, surfaced in `/api/state`) falls back to `setInsecure()` - for a
self-signed custom `orchHost` or a provider whose root isn't in the bundle (a connection
ERROR, not a security hole). ⚠ **Known limitation:** the flag is GLOBAL, not per-provider
- turning it off to accommodate a self-signed host also drops validation for the cloud
providers. Per-provider granularity is the follow-up if a self-hosted + cloud mix ever
needs both at once.

### DONE(security): cloud relay socket always validates (never setInsecure)

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

### DONE(security): tunnel secret-containment (canonicalize, deny, scrub)

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

### Access token out of URLs (largely implemented 2026-08-23)

**Status: LAN paths fixed; device-screen QR + AP first-run redirect pending.** The
durable token is no longer carried in any LAN-facing URL. What changed (CUM-45):

- **One-time exchange code.** A Sign-in QR / cross-origin link / Wi-Fi-handoff link now
  carries a short, single-use, TTL-bounded code (`?c=<code>`), not the token. The page
  POSTs it to `POST /api/signin/exchange` and receives the token in the response body,
  stores it client-side, and strips the URL. The code table is
  `nimbus::SigninCodes` (host-unit-tested, `test/test_signin_codes`); it is single-use
  and expires (default 2 min), so a copy of the link in synced history is inert once
  used or expired. `/api/connect` returns `?c=` links; `GET /api/signin/code` mints a
  fresh code for a caller that renders a QR.
- **Header-only downloads.** `/api/files/dl` is fetched as a Blob and handed to the
  browser via `URL.createObjectURL`, so the token rides the `X-Nimbus-Token` header and
  no `?t=`/`&t=` appears in a download link or the image preview.
- **API routes reject `?t=`.** `webAuthOk` reads the token only from the header or a
  form field, never a query param, so a leaked `?t=` deep link is inert on every API
  route.
- **Legacy links still work, with a hint.** The page still accepts a legacy `?t=` link
  once (existing QR codes / bookmarks), stores the token, strips it, and shows a one-time
  migration hint pointing the owner at the Sign-in QR.

**Still to do (owned outside the web layer):** the on-device Sign-in QR string is built
by `configUrl()`/`setupUrl()` in `main.cpp` (device-screen owner), and the unprovisioned
setup-AP redirect + captive-portal redirect still emit `/?t=<token>`. These are the
first-run paths; switching them to `/?c=<code>` (via `GET /api/signin/code` or the same
mint) needs hardware-in-the-loop verification of first-run + the Wi-Fi handoff. Until
then the client accepts the legacy `?t=` on those paths and shows the migration hint.
`POST /api/token/regen` remains the revocation lever.

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
whose NVS is unusable. The passphrase appears **only on the device's setup screen**
(printed, and embedded in the setup screen's Wi-Fi join QR so a phone camera joins
directly); it is never advertised over mDNS or any API. It regenerates only on
factory reset (`nvs_flash_erase` wipes the whole namespace). HIL can read it with
the TEST-console `APINFO?` command.

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
as “Wi-Fi setup is down.” ConfigQr uses the reachable station URL in that state;
SetupInfo alone owns the AP-first onboarding instructions.

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
