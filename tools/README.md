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
| `bench_guard.py` | The forbidden-device guard every port-touching tool asks first: lists the USB serial nodes and refuses the off-limits boards by MAC, reading the identity from the OS listing without opening the port (`list`, `check-port`, `check-mac`, `port-for-mac`). |
| `setup_device.py` | Guarded firmware installer over USB: finds the board by its USB descriptor, installs the image for its board and panel size, refuses a wrong-variant flash, never erases NVS. Re-runs itself under PlatformIO's Python when the launching `python3` has no pyserial. |
| `usb_reset.py` | Programmatic unbrick - a libusb bus reset, equivalent to replugging a wedged USB-serial port. macOS and Linux; needs pyusb (`pipx run tools/usb_reset.py` brings it) and libusb. |
| `tcal_wizard.py` | Touch-calibration wizard for the TFT variant (four-corner press → axis/orientation flags via `TCAL`). |
| `connectors_setup.py` | Push connector credentials from the root `.env` to a board via the token-gated API (`--list` to inspect; `--ip`/`--token` override the `.env` board). |
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
| `gen_turn_shape_fixtures.py` | Shared turn-shape fixtures from episodic exports: the conversation shape only, synthetic text (`--verify` checks it). |
| `sounds/` | The sound-pack pipeline → `dist/` + manifest + `src/sfx/sfx_basic_data.h` + `docs/sfx-map.md` (see its README). |
| `logo/gen_logo.py` | The Nimbus logo and every derived artifact (`assets/logo*.svg`, website logo/favicon/social card, `include/web/ui_logo.h`). |

## CI gates (run in `.github/workflows/checks.yml`)

| Tool | ⚠ Breaks if it fails |
|---|---|
| **⚠ `check_param_consumers.py`** | Fails the build if a settings parameter has no functional consumer (a knob that changes nothing). |
| **⚠ `check_status_doc.py`** | Fails when `docs/notifier-status-language.md` drifts from `status_style.cpp`. |
| **⚠ `webui_concat_check.py`** | Asserts the `include/web/ui_*.h` fragments re-concatenate into a well-formed page (byte-compares the blessed snapshot). |
| `model_id_lint.py` | Fails when a compiled-in model id is missing from the recorded provider model lists or has no built-in price (allowlist: `model_id_allowlist.json`). |

## Build & release

| Tool | Role |
|---|---|
| **⚠ `git_version.py`** | PlatformIO `extra_scripts` - injects `git describe` as `NIMBUS_FW_BUILD`. Runs on **every** build (platformio.ini). |
| **⚠ `engine_filter.py`** | PlatformIO `extra_scripts` in `[s3]` - a device env builds one engine (`custom_nimbus_engine`, 1 when absent) and drops the other engine's sources listed in `tools/engine/` (`engine2_only.txt` for an engine-1 env, `engine1_only.txt` for the `*-e2` envs). Every new engine-2 source gets its line there; `release_gate/check_engine_filter.py` enforces it. Tests: `test_engine_filter.py`. |
| **⚠ `make_manifest.py`** | Signs the OTA release manifest (release.yml). Message byte-identical to `nimbus::ota::buildSigMessage()`. |
| **⚠ `release/make_webflash_manifest.py`** | Builds the web-flasher manifest (release.yml). |
| **⚠ `release/variants.json`** | The one release variant table release.yml reads: stable rows (type, env, image, web-flash), the candidate entry (shipped empty), the releases repository. |
| **⚠ `release/publish_candidate.py`** | The release plan from `variants.json` (`plan`, written to `$GITHUB_ENV`), the candidate target check and `manifest-candidate.json` (`check-target`, `make`). Plans and writes locally only. Tests: `release/test_publish_candidate.py` (run in `pytest tools/release_gate`). |
| **⚠ `release/check_known_patterns.sh`** | Pre-commit leak gate (private `ops/leak_patterns.txt`; warns-and-passes for public contributors). |
| `release/seed_public_root.sh` | Maintainer gate - builds + verifies the public-repo root cut. Needs the private `ops/leak_patterns.txt`; refuses to run without it. |
| `release/lizard_whitelist.txt` | Baseline for the pre-commit lizard complexity gate. |
| `build_all.sh` | Compile-only gate: builds every PlatformIO environment and flag path. |

## Dev / contributor

| Tool | One-liner |
|---|---|
| `tftpreview.py` | Color counterpart for the TFT golden suite - `contact` sheet + tap-`regions` linter. |
| `nsn_send.py` | End-to-end broker→Bluetooth→device validation (needs `bleak` + macOS BT entitlement). |
| `harness-lab/` | Runs the real agent harness on the Mac instead of the device (see its README). |
| `context_report.py` | Context, cache, fold and cost figures per provider and turn from saved device logs (never contacts a device); `--check-baseline` re-derives the 15 Nov 2026 Lumi baseline. Tests: `test_context_report.py`. |
| `test_setup_device.py` | Unit tests for `setup_device.py`. |
| `test_usb_reset.py` | Unit tests for `usb_reset.py`'s setup error paths (no USB bus touched). |
| `test_device_http_auth.py` | Every tool that calls the device API signs in with the `X-Nimbus-Token` header, never a URL token (fake device, no network). |
| `test_tcal_wizard.py` | Unit tests for `tcal_wizard.py`. |
| **⚠ `release_gate/tag_shape.py`** | Release channel gate (release.yml): OTA tag vs virtual vn- tag, version.h coherence. |
| `release_gate/image_projection.py` | Flash bytes per object, folder and feature group from two `firmware.map` files: the engine-1 feature code not yet in the engine-2 image and the projected parity image (groups in `release_gate/parity_groups.json`); a parity lane runs it before and after. Tests: `release_gate/test_image_projection.py`. |
| **⚠ `release_gate/check_release_notes.py`** | Release notes gate (release.yml, run_gate.py): the one-line `docs/release-notes/<tag>.txt` the manifest carries, at most 95 bytes, ASCII, plain words. |

Bench-only and board-specific scripts (a live connector-QA harness, the quality
benchmark, panic-capture, the connector-shape grid) live in the private `ops/`
tree, not this public repo. The **Battery Lab** is its own project at
[ristllin/nimbus-battery-lab](https://github.com/ristllin/nimbus-battery-lab);
it drives the firmware's `DRAIN`/`STORAGE` commands, which remain here.
