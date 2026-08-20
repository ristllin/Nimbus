# Plan: Nimbus support for the Freenove ESP32-S3 Display (CYD) all-in-one board

Status: IMPLEMENTED (this branch) after a four-lens prism review of the plan AND the
code. Board bring-up (display + touch) validated on hardware; audio/SD/battery/ring
smoothness pending owner bench validation (tests/hil/MANUAL_freenove_cyd.md). See the
"Next TODOs" section at the end for the deferred multi-resolution work and the landing
step. Target: worktree `freenove-cyd-variant` + driver branch `feat/freenove-cyd`.

> The v1 of this plan rested on `board.h`'s promise that a new board is "a new Board
> constant + `-DSOLIDE_BOARD`, with zero driver changes." **Prism proved that promise
> false.** The driver layer hardcodes `kBoardSolideS3` by name, and the whole
> flash/provision/OTA entry path assumes one board. This v2 folds in every confirmed
> finding. The "what prism changed" appendix is at the end.

## 0. What the device is

Amazon `B0FSQF6FKN` = **Freenove ESP32-S3 Display**, model **FNK0104B**: 2.8" IPS
capacitive touchscreen, **ILI9341** panel at **240x320** (landscape 320x240),
**ESP32-S3 N16R8** (16 MB flash / 8 MB PSRAM, native USB), on-board microSD (SDMMC),
mic + speaker via an **ES8311** codec, a **single** WS2812 RGB LED (no 45-ring), battery
port. Same MCU family as the Solide S3 board, so platform pin, partitions, and the
Wi-Fi/BLE/cloud/orchestrator stacks are unchanged. On the bench now as MAC
`28:84:85:42:18:FC`.

### Confirmed pin map (FNK0104B, from Freenove example sources)

| Function | GPIO |
|---|---|
| RGB LED (WS2812, **1 px**) | 42 |
| Battery ADC (x2 divider) | 9 |
| Touch **FT6336U** (capacitive, I2C) | SDA 16, SCL 15, INT 17, RST 18 |
| **ES8311** audio codec | I2S MCK 4, BCK 5, DIN(mic) 6, DOUT(spk) 8, WS 7; I2C SDA 16 / SCL 15 (**shared with touch**), addr 0x18; amp-enable |
| microSD **SDMMC 4-bit** | CLK 38, CMD 40, D0 39, D1 41, D2 **48**, D3 47 |
| Display ILI9341 (SPI) | **SCLK/MOSI/MISO/CS/DC/RST/BL = TBD** - read from the Freenove **schematic** (the BSP is raw-SPI, there is no TFT_eSPI `User_Setup` to consume). Resolve before any hardware step (Phase 1). Verify none land on S3 strapping pins 0/3/45/46 or octal-PSRAM pins 33-37. |

Strapping-pin check on the known pins: 42, 38/40/39/41/48/47, 9, 15/16, 4-8, 17/18 - none
collide with 0/3/45/46 or PSRAM 33-37. Only the display-SPI row is unresolved.

The genuinely new drivers are **FT6336U** (capacitive touch), **ES8311** (audio codec),
and **SDMMC** (SD). The panel controller (ILI9341) and resolution (320x240 landscape) are
identical to the existing `tft` variant.

## 1. Corrected understanding of the current architecture (post-prism)

- One NVS string `scrModel` (`"eink"|"tft"`, accessor `screenModel()`, key constant
  `AKEY_SCREEN_MODEL`) is read once at boot into `g_screenIsTft`
  (call at `src/main.cpp:2359`) and selects renderer + input family.
