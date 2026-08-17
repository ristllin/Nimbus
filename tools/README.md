# tools/ - quick index

One line per tool, grouped by audience. This is the fast map; the full catalog
(what each does in depth, plus every console and Telegram command) is
[`docs/tools-and-commands.md`](../docs/tools-and-commands.md).

Run host scripts with `python3` from the repository root unless noted. Most need
only the standard library.

**⚠ Load-bearing** tools are marked below - breaking one breaks CI, the build, or
a release. See the [maintenance contract](AGENTS.md) before adding or moving a
tool.

## End-user device tools

Everything you need to install, recover, and manage a device you own.

| Tool | One-liner |
|---|---|
| `setup_device.py` | Guarded firmware installer over the UART port (MAC-confirmed; seeds display + mode on a fresh board). |
| `usb_reset.py` | Programmatic unbrick - a libusb bus reset, equivalent to replugging a wedged USB-serial port. |
| `tcal_wizard.py` | Touch-calibration wizard for the TFT variant (four-corner press → axis/orientation flags via `TCAL`). |
| `connectors_setup.py` | Push connector credentials from the root `.env` to a board via the token-gated API (`--list` to inspect). |
| `backup_device.py` | Pull a board's user data (files, vectors, scratchpad, state) over the web API into a dated folder. Read-only; never exports secrets. |
| `ring_check.py` | Quick LED-ring smoke test over serial (`LEDTEST` acknowledgement). |

## Generators (edit the generator, re-run, commit the output)

Every tool here owns a committed output; the generator is the source of truth.
The generator→output contracts are enumerated in the root
[`AGENTS.md`](../AGENTS.md) "Docs follow every commit" section - never hand-edit
a generated file.

| Tool | Generates |
|---|---|
| **⚠ `gen_docs_pack.py`** | `lib/core/include/nimbus/docs_pack_data.h` - the on-device docs pack (ships in firmware.bin). |
| `gen_nsn_vectors.py` | `tools/nsn_vectors.json` + embedded header - nsn codec test vectors, byte-locked to `nsnotify`. |
| `gen_qr_vectors.py` | QR known-answer vectors from Project Nayuki's `qrcodegen` (needs `pip install qrcodegen`). |
| `sounds/` | The sound-pack pipeline → `dist/` + manifest + `src/sfx/sfx_basic_data.h` + `docs/sfx-map.md` (see its README). |
| `logo/gen_logo.py` | The Nimbus logo and every derived artifact (`assets/logo*.svg`, website logo/favicon/social card, `include/web/ui_logo.h`). |

## CI gates (run in `.github/workflows/checks.yml`)

| Tool | ⚠ Breaks if it fails |
|---|---|
| **⚠ `check_param_consumers.py`** | Fails the build if a settings parameter has no functional consumer (a knob that changes nothing). |
| **⚠ `check_status_doc.py`** | Fails when `docs/notifier-status-language.md` drifts from `status_style.cpp`. |
| **⚠ `webui_concat_check.py`** | Asserts the `include/web/ui_*.h` fragments re-concatenate into a well-formed page (byte-compares the blessed snapshot). |

## Build & release

| Tool | Role |
|---|---|
| **⚠ `git_version.py`** | PlatformIO `extra_scripts` - injects `git describe` as `NIMBUS_FW_BUILD`. Runs on **every** build (platformio.ini). |
| **⚠ `make_manifest.py`** | Signs the OTA release manifest (release.yml). Message byte-identical to `nimbus::ota::buildSigMessage()`. |
| **⚠ `release/make_webflash_manifest.py`** | Builds the web-flasher manifest (release.yml). |
| **⚠ `release/check_known_patterns.sh`** | Pre-commit leak gate (private `ops/leak_patterns.txt`; warns-and-passes for public contributors). |
| `release/seed_public_root.sh` | Maintainer gate - builds + verifies the public-repo root cut. Needs the private `ops/leak_patterns.txt`; refuses to run without it. |
| `release/lizard_whitelist.txt` | Baseline for the pre-commit lizard complexity gate. |
| `build_all.sh` | Compile-only gate: builds every PlatformIO environment and flag path. |

## Dev / contributor

| Tool | One-liner |
|---|---|
| `golden.py` | Render/diff e-ink golden framebuffers (`test/golden/*.bin`) to PNG. |
| `tftpreview.py` | Color counterpart for the TFT golden suite - `contact` sheet + tap-`regions` linter. |
| `nsn_send.py` | End-to-end broker→Bluetooth→device validation (needs `bleak` + macOS BT entitlement). |
| `harness-lab/` | Runs the real agent harness on the Mac instead of the device (see its README). |
| `test_setup_device.py` | Unit tests for `setup_device.py`. |
| `test_tcal_wizard.py` | Unit tests for `tcal_wizard.py`. |

Bench-only and board-specific scripts (a live connector-QA harness, the quality
benchmark, panic-capture, the connector-shape grid) live in the private `ops/`
tree, not this public repo. The **Battery Lab** is its own project at
[ristllin/nimbus-battery-lab](https://github.com/ristllin/nimbus-battery-lab);
it drives the firmware's `DRAIN`/`STORAGE` commands, which remain here.
