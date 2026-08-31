# AGENTS.md - Nimbus

The operating manual for anyone - human or AI - working on this firmware. It is
deliberately heavy: the conventions here were paid for in bricked boards, burned
hours, and shipped bugs, and keeping to them is what lets a contributor land a
change without re-learning those lessons. Read it before you touch code, and keep
to it even when a shortcut looks safe.

If a rule here ever seems to contradict what the code does, trust the code and
flag the doc - but do not quietly work around a rule marked **frozen** or ⛔.

---

## 1. What Nimbus is

Nimbus is product firmware for a battery-capable desk device built on an
**ESP32-S3** (the Solide S3 board, an ESP32-S3-DevKitC-1 N16R8). The hardware is
a 45-LED WS2812B ring, an I²S microphone and speaker, and a **2.8" ILI9341 color
touchscreen** (240×320) for display and input. A second supported build targets
the Freenove all-in-one touch board. The `screenModel` NVS key is retained (frozen
key), with `tft` as the value; the legacy `eink` value now boots an unsupported-
display notice on the color panel.

The device has **two operating modes** over one shared attention router. In
**Notifier** mode it is an ambient status light for AI coding sessions: a host-side
broker speaks the `nimbus-notify` wire protocol to the device over encrypted
Bluetooth, and each active session gets a ring segment whose color and animation
say what that session is doing. In **Orchestrator** mode it is a self-hosted AI
assistant - a provider-hosted LLM agent you reach over Telegram or by voice, with
long-term memory on an SD card. Three battery modes (**Dark / Balanced / Full**)
trade ambient brightness and telemetry for runtime. Bring your own provider API key
(Mistral, OpenAI, or Anthropic).

---

## 2. Repository map

| Path | What lives here |
|---|---|
| `lib/core/` | Portable logic - **no Arduino**, host-tested. Protocol codec, profiles, power policy, orchestrator memory engines, the turn contract, RBAC. Lowest layer. |
| `lib/harness/` | The agent policy/orchestration layer (tool policy, skills, attribution). Host-testable. |
| `src/` | The firmware itself: `hw/` (Arduino/board glue), `modes/`, `agent/`, `net/` (web surface), and `main.cpp` (wiring + loop). |
| `include/` | Firmware headers, including the fragment-split web UI (`include/web/ui_*.h`). |
| `test/` | **Unity C++ host suites**, one directory per module, plus golden framebuffers. Run under the `native` build. |
| `tests/hil/` | **Python hardware-in-the-loop (HIL) suite** - pytest, drives a real board over serial/HTTP. |
| `tools/` | Installer, recovery, calibration, code generators, and host test harnesses. |
| `docs/` | The canonical documentation tree (the source of truth for the docs site). |
| `website/` | Docusaurus site that publishes `docs/` to GitHub Pages. |
| `hardware/` | Physical build artifacts - PCB manufacturing files (`fab/`) and space for assembly notes/BOM/CAD. (The *published* hardware guides live under `docs/hardware/`.) |
| `evals/` | Orchestrator behavior evaluations and reports. |
| `assets/` | Logo and brand art (mostly generated - see §5). |

The published hardware **documentation** (pinouts, BOM, build guides) lives under
`docs/hardware/`. The physical **artifacts** - PCB manufacturing files and space for
assembly notes/BOM/CAD - live in the top-level `hardware/` directory.

The `test/` vs `tests/` split is real and load-bearing: `test/` is the C++ Unity
host suite (fast, no hardware), `tests/hil/` is the Python HIL suite (needs a
board). The `test` **build environment** is a third thing again - the production
firmware plus a serial command console the HIL suite drives.

**Capability-matrix rule (CUM-94):** any new provider or capability MUST land
with either an entry in the capability matrix test
(`test/test_capabilities_matrix/`, re-bless the generated
`docs/reference/capabilities-matrix.md`) or an explicit not-supported entry -
never silently absent. The cloud repo enforces the same rule in its
`capability-matrix.ts` (see cumulo-nimbus/CLAUDE.md).

---

## 3. Build & test - the verification battery

