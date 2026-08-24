# N15 bench runbook - CUM-160 triage: tunnel gateway-timeout reset + 180 touch

Triage of two field symptoms, done host-side on current `main` (base `3e8db03`), with
the exact code sites and a bench procedure to confirm each on hardware. Whoever runs
this holds the device lock per the fleet-lane protocol (create
`/tmp/nimbus-devices/<port>.lock`, 45 min max, release after). Nimbus-4
(`usbmodem2101`) is the personal board - restore its NVS from
`~/nimbus-bench/nimbus4-backup/` when done. Freenove (`usbmodem101`) is the scratch board.

The touch injection console path (`NIMBUS_TEST` `injectTap`) deliberately bypasses the
real read + flip mirror, so the 180 symptom cannot be reproduced through the HIL console -
it needs a physical tap. Both checks below are therefore manual.

---

## Symptom A - tunnel gateway-timeout then device RESET then lag

### Architecture
The "tunnel" is the cumulo-nimbus cloud relay (`src/net/relay_client.cpp`): one resident
outbound WSS the device dials, answering browser requests by replaying them into its own
local `AsyncWebServer` over loopback. The relay runs on its own FreeRTOS task pinned to
core 0 (`relay_client.cpp` `begin()` -> `xTaskCreatePinnedToCore(relayTask, ..., 0)`), and
that task is NOT subscribed to the task watchdog. The only watchdog-subscribed task is the
main Arduino loop on core 1 (`src/main.cpp`: TWDT 8000 ms, `trigger_panic=true`;
`esp_task_wdt_add(nullptr)` at end of setup; fed once per `loop()`).

### The REAL 502 root cause (CUM-173, third handback - instrument-proven)
The tunnel path is `handleReq` -> `replayReq` -> `doLoopback`. Two field theories were
DISPROVEN on the bench and replaced by the real mechanism:

1. **"127.0.0.1 loopback is refused"** - FALSE on current main. Instrumented CLOUDLOOP on
   the Freenove: `relay: loopback via=127.0.0.1 conn=2ms status=200` for `/`, `/api/state`,
   `/logo.svg`, `/api/signin/code`. `CONFIG_LWIP_NETIF_LOOPBACK=y` is set for esp32s3, so
   127.0.0.1 connects in ~2 ms and serves every route. The latent bug there was the
   FALLBACK: `WiFi.localIP()` can be `0.0.0.0` just after a (re)join, and the old code dialed
   it -> guaranteed 502. Hardened: 127.0.0.1 primary, STA fallback only when usable
   (`nimbus::cloud::loopbackFallbackUsable`, host-tested), short connect timeout (no 4 s stall).
2. **THE actual 502: response OVERFLOW.** The config page (`GET /`) is ~278 KB but the old
   loopback body cap `kMaxRespBody` was 256 KB (`262144`). The parser flagged `overflow()`
   and `handleReq` turned every tunneled `GET /` into a 5xx - the field white-screen + bad
   gateway. It serves fine to LAN clients (no cap). CLOUDLOOP hid this because
   `loopbackSelfTest` returns `rp.status()` and ignores overflow; `handleReq` does not.
   The exact evidence: `CLOUDLOOP / -> bytes=262144` (the body truncated at the cap).
   FIX: `kMaxRespBody` raised to `nimbus::cloud::kLoopbackMaxRespBody` = 320 KB (PSRAM-backed;
   base64(320 KB) = ~437 KB, under the 512 KB res-frame max). A host regression guard
   (`test/test_loopback_capacity`) asserts the assembled page (webui snapshot) + 16 KB
   headroom fits the cap, so the NEXT page growth fails the battery, not the field. The
   overflow backstop now returns an EXPLICIT 500-with-reason (distinct log tag) instead of a
   bare 502, so the cloud maps it to the "Reaching your Nimbus" interstitial.

