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
  NVS, no setter). `net::webAuthOk()` checks `X-Nimbus-Token` (or `?t=`) constant-time
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
- **Secret redaction in logs.** The Telegram token (embedded in the `/bot<token>/…`
  send path) is redacted before it can reach `GET /api/log`. That endpoint is itself
  token-gated, so the redaction is defence-in-depth rather than the only barrier.
- **Telegram allowlist fails CLOSED.** An empty allowlist rejects all chats (was
  fail-open = allow-all); the poll task warns loudly if a token is set with no allowlist.
- **Shared-engine mutex.** `memory::Lock` (recursive) serializes VectorMemory /
  Scratchpad / episodic access across the AsyncTCP web/MCP task + the Telegram turn
  task (fixes an iterator-invalidation / heap-corruption race). Never held across a
  blocking TLS embed.
- **Notifier BLE link is bonded + encrypted.** In Notifier mode the ring/e-ink are
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

⚠ **On-device verification is the gate, not the build.** A green link only proves the
bundle symbol resolved. Each provider must be confirmed to still CONNECT with validation
on (trigger `provider_verify` per provider → `result=1 verified`, plus one real turn) -
a provider whose root isn't bundled fails as a connect error until the flag is flipped.

### TODO(security): get the access token out of URLs

**Status: OPEN - design note, not yet implemented.** The token is delivered as a query
parameter (`?t=<token>`) and that is currently the *only* delivery mechanism:
`configUrl()`/`setupUrl()` build the Config-QR link, the unprovisioned-AP redirect uses
it, `/api/connect` returns `url`/`mdnsUrl`, and `/api/files/dl` puts it in every
download link. The page strips it from the address bar
(`history.replaceState`, `ui_js.h`) as soon as it is durably stored.

**Why the strip is not enough.** The navigation is committed to the browser's history
database *before* any script runs, so `replaceState` cannot unwrite it - it only edits
the current session-history entry. That leaves a `http://<ip>/?t=<token>` record in
history, and Chrome/Edge/Firefox **sync history and bookmarks across machines while
localStorage is never synced**. A second computer signed into the same browser profile
can therefore omnibox-autocomplete a token-bearing URL and silently authenticate - which
is exactly what "the IP let me straight in on a machine I'd never used" turns out to be.
Two related leaks: `/api/files/dl?…&t=<token>` opens in a new tab (`target=_blank`), so
it lands in history *and* download history and is never stripped (no script runs on a
binary response); and the Connectivity panel's copy-token control puts the token on the
system clipboard, which macOS Universal Clipboard and Windows Cloud Clipboard sync
across devices.

**Blast radius (why this is a TODO, not an incident).** Everything above stays inside
surfaces the owner already controls - their own synced browser profile and their own
clipboard. It is not remotely reachable and grants nothing an owner does not have. What
it does defeat is the *revocation* story: rotating the token cannot claw back copies
sitting in synced history on machines you have forgotten about.

**Options when picked up** (rough order of value/effort):
1. **One-time exchange code.** The QR carries a short-lived, single-use code; the page
   POSTs it and receives the real token in the response body. Nothing durable ever
   appears in a URL. Needs a small server-side code table with a TTL.
2. **Header-only downloads.** Replace the `?t=`-bearing `/api/files/dl` anchors with a
   `fetch` + `Blob` + `URL.createObjectURL` download so the token rides the
   `X-Nimbus-Token` header like every other request. Self-contained, no protocol change.
3. **Scope the QR token.** Issue a separate, rotatable "enrollment" secret for the QR so
   the long-lived control token is never the thing printed on the panel.
4. **Belt-and-braces:** keep `replaceState`, and additionally accept `?t=` only on `GET /`
   (never on API routes) so a leaked deep link is inert.

Until then, `POST /api/token/regen` (Settings → Connectivity) is the mitigation: it
invalidates every previously-issued URL, including any sitting in synced history.

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

The setup AP exists only in **Orchestrator** mode; Notifier keeps Wi-Fi off. On an
e-ink Orchestrator it stays reachable, while a TFT Orchestrator drops it after STA
gets an IP (to protect the panel from the beacon train) and restores it whenever
STA is down. A new or disconnected Orchestrator therefore still exposes the setup
AP - now gated on the per-device passphrase.

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

**Remaining (deferred):** consider applying the TFT's post-join AP teardown to
e-ink devices.

## Reporting-only (not acted on)

Low-severity + deliberate-tradeoff items (multipart `Content-Length` ceiling, the
MCP-abort buffer leak, a few `snprintf`/`toCharArray` bounds nits, large-function
simplify candidates in `main.cpp`/`orchestrator.cpp`). Not fixed: refactoring working,
heavily-tested device code carries more regression risk than value.