- **The pinout lives in the sibling `solide-drivers` library, but the drivers are NOT
  board-parameterized.** `board.h:8-10` claims "zero driver changes," yet every driver
  binds pins to the `solide_s3` constant *by name*, at compile time:
  `display_tft.cpp:30-36`, `leds.cpp:9-10`, `storage.cpp:8-11`, `touch.cpp:21`,
  `input.cpp:8-10`, `audio.cpp:20-21` all read `kBoardSolideS3.*` (often `constexpr`, and
  `leds.cpp` uses it to size a static buffer). `board.cpp:8-9` returns `kBoardSolideS3`
  unconditionally and is consumed in exactly one place (`selftest.cpp:156 batt.cells`).
  `library.json:18` compiles `+<device/>` for every build. **So `-DSOLIDE_BOARD`
  currently changes nothing; a Freenove build would clock the ILI9341 on Solide pins and
  bit-bang 45 WS2812 pixels on GPIO 21.** Converting the drivers to a compile-time-selected
  active board is the real Phase 2 work, not an afterthought.
- The `Board` struct (`board.h:16-38`) models only SPI-SD, raw-I2S INMP441 mic, I2S amp,
  and XPT2046-on-SPI touch, with **no capability flags** and no home for FT6336U (I2C +
  INT/RST + addr), ES8311 (I2C addr + amp enable), SDMMC (6 pins), or `hasRing`.
- Touch is abstracted to a `Gesture` ("nothing downstream learns touch exists"), but the
  **calibration** underneath is XPT2046-shaped: `touchCal` is raw 12-bit min/max
  (`touch_cal.h:24-27`, `test_console.cpp:927-934`, applied `main.cpp:2377-2388`, web
  `tchCal` `webui.cpp:503`). FT6336U reports pixel coordinates, so min/max cal is
  meaningless/harmful for it - only swap/invert flags apply.
- The ring is portable and reusable: `attn::Router -> ring::compose() -> ring::Plan ->
  Animator::frame(now, RGB* out, 45)`, then `hw::applyRingPlan()` pushes to `solide::leds`.
  Everything above `showFrame()` is board-agnostic.
- `NIMBUS_RING_LEDS` is a hard `#define 45` (`nimbus_config.h:27`) and is **advertised over
  BLE** (`ble_notifier.cpp:163`), so a 1-LED board would tell hosts it has 45.
- OTA is per-variant: the device requests one `NIMBUS_OTA_VARIANT` and the manifest maps
  variant->image. The signature covers `version\nvariant\nsha` only - it does **not** bind
  the image to a `SOLIDE_BOARD`. A wrong image in a variant slot is signable and installable.

## 2. Core design decisions (revised)

### D1 - Board = compile-time `SOLIDE_BOARD`; display-family stays `scrModel` (asymmetric, on purpose)
- **Board pinout is a compile-time identity.** Runtime board selection is incompatible with
  the driver design (constexpr pins size static buffers) and would let a user brick an
  all-in-one by mis-selecting a pin map. This is the right call, but be explicit about the
  trade it makes: `scrModel` exists so **one image serves two displays**; a compile-time
  board means the **Freenove is a separate firmware image and OTA variant**, not a
  universal image. Board (per-binary) and display-family (per-device) are orthogonal in
  intent but **asymmetric in mechanism**.
- **`scrModel` stays `"tft"`** for the Freenove (renderer + input family match). No new slug,
  so the `screenIsTft()` bool and its ~8 clamp/boot sites are untouched. Confirmed nothing
  at the `scrModel` gate hard-asserts XPT2046 or the 45-ring: self-test already branches on
  runtime probes (`testInput->touch::present()`, `testEpd->display_tft::taskAlive()`) and
  `fault::LED` already suppresses the ring push. **But** one `scrModel` value now covers two
  hardware identities, so everything keyed on "tft == solide-TFT specifics" must be
  enumerated and fixed (see D3, D4, and the hardening section) rather than found on the bench.
- Web `scrModel` selector is hidden/locked on the Freenove, **driven by board id** (not by
  `scrModel`, which would be circular).

### D2 - The real solide-drivers work: parameterize the driver layer, then add drivers
1. Introduce a single compile-time active-board alias (e.g. `constexpr Board kActiveBoard =
   <selected by SOLIDE_BOARD>;` in `board.h`), keep it `constexpr` so static sizing holds,
   and convert all ~7 drivers off `kBoardSolideS3` to `kActiveBoard`. Prove it compiles for
   **both** `solide_s3` and `freenove_s3` with identical behavior on `solide_s3` (a no-op
   refactor for the existing board - guarded by the existing golden/HIL suites).
