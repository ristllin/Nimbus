# tools/ - maintenance contract

Short rules for changing anything under `tools/`. The human-facing index is
[`README.md`](README.md); the full catalog is
[`docs/tools-and-commands.md`](../docs/tools-and-commands.md).

## Adding a tool

1. **Pick the audience.** End-user device tool, generator, CI gate, build/release,
   or dev/contributor - the [`README.md`](README.md) table it belongs in tells you
   who it's for. A tool that names a specific bench board, needs bench WiFi/tokens,
   or burns real provider credits is **bench-only** and does **not** belong in the
   public tree - it lives in the private `ops/` tree.
2. **Add a catalog entry.** New user-visible tool → a row in
   [`docs/tools-and-commands.md`](../docs/tools-and-commands.md) *and* a one-liner
   in [`README.md`](README.md), in the same change. On-device docs are a generated
   pack - if you edited `docs/tools-and-commands.md`, re-run `gen_docs_pack.py`
   (below) and commit the regenerated header too.
3. **Does it need a test?** A tool with real logic (a parser, a guarded installer,
   a wizard) gets a `test_<name>.py` - see `test_setup_device.py`,
   `test_tcal_wizard.py`. A thin HTTP/serial wrapper usually doesn't.
4. **Ruff + lizard apply** to `tools/**/*.py` (see `.pre-commit-config.yaml`,
   `ruff.toml`). New code must clear CCN≤15 / len≤120 / args≤6 - do **not** add a
   `release/lizard_whitelist.txt` line for new code (the whitelist is a shrinking
   baseline of pre-existing offenders only).

## ⚠ Load-bearing tools - breaking these breaks CI/build/release

Enumerated with their consumer, so a rename is caught before it ships:

| Tool | Consumer | Breaks |
|---|---|---|
| `git_version.py` | `platformio.ini` `extra_scripts` | **every** build (injects `NIMBUS_FW_BUILD`) |
| `check_param_consumers.py` | `.github/workflows/checks.yml` | the checks CI job |
| `check_status_doc.py` | `.github/workflows/checks.yml` | the checks CI job |
| `webui_concat_check.py` | `.github/workflows/checks.yml` | the checks CI job |
| `make_manifest.py` | `.github/workflows/release.yml` | OTA release signing (must stay byte-identical to `nimbus::ota::buildSigMessage()`) |
| `release/make_webflash_manifest.py` | `.github/workflows/release.yml` | web-flasher manifest |
| `release/check_known_patterns.sh` | `.pre-commit-config.yaml` | the pre-commit leak gate |

Rename or move one of these and you must update its consumer in the same change.

## Generators: edit the generator, re-run, commit the output

A generated file has a generator that is its single source of truth - never
hand-edit the output. The generator→output contracts:

- `gen_docs_pack.py` → `lib/core/include/nimbus/docs_pack_data.h` (ships in
  firmware; OTA updates it - a stale header ships stale on-device docs).
- `gen_nsn_vectors.py` → `tools/nsn_vectors.json` + embedded header.
- `gen_qr_vectors.py` → the QR known-answer vectors.
- `sounds/` → `dist/`, the manifest, `src/sfx/sfx_basic_data.h`, `docs/sfx-map.md`.
- `logo/gen_logo.py` → `assets/logo*.svg`, the website logo/favicon/social card,
  `include/web/ui_logo.h`.

The root [`AGENTS.md`](../AGENTS.md) "Docs follow every commit" section is the
authoritative list - check it when in doubt.

## No bench/board-specific scripts in the public tree

Anything that names a bench board, needs device-identifying secrets (tokens,
MACs, bench WiFi), or spends real provider credits stays in the private `ops/`
tree. The Battery Lab is its own repo
([ristllin/nimbus-battery-lab](https://github.com/ristllin/nimbus-battery-lab)),
driving firmware commands that remain here.