Board support lives in the public repo
[solide-drivers](https://github.com/ristllin/solide-drivers), consumed as a
PlatformIO library. **Device builds fetch it automatically** from the pinned tag
in `platformio.ini` - a fresh clone builds `esp32s3`/`test` with nothing else.
The **host test suite** (`pio test -e native`) additionally needs the driver
headers on its include path, which expects a sibling checkout:

```bash
git clone https://github.com/ristllin/solide-drivers.git ../solide-drivers
git -C ../solide-drivers checkout v0.5.1   # the tag the firmware pins
```

Driver developers can point the device builds at that same sibling by swapping
the commented `symlink://../solide-drivers` line in `platformio.ini`.

Run the full battery below before every commit and **paste the real output** into
your PR - never assert green from memory. Each command has a positive marker; if
you don't see it, it did not pass.

| Command | Passes when you see |
|---|---|
| `pio test -e native` | the summary line `... Tests ... Failures ...` with **0 failures** |
| `pio run -e esp32s3` | `SUCCESS` (production build links) |
| `pio run -e test` | `SUCCESS` (test-console build links) |
| `cd website && npm run build` | `[SUCCESS] Generated static files` |
| `pre-commit run --all-files` | every hook `Passed` or `Skipped` |

Notes:

- **`pio test -e native`** is the host unit + golden suite and must stay at 100%.
- **`npm run build`** doubles as the dead-link check: `onBrokenLinks` is set to
  throw, so a build failure there means a doc link points at something that moved
  or was removed. A clean build proves the doc graph is intact.
- **`pre-commit`**: install once with `pip install pre-commit && pre-commit
  install`. CI runs the same hooks. A complexity gate (lizard) applies to **new**
  code only; existing offenders are baselined in a whitelist that only ever
  shrinks - don't add to it without discussion.
- The build matrix beyond the two shown (`notifierdbg`, `bttest`, the
  `tft*`/`beep`/`provision` bring-up sketches) is compiled by
  `tools/build_all.sh`.

**"It compiles" is NOT "it works."** A clean build and a boot in the other mode
prove nothing about the touch input, the radio, the ring, or a live turn. Device behavior
is verified one of two ways only: the HIL suite (`tests/hil/`, needs a board), or a
human running an explicit manual step and confirming the result. Anything
user-visible must be asserted through a real seam (`RENDER?`, `STATUS`, an echoed
reply) or handed off as a manual step - never claimed from a green host build. A
render in particular is confirmed by a human looking at the actual screen: a passing
snapshot only proves the bytes match what was captured, which is worthless when the
capture itself was never inspected (the Safety tab checkboxes shipped centered under a
green snapshot, and the white screen was host-green while the panel was blank on glass).

```bash
python3 -m pytest tests/hil -m "hil and not manual" --allow-hardware   # device suite
python3 -m pytest tests/hil -m "net and not manual" --allow-hardware   # LAN suite
```

HIL collection must stay clean with no board attached
(`pytest tests/hil --collect-only`); hardware tests are gated behind
`--allow-hardware`. Secrets (provider keys, Wi-Fi, a dedicated Telegram test bot)
are supplied at runtime through environment variables, **never committed**. A "no key
/ not configured" result is a claim about the environment, not a fact: re-run the check
in the correct working directory and env before you believe or report it - a worktree
that never inherited the gitignored `.env` will lie that keys are missing.

**Test the class, not the instance - the rules the recurring bugs paid for.** Each of
these is here because a bug came back after a fix that pinned only its own case; a green
suite is not enough if it never encoded the class.

- **Test the invariant, not the instance.** When a bug is the Nth recurrence of a
  class, the fix adds a property test over ALL cases (every enum value, every catalog
  entry), not one more point test, and a new case with no guard must FAIL. The stuck
  ring recurred five times (CUM-11 -> 134 -> 221) because every fix hardened one
  `(ring::Status, task)` pair and none asserted the class rule "no lit arc outlives its
  job"; the provider list was hardcoded and drifted twice the same way.
- **Assert the absence of thrash, not just the final value.** A state-machine test
  counts transitions and fails on oscillation. The panel flip -> `healthy()` fix
  (CUM-188) passed a final-state test while quietly repainting every cycle, which was
  masking a white screen underneath.
- **Fresh device is a first-class test state.** At least one host or HIL leg runs on
  default or absent NVS: the first boot after a new version, the exact state the owner
  QAs. Touch shipped mirrored out of the box (CUM-203) because every touch test ran on
  an already-calibrated unit, never a fresh pre-cal panel.
- **A loosened mask, threshold, or timeout ships a counter-test in the dimension it
  now ignores** (see §4, Golden files - it is the same discipline).
- **Never mask a gate's exit status.** No `| tail` or other pipe that swallows a
  non-zero exit on a battery command, and no suite quietly excluded from the default
  sweep. A masked `lint | tail` and an e2e directory dropped from the sweep each hid
  real failures behind a green run.

---

## 4. Frozen invariants

These are the hard rules. Breaking one usually compiles, often passes the tests
that exist, and fails on real hardware or in the field. Treat them as load-bearing.

### ⛔ No on-device concurrency

The turn / sub-agent path is **single-task, single-TLS-slot, and fully
serialized by design.** Do **not** add a dispatch or worker task, a second
concurrent TLS session, or any on-device parallelism.

This is hardware-proven, not a preference. The chip has very little
largest-contiguous internal SRAM free, and a TLS-capable task stack cannot live in
PSRAM. A second concurrent TLS work slot measurably collapsed the head's contiguous
internal heap and failed real turns; a second poll/worker task overflowed its stack
and panicked; contending inline work starved the watchdog and reset the device.
Providers parallelize **remotely** - the device still polls them one at a time - and
deep fan-out scales **sequentially over waves**, never concurrently. "Run providers
closer together" means reduce idle poll latency, nothing more.

### Byte-locked wire codecs

The `nimbus-notify` frame codec and the on-device QR encoder are **byte-locked** to
external references through generated test vectors. Never hand-tune a codec to make
a vector pass. If the protocol genuinely changes, regenerate the vectors with the
generator (§5) and review the diff - the vectors are the contract, the codec obeys
them.

### Golden files

UI screens are pinned as **golden framebuffers** (`test/golden_tft/*.bin` for the
color panel), and several protocol/prompt surfaces have golden text (`test/golden/`).
A golden may only be re-blessed by running the suite with
`GOLDEN_UPDATE=1` **and visually confirming** the new render is correct. A golden
that changed because you "just ran the updater" and didn't look is a silent
regression.

The same discipline governs any **compare mask, threshold, or timeout you loosen**: it
ships with a counter-test proving the fault it now ignores is still caught. Widening the
panel-health compare mask (`0xFE` -> `0x3E`, to quiet a repaint thrash) silently dropped
MY/MX fault detection and reintroduced the white screen (CUM-231); the suite stayed
green because no test asserted a fault in the newly-ignored bits.

### Generated files - edit the generator, never the output

Many tracked files are build artifacts. Editing the output by hand is always wrong:
the next generator run silently reverts you, or worse, ships a hand-edit that no
longer matches its source. Change the source, re-run the generator, and commit both
in the same change.

| Generator | Output(s) |
|---|---|
| `tools/gen_docs_pack.py` | `lib/core/include/nimbus/docs_pack_data.h` (docs pack riding the firmware image) |
| `tools/sounds/*` (`gen_tones.py`, `embed_basic.py`) | `src/sfx/sfx_basic_data.h` |
| `tools/sounds/gen_sfx_map.py` | `docs/sfx-map.md` |
| `tools/logo/gen_logo.py` | `assets/logo*.svg`, the website logo / favicon / social-card, `include/web/ui_logo.h` |
| `tools/gen_nsn_vectors.py` | `tools/nsn_vectors.json`, `test/test_proto/nsn_vectors.h` |
| `tools/gen_qr_vectors.py` | `test/test_qr/qr_vectors.h` |
| `website/scripts/migrate-docs.mjs` | `website/docs/{quick-start,guides,reference,contributing}/**` (regenerated on deploy; **not committed**) |
| `tools/webui_concat_check.py` | `tools/webui_page.snapshot` (byte capture of the assembled web UI) |
| `tools/make_manifest.py` | the signed OTA release manifest (golden-tested against the on-device signer) |

### Frozen strings and wire numbers

- **NVS keys and machine slugs are frozen.** Persisted setting keys (e.g.
  `screenModel`, `sfxLvlN`) and machine-facing enum slugs (`balanced`, `light`,
  `terran`, …) are stored on real devices and read by tools; renaming one breaks
  every device already in the field. The user-facing *display* text is a separate,
  changeable thing (see §6) - the machine key underneath is not.
- **`ScreenId` and `ConnRow` wire numbers are positionally mirrored by the HIL
  tests.** Reordering or inserting an entry shifts every number below it, and a HIL
  test will then assert the wrong screen or menu row *while still passing*. Do not
  reorder or renumber these enums.

---

## 5. Code layout & layering

The dependency direction is strict and one-way:

```
main.cpp  (wiring + loop)
   └─ src/modes · src/agent · src/net   (mode logic, agent, web/LAN surface)
        └─ src/hw                        (Arduino / board glue)
             └─ lib/core                 (portable logic - NO Arduino, host-tested)
```

**A lower layer never includes an upper one.** `lib/core` is portable C++ with no
Arduino dependency, which is exactly what lets it be unit-tested on the host; the
moment it reaches up into `src/`, that property is gone. New portable logic belongs
in `lib/core` with a host test; the device seam that wires it to hardware belongs in
`src/`.

---

## 6. Copy style guide - every user-facing word

Applies to web UI copy, device screens and menus, Telegram replies, OTA notices,
human-visible auth errors, the README, `docs/`, and the website. Exempt:
model-facing prompt strings, code identifiers, machine keys, and log lines.

**Voice.** Calm, direct, benefit-first - say what happens for the person, never how
it is engineered. Rationale belongs in code comments and internal docs, not in a
label or hint ("Volume", not "Volume - the amp overdrives a small speaker at full
scale"). System copy never says I/me/my; the device is "it". (The one exception:
the Orchestrator assistant speaks first person *in conversation* - that is a
persona, not the hardware.) No exclamation-mark alarm, no ALL-CAPS shouting, no
jokes in error paths. An error states what happened, then the one next step:
"Couldn't reach the network. Check Wi-Fi and try again."

**Capitalization.** Title Case for tabs, nav, buttons, and page titles. Sentence
case for group headings, field labels, toggles, hints, and errors. When docs quote
the UI, quote its exact case.

**Spelling.** US English everywhere: color, behavior, customize.

**Punctuation.** Never use an em dash (U+2014) anywhere in this project: not in
docs, comments, UI strings, or commit messages. Use a plain hyphen, a comma, a
colon, parentheses, or restructure the sentence. A pre-commit hook rejects any
staged em dash (the one exception is recorded external data under
`test/support/fixtures/`, which is byte-frozen).

**Surface constraints.** The device screen and serial are printable ASCII only, ~48 chars per
line, no emoji. Telegram may use UTF-8 and should read well aloud. Web toasts are
sentence case, verb-led, ≤5 words; failures name the next step.

**Canonical spellings** (grep for the losers before shipping):

| Use | Not |
|---|---|
| Wi-Fi | WiFi, wifi (in user copy; identifiers exempt) |
| Bluetooth | BLE (in user copy; BLE is fine internally) |
| battery mode: Dark / Balanced / Full | power profile, Battery Saver, Desk, Saver |
| ring level: Dark / Calm / Full | light level |
| sound level: Off / Low / Medium / High | none/light/medium/heavy, mute |
| Notifier, Orchestrator (capitalized) | notifier / orchestrator in copy |
| routine (user noun) | loop (in user copy) |
| device sign-in code; Cloud link code; Sign-in QR | access token, webtok, Config QR, "the QR" |
| restart | reboot (in user copy) |
| erase | wipe, nuke |
| the display / the screen (user copy) | the panel, the TFT (in user copy) |
| sound theme: Pulse | voice pack, Terran / Protoss / Zerg |
| provider, Providers | vendor, backend (in user copy) |
| routing, Router | dispatch, egress (in labels) |
| pair, Pairing | claim, adopt, link (in labels) |
| usage, Usage | consumption, activity |

The last block is the cross-surface vocabulary from the IA naming proposal
(section 4.4), binding on the cloud surfaces too. On the portal and admin, also:
Credits (not wallet or a balance-as-page-name), Plans (not Subscriptions in nav;
fine in body copy and code), Virtual Nimbus / Instances (not virtual device / VM),
Cloud link code (not 8-character code or claim code).

---

## 7. Docs follow every commit

Stale docs are a bug. Land the doc change in the **same** commit as the code.

- Renamed or moved anything user-visible (a tab, label, menu row, command, flow)?
  Grep for the old wording across `README.md docs/ website/docs/ include/web/ src/
  tools/` and fix every hit.
- Edited a canonical doc under `docs/`? If it is a published page, run
  `node website/scripts/migrate-docs.mjs` (regenerates the site mirror - which is
  **not committed**, so this is for your local build) and confirm
  `cd website && npm run build` is clean. If the doc is one the device ships (the
  curated list in `tools/gen_docs_pack.py`), re-run `python3 tools/gen_docs_pack.py`
  and commit the regenerated `docs_pack_data.h` in the same change.
- Edit only the canonical Markdown under `docs/`. The `website/docs/**` guides and
  reference pages are generated; the hand-written site pages are
  `website/docs/intro.md` and `website/docs/getting-started/`.
- **Parity ledger.** Every user setting has one canonical home and a declared
  presence on the other surface (device menu vs web UI). Adding or renaming a setting
  updates that ledger (in `docs/`) in the same commit - a setting that exists in code
  but not the ledger is a UI that lies while its tests pass.

Before tagging a release, re-run `cd website && npm run build` and read the
broken-link warnings, then re-grep every label renamed since the last tag.

---

## 8. Contributing workflow - don't cut corners

The point of this section is simple: **a change is not done because it compiles.**
It is done when it is verified, clean, documented, and reviewed.

1. **Work on a branch or a worktree, never on `main` directly.** If the tree is
   dirty or someone else is mid-change, use a separate worktree so you never commit
   over shared work.
2. **Stage with explicit paths.** Commit with pathspec (`git add -- <paths>`), not
   `git add -A`; you should know exactly what is in every commit. Never commit
   generated site output, local secrets, or run artifacts (the `.gitignore` keeps
   these out - keep it that way).
3. **Run the full battery before every commit** (§3) and paste the real marker
   lines. A faked-green report wastes a reviewer's trust and a maintainer's
   hardware time.
4. **Keep the tree clean.** No repo-wide reformatting; formatting-only diffs of code
   you didn't otherwise change will be declined. No dead code, no commented-out
   blocks left behind.
5. **Docs ride the change** (§7).
6. **Review your own diff before asking anyone else to.** A recommended, genuinely
   useful step: run a multi-lens review over your diff - roast (find the weak
   spots), simplify (lower complexity, flatten nesting), peer-review (a second,
   ideally different, model's critique), and a security pass. If you have the
   `prism` skill installed, `/prism` does exactly this; otherwise apply the four
   lenses by hand. Fix the confirmed findings, then commit. Do this review in-session
   and act on it before you merge: an after-the-fact or background review is a
   supplement, not the gate, because an in-session adversarial re-run has caught bugs a
   background review had already rationalized away.
7. **Hardware claims need hardware.** If your change touches device behavior and you
   have no board, say so in the PR - a maintainer runs the HIL suite. Don't dress a
   host-only result as a device result.
8. **A known gap becomes a tracked issue, not a comment aside.** If your work leaves
   something not true end-to-end - a DECISION note that says "X isn't wired yet" - file
   it as a blocking issue, not a line buried in a PR comment. A deferred head-host key
   gap noted as an aside (CUM-201) shipped and re-surfaced as a dead end for the
   flagship one-key path (CUM-242).

Licensing, the Developer Certificate of Origin, and first-contribution ideas are in
[CONTRIBUTING.md](CONTRIBUTING.md). Security issues go through
[SECURITY.md](SECURITY.md), never a public issue. Be kind - see
[CODE_OF_CONDUCT.md](CODE_OF_CONDUCT.md).

---

## 9. Where to read more

- **[docs/architecture.md](docs/architecture.md)** - the system, layer by layer.
- **[docs/development.md](docs/development.md)** - build environments, the test
  ladder, the golden-test flow, and the hard-won constraints in depth.
- **[docs/modes-and-signals.md](docs/modes-and-signals.md)** - every user-facing
  knob and what it changes.
- **[docs/hardware.md](docs/hardware.md)** - pinout, wiring, and first-flash
  guidance.
- **[docs/orchestrator-world.md](docs/orchestrator-world.md)** - the agent's memory
  and control surface.
- **[docs/security.md](docs/security.md)** - the auth model and open items.
- The full published set lives at **https://docs.cumulo-nimbus.ai/**.