### CUM-160 slot-hold: corrected in this handback
The earlier CUM-160 fix bounded `httpsPostJson`'s whole slot-hold to 5500 ms and ABORTED
past it. That was a regression: a real relay/Cloudflare TLS connect legitimately takes
many seconds (12 s+ is latency, not a hang), and the abort stopped the device reconnecting.
Corrected: NO connect-abort (the relay task is not watchdog-subscribed, so its blocking
connect cannot trip the 8 s WDT - proven by the old unbounded build reconnecting fine); the
READ runs as watchdog-fed, bounded steps via `nimbus::cloud::driveStagedWait` (no single
step over `relayStepMs` = 2 s, feeds between steps), host-tested with a synthetic 10 s op
(`test/test_relay_timing`: completes, never aborts, feeds occur). Hold-instrumentation kept:
```
relay: httpsPost held=<ms> connect=<ms> status=<n>
```

The old OOM levers stay mitigated (`kMaxInboundFrame = 16*1024`, heap loopback read buffer,
PSRAM res body with a graceful "res drop (no mem)" fallback).

**Loopback instrumentation (device evidence, CUM-174 gate asserts on it):** every
`doLoopback` logs `relay: loopback via=<target> conn=<ms> status=<n> bytes=<n>`, or
`relay: loopback REFUSED via=<target> conn=<ms> (tunnel 502)` on a hard refusal.

### Bench procedure (manual)
1. Lock the port; USB-reset-clear if the console is wedged (CUM-141 pyusb recipe in
   `/tmp/nimbus-devices/MAP.md`).
2. Flash the `test` (or `test-cyd`) firmware; provision Wi-Fi (no cloud pairing needed).
3. **CLOUDLOOP self-test (no cloud dependency, drives the exact tunnel path):**
   - `CLOUDLOOP /api/state` -> expect `-> 200` and a log line
     `relay: loopback via=127.0.0.1 conn=~2ms status=200 bytes=<n>`.
   - `CLOUDLOOP /` -> expect `-> 200` and `bytes=<full page size ~278KB>`, NOT `bytes=327680`
     (the cap). Before the cap fix this showed `bytes=262144` (truncated/overflow).
   Verified on the Freenove 2026-08-24: all four routes 200, `via=127.0.0.1 conn=2ms`,
   `CLOUDLOOP / bytes=277882` after the cap fix.
4. Field confirm (owner, Nimbus-4 on merged main + v0.6.1): open `d.cumulo-nimbus.ai`, sign
   in, navigate. Expect the full page (no white screen / bad gateway) and, over a
   pair/re-mint, `relay: httpsPost held=<ms> connect=<ms> status=200` with the relay coming
   ONLINE and staying online across reboots (the CUM-160 reconnect regression is gone).
5. Release the lock.
Pass = CLOUDLOOP `/` returns 200 with the full page bytes (no overflow), and the owner's
tunnel login loads and the relay reconnects.

---

## Symptom B - touch input reversed 180 degrees

### Where the flip lives
Two transforms. The driver calibration produces landscape pixels
(`solide/touch.h` `Calibration`: `swapXY=true, invertY=true` for the mounted panel). The
180 mirror is in `src/hw/touch_input.cpp:84-87`:

```cpp
if (down && solide::display_tft::flipped()) {
  x = solide::display_tft::kW - 1 - x;   // 319 - x
  y = solide::display_tft::kH - 1 - y;   // 239 - y
}
```

The mirror runs only when `display_tft::flipped()` is true, and must track the MADCTL
display flip set from `agent::store::tftFlip()` (boot: `main.cpp`; live: the settings menu,
the web toggle, and the `TFTFLIP` console command). A 180 reversal means the touch mirror
and the display flip DISAGREE: taps land at the diagonally opposite point.

### Fix (IMPLEMENTED - lane N15, second handback)
Root cause is TWO sources of truth for the 180: the touch calibration's invert flags AND
the runtime flip mirror could each try to handle it, so they could double-apply (taps 180
out) or disagree. The fix makes the DISPLAY FLIP the single source of truth: the 180 is now
applied by one pure, host-tested function `nimbus::touch::orientTouch(point, displayFlipped,
w, h)` (`lib/core/.../touch_cal.cpp`, tests in `test/test_touch_cal`), which `touch_input.cpp`
calls instead of the inline mirror. The calibration maps ONLY the mount (canonical,
un-flipped landscape); the flip is applied there exactly once, so it can never double-apply.