2. Grow the `Board` struct: add capability flags (`hasRing`, touch-kind resistive|capacitive,
   audio-kind rawI2S|codec, sd-kind spi|sdmmc) and the new peripheral fields (FT6336U i2c +
   int/rst + addr; ES8311 i2c addr + i2s + amp-enable; SDMMC 6 pins).
3. Extend each driver to honor the capability/kind, branching at compile time on
   `kActiveBoard`. New peripheral code (FT6336U reader, ES8311 init, SDMMC mount) lives
   behind those switches so it compiles-out on `solide_s3` (respecting the unconditional
   `library.json` srcFilter).
4. `board_freenove_s3.h` (pin table + capabilities) + `board.cpp` dispatch on `SOLIDE_BOARD`.
5. `solide::begin()`: bind the Freenove driver set; make the **boot-time onboard-LED clear**
   board-conditional - today `solide.cpp:23` unconditionally writes `RGB_BUILTIN` (GPIO 48),
   which is **SDMMC D2** on the Freenove.
- **Reuse, don't vendor:** hand-write the small FT6336U I2C reader and ES8311 register init
  (keep audio inline on `i2s_std`, no codec library that fights the inline path); use the
  Arduino core **`SD_MMC.h`** (built-in, same `fs::FS` surface) for SDMMC rather than
  hand-rolling ESP-IDF. A single **I2C bus owner** does `Wire.begin(16,15)` once - FT6336U
  and ES8311 must not each init the bus.

### D3 - No-ring boards render the ring on the panel (task item 2), corrected seam
- Add `board().hasRing` (false for Freenove).
- **Fallback trigger truth table:** draw the ring on the panel when `!hasRing` **OR**
  (`hasRing` AND (`fault::active(LED)` OR `led` self-test fail OR `!leds::taskAlive()`)).
  Note: on a no-ring board the `led` self-test is *healthy-but-absent* and will PASS, so
  `!hasRing` (compile-time) is the real trigger there; the deepened self-test is a separate
  feature for ring boards whose ring dies. (Web toggle to force on-screen; N/A on e-ink.)
- **The physical push must be REPLACED, not shadowed:** gate `applyRingPlan()`'s
  `solide::leds::showFrame(...)` on `hasRing` so a no-ring board never bit-bangs a 45-px
  frame onto GPIO 21. On the Freenove the single WS2812 (GPIO 42) is driven separately as a
  simple status pixel (or left dark) - it is not the ring.
- **New work the plan owns honestly:** a LED-index -> (x,y) panel mapping (the existing
  `layout()`/`arcCenter()` return segment spans / physical LED indices, *not* panel
  coordinates - that mapping must be authored). Snapshot `g_animBuf` rather than run a second
  Animator. Draw inline from `tickAnimation()`/`refreshRing()` (already loop-driven, no new
  task/timer - respects the no-concurrency invariant). Keep to a small dirty-region arc and
  **measure the per-tick ILI9341 blit cost at bring-up** (ring animates <=60 FPS; it competes
  with the 150 KB PSRAM double-buffer and panel-watchdog repaints) before wiring it into the
  Notifier loop.
- **BLE ring-size:** decide what a 1-LED board advertises (`ble_notifier.cpp:163` currently
  hardcodes `NIMBUS_RING_LEDS`=45). The on-screen ring renders the logical segment model, so
  advertise the logical ring capability consistently rather than a physical count of 1.

### D4 - Capacitive touch on a resistive-cal seam must be neutralized (task item 1 safety)
- The FT6336U driver interprets **only** swap/invert flags from `Cal`; it ignores the raw
  12-bit min/max entirely.
- On a capacitive board, hide/neutralize the resistive-cal affordances: the web `tchCal`
  field, the `TCAL minX,maxX,...` console command, and `tools/tcal_wizard.py` (add a touch-kind
  gate). Otherwise an owner running touch-cal corrupts their own coordinate map.

