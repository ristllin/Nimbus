# `docs/` - the canonical documentation tree

This directory is the **canonical source** for all Nimbus documentation. The
published site (https://ristllin.github.io/Nimbus/) is a generated *view* of it:
`website/scripts/migrate-docs.mjs` copies every page listed in its `PAGES` table
into `website/docs/**` with front matter and link fixes. Edit here, re-run the
migrate script, commit. Two site pages are hand-written and live only under
`website/docs/` (the intro and the getting-started guides); the generated mirror
under `website/docs/{quick-start,guides,reference,contributing}/` is rebuilt on
deploy and is not committed.

Three other generators read this tree - keep them in sync when you edit:

- `tools/gen_docs_pack.py` (CURATED list) - embeds selected docs into the firmware
  so the on-device assistant can read them. Re-run after editing a curated file.
- `tools/check_status_doc.py` - CI gate keeping `notifier-status-language.md`
  byte-consistent with `lib/core/src/status_style.cpp`.
- `tools/sounds/gen_sfx_map.py` - **generates `sfx-map.md`. Never edit that file by
  hand.**

## The docs tree

### Quick start
| File | Covers |
|---|---|
| `quick-start/what-you-need.md` | Parts, tools, and prerequisites |
| `quick-start/flash.md` | Flashing the firmware (browser flasher or the UART port) |
| `quick-start/setup-wizard.md` | Joining the setup network and walking the wizard |
| `quick-start/first-conversation.md` | First Orchestrator conversation |
| `quick-start/notifier-quick-start.md` | Bringing up the status light |

### Hardware
| File | Covers |
|---|---|
| `hardware.md` | Pinout, wiring, and first-flash guidance (overview) |
| `hardware/README.md` | Folder map for the hardware directory |
| `hardware/bom.md` | Consolidated bill of materials (both configurations) |
| `hardware/build-eink.md` | Build guide - e-paper + knob |
| `hardware/build-tft.md` | Build guide - touch TFT |
| `hardware/eink-knob.md` | Configuration A - e-paper + knob |
| `hardware/touch-tft.md` | Configuration B - touch TFT |


### Using Nimbus
| File | Covers |
|---|---|
| `modes-and-signals.md` | Every user-facing knob and what it changes |
| `led-ux.md` | The LED experience - motion and color |
| `notifier-status-language.md` | Session-status → color/animation map (CI-checked against code) |
| `sfx-map.md` | Audio state language (**generated** - never hand-edit) |
| `connectors.md` | External tools per provider |
| `people-and-privacy.md` | Roles, quotas, and what stays private |

### How it works
| File | Covers |
|---|---|
| `architecture.md` | The system, layer by layer |
| `turn-anatomy.md` | What the model sees in a turn |
| `architecture/orchestrator-live-turn.md` | The live orchestrator turn |
| `orchestrator-world.md` | The agent's memory and control surface |
| `memory.md` | RAM pools, the turn budget, and compaction |
| `orchestrator-storage.md` | Storage tiering (SD / PSRAM / NVS / LittleFS) |
| `sub-sessions.md` | Background sub-agents, end to end |
| `harness.md` | The agentic harness |
| `provider-wire.md` | Structured outputs and tool loops per provider |
| `security.md` | The auth model and open items |

### Reference
| File | Covers |
|---|---|
| `reference/config-and-nvs.md` | Config keys and NVS layout |
| `reference/tool-catalog.md` | The tool catalog |
| `reference/turn-contract.md` | The turn contract |
| `tools-and-commands.md` | User and contributor tools and console commands |
| `changelog.md` | Release history |

### Forking & contributing
| File | Covers |
|---|---|
| `development.md` | Build environments, the test ladder, golden-test flow, constraints |
| `ota.md` | Over-the-air firmware updates |
| `ota-operations.md` | OTA operations and maintenance |
| `self-hosted-ota.md` | Running your own OTA release channel |

## Rules of the road

- **Site structure** lives in `website/sidebars.js`; a page migrated by `PAGES`
  but absent from the sidebar is an orphan the site never links to.
- **Generated files are never hand-edited**: `sfx-map.md`,
  `website/docs/{guides,reference,quick-start,contributing}/**`, and
  `lib/core/include/nimbus/docs_pack_data.h`.
- **Docs follow every commit**: rename or move anything user-visible and you grep
  for the old wording and fix every hit in the same commit; before a release,
  `cd website && npm run build` must pass with zero broken links.
- Copy style (Title Case for tabs/buttons, sentence case for labels, "Wi-Fi" not
  "WiFi", "restart" not "reboot", US English) is defined in `AGENTS.md`.