**Reachable calibration path (already user-facing, confirmed):** the web Device tab exposes
a **Display flip** toggle (flips display + touch together now, applies immediately), a
**Touch calibration** field (minX,maxX,minY,maxY,flags), and a **Touch orientation** control
for the capacitive panel ("toggle these until a tap lands where you touch"); the console has
`TFTFLIP` and `TOUCHCAL`. So a 180 is correctable on-device with no reflash.

### Bench check (physical taps, confirms the field report is closed)
The polarity of a given mount is still a physical fact; the console inject path bypasses the
orient branch, so verify with real taps:

### Bench procedure (manual, physical taps)
1. Lock the port; flash `test` firmware.
2. Read state: `STATUS` (note the display flip / `tftFlip`), and confirm `scr=tft`.
3. Render a screen with a known-position target (e.g. a corner button). Physically tap it.
   Confirm the tap registers on the target, not the diagonally opposite corner.
4. Toggle the flip (`TFTFLIP` console command or the web toggle) and repeat the tap test in
   both states. The correct configuration is the one where the tapped target responds in
   the orientation the display is actually mounted.
5. If reversed, apply the matching fix above, reflash, and re-verify all four corners.
6. Restore Nimbus-4's original `tftFlip` / NVS when done (personal board).
Pass = taps land on the on-screen target in the mounted orientation, all four corners.

---

## Symptom C - relay refuses to dial: "Not enough memory right now" (CUM-167)

### Root cause (instrument-proven, NOT a lane-N15 code regression)
On final main Nimbus-4's /api/state reports cloud state=disabled, err="Not enough memory
right now": the relay heap-floor guard (heapFloorOk) refused to dial. Measured: internal
free ~26 KB but the largest free internal block dipped below the old 8000 floor.

The cause is solide-drivers **v0.6.1** (CUM-167 white-screen fix): full frames from PSRAM
are staged through a persistent **internal DMA bounce buffer**
(`display_tft.cpp` `kBlitBandRows=8` -> `8*320*2 = 5120 B`, `MALLOC_CAP_INTERNAL|MALLOC_CAP_DMA`).
That 5 KB block, allocated at display init, sits mid-heap and SPLITS the largest free
internal block below 8000 - so the relay's contiguous-block floor failed even with ample
total free. It is NOT the N15 third-handback code: static RAM is byte-identical (83068)
across b69b2bb, my HEAD, and 32703ed, and the relay's big buffers (TLS/body/frame) are
PSRAM-backed. The buffer is essential (a PSRAM DMA burst resets the panel) and cannot move
to PSRAM; it can only be shrunk in the driver (owner: CUM-167).

### Fix (lane N15 scope)
The relay's real internal-contiguous demand is only the 4 KB WS handshake head - the floor's
8000 was headroom assuming "largest ~13 KB is normal", which the bounce buffer broke. Lowered
`kRelayHeapFloorLargest` 8000 -> 5000 (host-tested in `test/test_relay_heap`; still clears the
4 KB handshake need with margin, free floor unchanged so genuine starvation still refuses).
The guard now RE-CHECKS every 10 s (was 60 s) and logs the live numbers
`relay: heap floor - free=<n> largest=<n> ...` so it comes online promptly on recovery. The
relay's own transient loopback buffer moved to prefer PSRAM.

### Bench numbers (before/after intLargest)
- BEFORE (floor 8000): /api/state cloud.state=disabled, err="Not enough memory right now".
- AFTER (floor 5000): with the same intFree/intLargest, relayCanDial returns true; /api/state
  cloud dials (Idle/Connecting/Online). Read `mem.intFree` / `mem.intLargest` from /api/state
  before and after. Field-verify on Nimbus-4 (owner) that the cloud link comes up.