### D5 - Resolution-independent UI (task item 1, "highly valued") - staged, non-blocking
- **Phase A (board PR): ship 2.8" reusing the fixed 320x240 renderer + `golden_tft`
  unchanged.** Corrected rationale: goldens are the board-independent RGB565 *render output*;
  panel rotation/BGR is handled at the `display_tft` push (MADCTL), so a different touch
  controller or color order **cannot change a golden byte**. (Do not "helpfully" re-bless.)
- **Phase B (follow-on): parameterize `tft_render` by `{w,h}`**, deriving layout from the
  existing `theme.h` tokens instead of magic numbers, keeping 320x240 goldens byte-identical
  as the regression proof. A prism scan found only ~14 bare 3-digit literals in
  `tft_screens.cpp`, so this is smaller than v1 feared - but still kept out of the board PR so
  it can't threaten first light or the goldens. A code generator remains the rejected fallback.

## 3. Hardware-harm hardening (new section - the residual wrong-pinout vectors)
The web vector is closed by D1, but prism found three more ways to drive a board with the
wrong pin map. All must land with the board:
- **Provisioning firmware:** `provision`/`provision-uart` `extends s3` (`-DSOLIDE_BOARD=
  solide_s3`). Flashing it to a Freenove drives Solide pins on the all-in-one PCB. Add
  `provision-cyd`/`provision-cyd-uart`, and make provisioning **not** init peripherals it
  does not need.
- **`setup_device.py` board selection:** it hardcodes the flash env `esp32s3` and
  `display in {eink,tft}`, with no board->env map; a blank Freenove has no NVS to
  self-classify. Add an explicit **board argument** (the operator states the board, since HW
  can't self-identify pre-flash), mapping board -> {provision env, production env, OTA
  variant}. Keep the factory-MAC confirmation gate.
- **`tftFlip` per-board default:** `setup_device.py:73-81` force-sets `SETI tftFlip=1` for
  every `tft` board (a Solide-mount-specific 180deg MADCTL flip). The Freenove mount differs,
  so the correct flip default is board-specific and must be determined at bring-up - not
  inherited blindly (v1 would have shipped the UI possibly upside down).
- **OTA variant<->board binding:** add a build-time `static_assert`/`#error` that binds each
  `NIMBUS_OTA_VARIANT` to exactly one `SOLIDE_BOARD` so the two can never be compiled apart;
  give `test-cyd` its **own** OTA variant (not the shared `"test"`, else Solide and Freenove
  test units are mutually cross-flashable). Optional stretch: embed the board id in the image
  and refuse a manifest whose variant does not map to the running board.
- Standing release cost to accept: every release now builds+signs+ships **two** images
  (`esp32s3` + `cyd`); `tools/build_all.sh` and the manifest tooling carry both.

## 4. Work breakdown (resequenced for fastest safe first-light)

### Phase 0 - Unblock (no board writes)
0a. **Confirm the canonical, writable `solide-drivers` repo.** The local sibling tracks
   `ristllin/solide-drivers-archive` (tag v0.4.0) but `platformio.ini` pins
   `ristllin/solide-drivers`; both exist on GitHub and neither is archived. Resolve which is
   canonical before any driver work. **(Open question for the user.)**
0b. Fix worktree build resolution: `../solide-drivers` does not resolve from
   `.claude/worktrees/freenove-cyd-variant` (it points at `.claude/worktrees/solide-drivers`).
   Add a local symlink or run host/symlink builds from the main checkout.
0c. Read the Freenove **schematic**; fill the display-SPI row; strapping-pin check.

### Phase 1 - solide-drivers seam spike (the real prerequisite)
1. Active-board alias + convert all ~7 drivers off `kBoardSolideS3`; prove `solide_s3`
   behavior unchanged (existing goldens/HIL) and `freenove_s3` compiles. Grow the `Board`
   struct with capabilities + new peripheral fields. Develop on the **symlink** the whole time.

### Phase 2 - solide-drivers: the three new drivers + board profile
2. FT6336U touch (I2C, flags-only cal). 3. ES8311 audio (inline, single I2C owner, no task).
4. SDMMC via `SD_MMC.h`. 5. `board_freenove_s3.h` + `board.cpp` dispatch + `solide::begin()`
   Freenove branch + board-conditional onboard-LED clear. 6. `leds` count=1 on GPIO 42.

