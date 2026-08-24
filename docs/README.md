# `docs/` - the canonical documentation tree

This directory is the **canonical source** for all Nimbus documentation. The
published site (https://docs.cumulo-nimbus.ai/) is a generated *view* of it:
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
| `quick-start/computer-setup.md` | Set up your computer for the command-line flasher (terminal, Python, PlatformIO, drivers) |
| `quick-start/flash.md` | Flashing the firmware (browser flasher or the UART port) |
| `quick-start/setup-wizard.md` | Joining the setup network and walking the wizard |
| `quick-start/first-conversation.md` | First Orchestrator conversation |
| `quick-start/notifier-quick-start.md` | Bringing up the status light |

### Hardware
| File | Covers |
|---|---|
| `hardware.md` | Pinout, wiring, and first-flash guidance (overview) |
| `hardware/README.md` | Folder map for the hardware directory |
| `hardware/bom.md` | Consolidated bill of materials (all configurations) |
| `hardware/build-tft.md` | Build guide - touch TFT |
| `hardware/touch-tft.md` | Configuration A - touch TFT (Solide S3) |
| `hardware/all-in-one-cyd.md` | Configuration B - all-in-one (Freenove CYD) |


### Using Nimbus
| File | Covers |
|---|---|
| `modes-and-signals.md` | Every user-facing knob and what it changes |
| `led-ux.md` | The LED experience - motion and color |
| `notifier-status-language.md` | Session-status → color/animation map (CI-checked against code) |
| `sfx-map.md` | Audio state language (**generated** - never hand-edit) |
| `connectors.md` | External tools per provider |
| `mcp.md` | The device as an MCP client and server |
| `people-and-privacy.md` | Roles, quotas, and what stays private |

### Cloud
| File | Covers |
|---|---|
| `cloud-relay.md` | Cloud access: reach the device from anywhere (remote tunnel) |
| `cloud/cumulo-key.md` | Use your Cumulo key on the device and from your own code |

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
| `provider-wire.md` | Structured outputs, tool loops, model catalog, and fallbacks per provider |
| `security.md` | The auth model and open items |

### Reference
| File | Covers |
|---|---|
| `reference/config-and-nvs.md` | Config keys and NVS layout |
| `reference/tool-catalog.md` | The tool catalog |
| `reference/turn-contract.md` | The turn contract |
| `reference/capabilities-matrix.md` | Provider x role x feature matrix (generated from the catalog) |
| `tools-and-commands.md` | User and contributor tools and console commands |
| `changelog.md` | Release history |

### Forking & contributing
| File | Covers |
|---|---|
| `development.md` | Build environments, the test ladder, golden-test flow, constraints |
| `ota.md` | Over-the-air firmware updates |
| `ota-operations.md` | OTA operations and maintenance |
| `self-hosted-ota.md` | Running your own OTA release channel |

## Page types (Divio)

Every page is one of the four [Divio](https://docs.divio.com/documentation-system/)
types, and the sidebar groups them by type so a reader lands in the right mode.
Keep new pages to one type; the few intentional blends below are called out so
they are not "fixed" into fragments (coherency over richness).

| Type (learning mode) | Sidebar group(s) | Pages |
|---|---|---|
| **Tutorial** (learning by doing) | Quick Start | `what-you-need`, `computer-setup`, `flash`, `setup-wizard`, `first-conversation`, `notifier-quick-start` |
| **How-to** (task recipes) | Hardware Build, Using Nimbus, Cloud, Forking & Contributing | `hardware/*`, `modes-and-signals`, `connectors`, `mcp`, `people-and-privacy`, `cloud-relay`, `cloud/cumulo-key`, `ota`, `ota-operations`, `self-hosted-ota`, `development` |
| **Reference** (look it up) | Reference | `reference/*`, `tools-and-commands`, `changelog`, `notifier-status-language`, `sfx-map`, `hardware/bom` |
| **Explanation** (understand why) | How It Works | `architecture`, `turn-anatomy`, `architecture/orchestrator-live-turn`, `orchestrator-world`, `memory`, `orchestrator-storage`, `sub-sessions`, `harness`, `provider-wire`, `security`, `led-ux` |

Intentional blends (kept whole on purpose):

- `what-you-need` is a tutorial that opens with a reference checklist; the
  checklist is what a first build needs in front of it.
- `flash` is a how-to whose back half is reference (reflash, recovery, build
  environments), clearly separated by a rule and a "the rest of this page is
  reference" line.
- `hardware.md` and `modes-and-signals.md` carry a short "for developers" source
  table at the end so a reader never has to leave the page to find where the
  behavior lives; the body stays single-type.

## Rules of the road

- **Site structure** lives in `website/sidebars.js`; a page migrated by `PAGES`
  but absent from the sidebar is an orphan the site never links to.
- **Generated files are never hand-edited**: `sfx-map.md`,
  `website/docs/{guides,reference,quick-start,contributing,cloud}/**`, and
  `lib/core/include/nimbus/docs_pack_data.h`.
- **Docs follow every commit**: rename or move anything user-visible and you grep
  for the old wording and fix every hit in the same commit; before a release,
  `cd website && npm run build` must pass with zero broken links.
- Copy style (Title Case for tabs/buttons, sentence case for labels, "Wi-Fi" not
  "WiFi", "restart" not "reboot", US English) is defined in `AGENTS.md`.
