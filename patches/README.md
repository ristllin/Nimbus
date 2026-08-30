# Pending driver patches (for the orchestrator)

## solide-drivers-0.7.2-healthy-flip-aware.patch

The real fix for CUM-231 (white-screen regression), against solide-drivers **v0.7.1**.
Touches `src/device/display_tft.cpp` (`healthy()`) and `CHANGELOG.md`.

**What it does.** v0.7.1's `healthy()` masked the MY/MX flip bits (0xC0) out of the
RDDST compare on both sides (`kMask=0x3E`) to stop the flip=1 watchdog thrash. That
also dropped fault detection: a partial state loss that raises MY/MX in RDDST
(got=0xE8) while the fixed bits stay correct read HEALTHY. v0.7.2 instead compares the
full byte (minus the refresh scan-toggle bit0) against `madctlFor(g_flip)` with the
flip bits cleared - `(got & 0xFE) == (madctlFor(g_flip) & ~0xC0)`, which is 0x28 for
both orientations. Keeps the no-thrash property; restores detection of got=0xE8; reset
(0x00) still fails.

**How the orchestrator lands it:**
1. `git -C <solide-drivers> checkout v0.7.1 && git apply <this patch>` (or cherry-pick
   the equivalent commit), review the one-line compare change.
2. Tag **v0.7.2** and push (driver repo: github.com/ristllin/solide-drivers).
3. Bump the nimbus pin: `platformio.ini` line 63 `...solide-drivers.git#v0.7.1` ->
   `#v0.7.2`, and the note at line 7 / the pin-bump commit message
   (`2b7a3fa`-style).

**Verification already done (host):** nimbus `test/test_panel_health` pins the exact
compare arithmetic and asserts both directions - v0.7.0 thrash at flip=1, v0.7.1 mask
miss at got=0xE8, and v0.7.2 correct on all. `pio test -e native -f test_panel_health`.

**Bench verify still owed** (CUM-231 DoD 4): repro white on v4.4.3 in setup mode, then
confirm the fix on the TFT. Blocked on device availability (owner onboarding nimbus-6);
lane TF-F6B will patch `.pio/libdeps/*/solide-drivers` locally for the bench build when
a device window opens (host-side first per TASK.md).

**Open RCA item (see CUM-231 comment):** the driver fix corrects `healthy()`'s
semantics, but a *stuck* white in setup mode is only fully explained together with the
firmware `renderAndPush()` dirty-gate probe (`src/hw/tft_out.cpp:142`). See the lane's
`src/hw/tft_out.cpp` change and the CUM-231 DECISION comment.