### Phase 3 - Nimbus firmware: first light
7. Build envs: `esp32s3-cyd`, `test-cyd` (own OTA variant), `provision-cyd(-uart)`; OTA
   `static_assert` binding variant<->board. 8. `SD.h`->SDMMC path selection in the firmware
   storage seam. 9. STATUS/`/api/state` gain `board=` **appended at the end of the STATUS
   line** (after `uptime=`, not between `heap=` and `scr=`). 10. `setup_device.py` board arg
   + per-board `tftFlip` default; lock web `scrModel` on Freenove by board id. 11. Neutralize
   resistive touch-cal on capacitive (D4).
   **Milestone: board boots, screen lights, touch works, one Wi-Fi orchestrator turn.**

### Phase 4 - No-ring on-screen ring (task item 2)
12. `hasRing` plumbing; replace (not shadow) the physical push; on-screen ring widget +
   LED->panel mapping + `g_animBuf` snapshot; fallback truth table + deepened `led` self-test
   for ring boards; web toggle; BLE ring-size decision; new golden(s) for the widget.

### Phase 5 - Docs (task item 4; happy-path, 1-item BOM)
Canonical `docs/` only, then regenerate mirrors (see the docs map in review notes):
13. `docs/hardware.md` selector row; **new** `docs/hardware/all-in-one-cyd.md` (pinout +
   happy-path quick start: buy one module, plug USB-C, flash, done); `docs/hardware/bom.md`
   `## Configuration C` **1-item** BOM; `docs/quick-start/what-you-need.md` third column;
   `docs/modes-and-signals.md` on-screen-ring behavior + control (parity ledger);
   `docs/reference/config-and-nvs.md` note (board compile-time, `scrModel` fixed tft).
14. Grep-fix "one of two / two configurations" repo-wide (AGENTS.md list).
15. Website: add pages to `migrate-docs.mjs` `PAGES` **and** `sidebars.js`; `npm run build`
   clean. Re-run `tools/gen_docs_pack.py` (hardware.md is curated); commit `docs_pack_data.h`.

### Phase 6 - Tests (task item 3; whole battery, new pinout)
16. Host: reuse `test_tft_render` + `golden_tft` for 2.8"; new golden(s) only for the
   on-screen ring; host tests for board-capability selection, the ring-fallback truth table,
   FT6336U coord mapping, touch-kind cal gating.
17. Bring-up sketches, reordered to first-light: `tftbringup-cyd` (ILI9341 + BGR/backlight)
   -> `tfttouch-cyd` (FT6336U axis/mirror) -> full boot + turn; then `beep-cyd`/`mictest-cyd`
   (ES8311), `sdmmc-cyd`, `battery-cyd`, `ledpixel-cyd`. Prefer reusing existing sketches
   under `-DSOLIDE_BOARD=freenove_s3` where pins now come from `board()` data; add only the
   genuinely-new-driver envs.
18. HIL: add `board=` + a `_require_board("freenove_s3")` loud-skip helper. New
   **`test_l27_freenove.py`** (l24 is taken by `test_l24_results.py`) scoped to Freenove
   deltas only - panel+touch (FT6336U), ES8311 loopback, SDMMC, battery, on-screen ring -
   reusing board-agnostic L4/L6/L23 via the skip rather than re-implementing. Keep
   `test_l21`'s e-ink non-regression green.

### Phase 7 - Manual test script (task item 3 tail)
19. Written human checklist delivered at the end: screen/touch/audio/SD/Wi-Fi/battery/
   on-screen-ring/orchestrator-turn/notifier-session, plus the `tftFlip` orientation check.

### Phase 8 - Land the cross-repo change
20. Tag solide-drivers `v0.5.0` and flip `platformio.ini` from the symlink back to the pinned
   tag **as the final commit** - the tag and the pin bump must land together, or a fresh clone
   (and the `esp32s3-cyd` env) won't build.

