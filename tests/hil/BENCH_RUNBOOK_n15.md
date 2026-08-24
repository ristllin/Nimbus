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

### What a plain tunneled request does (NOT the reset)
`handleReq` -> `replayReq` -> `doLoopback` (`relay_client.cpp:147`). Loopback does up to two
blocking `WiFiClient::connect(..., 4000)` (127.0.0.1 then the STA IP = up to 8 s) plus a
15 s read deadline (`kLoopbackTimeoutMs`), all on the relay task. On failure the caller
frames an HTTP 504 "device timeout" back to the browser (`relay_client.cpp:296`, body at
`:322`). This is a graceful gateway-timeout: by itself it does NOT reboot the device.

### The reset levers (audit result)
There is NO `esp_restart()`/`abort()` on the tunnel path. A tunnel-correlated reset is a
side effect, one of:

1. **Watchdog panic from the single TLS work slot.** The relay's pairing / credential
   re-mint path `httpsPostJson` (`relay_client.cpp:333`) does
   `agent::arbiter::acquireWork(15000)` then a 12 s synchronous TLS connect
   (`kConnectTimeoutMs = 12000`) while HOLDING the one work slot
   (`src/sys/tls_arbiter.cpp`, default 1 slot). The plain loopback replay does NOT take the
   arbiter, so an ordinary tunneled GET/POST does not contend - only pairing / re-mint do.
   If any watchdog-fed main-loop path waits on that slot while the relay holds it for the
   12 s connect, the 8 s TWDT fires -> panic reboot. `15000 > 8000` and `12000 > 8000` are
   the mismatched budgets to watch. Reset cause decodes to `crash(watchdog)`
   (`main.cpp` `ESP_RST_TASK_WDT`).
2. **OOM reboot from an in-flight oversized frame** (already mitigated, verify caps hold):
   inbound frame cap `kMaxInboundFrame = 16*1024`, loopback read buffer moved off the task
   stack to heap (`relay_client.cpp:162-166`), response body cap `kMaxRespBody = 256*1024`
   in PSRAM with a graceful "res drop (no mem)" fallback.

The following "lag" is the reboot + relay reconnect/backoff cycle
(`applyCloseCodePolicy`).

### Fix (IMPLEMENTED - lane N15, second handback)
Keeps the frozen invariants (single TLS slot, no on-device concurrency). `httpsPostJson`
now bounds its WHOLE slot-hold (connect AND read) to a budget derived from the watchdog:
`nimbus::cloud::relaySlotHoldBudgetMs(kTaskWdtTimeoutMs)` = 8000 - 2500 = **5500 ms**, so
the slot is never held past ~5.5 s, leaving 2.5 s for a fed waiter to take the freed slot
and reset the dog before the 8 s panic. `kTaskWdtTimeoutMs` is the SINGLE source used by
both the watchdog config (`main.cpp`) and the budget, so they cannot drift. The budget math
is host-tested (`test/test_relay_timing`). Over budget -> the pairing / re-mint returns its
error and retries on the next relay tick; never a reset.

**Timing instrumentation (device evidence):** every `httpsPostJson` now logs to the agent
log (GET /api/log):
```
relay: httpsPost held=<ms> budget=5500ms wdt=8000ms status=<n> ok
```
`ok` means the hold stayed within budget; an `OVER-BUDGET` line would be the old bug
signature. Bench verify: pair / re-mint over the relay and confirm every line reads `ok`
and no reset follows a forced 504.

### Bench procedure (manual)
1. Lock the port; USB-reset-clear if the console is wedged (CUM-141 pyusb recipe in
   `/tmp/nimbus-devices/MAP.md`).
2. Flash the `test` (or `test-cyd`) firmware; provision Wi-Fi; pair the cloud link so the
   relay is live.
3. Drive tunneled requests through the relay while forcing a slow local answer (a heavy
   endpoint or a deliberately paused handler) to provoke the 504.
4. Watch the serial console for the reset banner and read the decoded reset cause on the
   next boot (`STATUS` / boot log). Record whether it is `crash(watchdog)` (lever 1) or an
   allocation failure (lever 2).
5. Release the lock.
Pass after fix = repeated forced 504s return gracefully with NO reset across the run.

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