## 5. Verification battery (real markers pasted in the PR)
`pio test -e native` (0 failures) | `pio run -e esp32s3` + `esp32s3-cyd` + `test` + `test-cyd`
(SUCCESS) | `cd website && npm run build` ([SUCCESS] Generated static files) | `pre-commit
run --all-files` (Passed/Skipped, incl. no-em-dash + lizard-on-new-code). Device behavior
proven only via HIL + the manual script.

## 6. Open questions for the user
1. **Canonical solide-drivers repo:** work in `solide-drivers` (the pinned name) or
   `solide-drivers-archive` (what the local checkout tracks)? Which is writable/taggable?
2. **PR staging:** one large PR, or land it as a sequence (board first-light -> on-screen
   ring -> resolution parameterization)? Recommendation: staged.
3. **On-screen ring scope:** confirmed as task item 2 - kept, but sequenced *after* first
   light (Phase 4), not gating the board working. OK?
4. **Other panel sizes (3.5"/4.0"):** parameterize now (Phase B, follow-on) but only bring up
   a physical 2.8"? Or acquire a larger panel to prove Phase B on real hardware?

## Appendix - what the prism review changed (all CONFIRMED against code unless noted)
- **Driver layer is not board-parameterized** (peer-review #1, simplify #1): `board.h`'s
  "zero driver changes" is false; ~7 drivers hardcode `kBoardSolideS3`. -> new Phase 1.
- **Flash/provision entry path assumes one board** (roast #2/#3/#4): no board->env map,
  `provision` runs Solide pins, `tftFlip=1` hardcoded. -> Section 3 + Phase 3.
- **Capacitive touch on resistive cal** (roast #5, peer #3): FT6336U + XPT2046 min/max cal.
  -> D4.
- **OTA binds variant string, not board** (security #1/#2): wrong image is signable/installable;
  `test-cyd` must not share `"test"`. -> Section 3.
- **On-screen ring seam**: must replace not shadow the physical push; LED->panel mapping is
  net-new; measure blit cadence (peer #4, roast #8). -> D3.
- **Boot GPIO48 clear == SDMMC D2** (peer #5): make board-conditional. -> D2.5.
- **`NIMBUS_RING_LEDS`=45 advertised over BLE** (roast #8). -> D3.
- **Shared I2C single-owner init** (roast #9). -> D2.
- **SDMMC via Arduino `SD_MMC.h`, hand-write FT6336U/ES8311, no vendored codec** (simplify
  #5/#6). -> D2.
- **Golden-reuse rationale corrected** (roast #6): goldens are board-independent render output.
  -> D5.
- **Worktree can't resolve `../solide-drivers`; tag v0.5.0 last; test_l24->l27; trim HIL;
   fixed wrong `src/device/solide.cpp` citation (call is `main.cpp:2359`)** (simplify
  #7/#8/#11, roast #10/#12). -> Phases 0/6/8.
- **Kept as sound:** `scrModel=tft` (no new slug), compile-time board for HW-harm safety,
  reuse `golden_tft` for 2.8", inline audio (no-concurrency), staged Phase B.

## Next TODOs (deferred, tracked)
- **Multi-resolution (IMPORTANT, user-flagged):** parameterize the TFT renderer by
  `{w,h}` (derive layout from the `theme.h` tokens instead of the fixed 320x240
  constants) so the 3.5"/4.0" Freenove panels work. The 2.8" is 320x240 and needs
  none of this; a larger physical panel is needed to bless new goldens. This is the
  "highly valued" item from the original task, kept out of the board-bring-up scope
  so it can't threaten first light or the goldens.
- **On-device bench validation (owner):** ES8311 audio (the shared-channel mic RX
  slot mono/stereo is the likely gotcha - see the code note + MANUAL step 2), SDMMC
  card, battery voltage, the ring SMOOTHNESS on real hardware, and touch orientation
  in the live UI. All in `tests/hil/MANUAL_freenove_cyd.md`.
- **Landing:** tag `solide-drivers` and flip `platformio.ini` off the dev symlink
  (Phase 8) - the two repos land together.
