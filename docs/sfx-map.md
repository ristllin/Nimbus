# Nimbus SFX - action → audio map

> **Generated** by `tools/sounds/gen_sfx_map.py` from `palette.py` (curation)
> and `lib/core/src/sfx_map.cpp` (per-mode level thresholds). Do not edit by
> hand - re-run the generator.

Every device action maps to a short, wordless sound cue - synthesized tones
sharing one musical palette, so the device sounds like one instrument. What
you actually hear depends on three axes:

- **Mode** - **Notifier** (status display; deliberately sparse - the broker
  floods job events during a coding session) vs **Orchestrator** (the agent).
- **Level (verbosity)** - `none` (0, silent) · `light` (1) · `medium` (2) ·
  `heavy` (3); shown in the UI as **Off / Low / Medium / High**. An action
  fires when the mode's level ≥ its threshold below. Defaults: Notifier
  **none** (silent out of the box), Orchestrator **medium**.
- **Storage tier** - with **no SD card** the device plays the small **embedded**
  set (the attention-critical events below); with an SD card it resolves
  `custom → theme → general → embedded → silence`, adding variants and the
  full 24-event coverage. Missing files fall through silently by design.

**Your own sounds:** drop 22.05 kHz mono 16-bit WAVs named `<slug>-<n>.wav`
(n from 0) into `/sfx/custom/` on the SD card - they win over every built-in
pool and are never touched by the background sync.

## Event map

| event | Orchestrator ≥ | Notifier ≥ | embedded | SD variants (general + theme) |
|---|---|---|---|---|
| `boot` | light | medium | ✓ | general×2 + pulse×2 |
| `wifi_up` | medium | heavy | ✓ | general×2 + pulse×2 |
| `wifi_down` | medium | heavy | ✓ | general×2 + pulse×2 |
| `ble_up` | medium | heavy | ✓ | general×2 + pulse×2 |
| `ble_down` | medium | heavy | ✓ | general×2 + pulse×2 |
| `ble_bond` | medium | medium | ✓ | general×2 + pulse×2 |
| `agent_spawn` | medium | - | ✓ | general×3 + pulse×3 |
| `agent_done` | medium | heavy | ✓ | general×3 + pulse×3 |
| `error` | light | light | ✓ | general×2 + pulse×2 |
| `needs_you` | light | light | ✓ | general×3 + pulse×3 |
| `low_battery` | light | light | ✓ | general×2 + pulse×2 |
| `battery_ok` | medium | heavy | - | general×2 + pulse×2 |
| `mode_switch` | medium | medium | ✓ | general×2 + pulse×2 |
| `sd_mounted` | medium | - | - | general×2 + pulse×2 |
| `sd_lost` | medium | - | - | general×2 + pulse×2 |
| `turn_start` | heavy | - | - | general×3 + pulse×3 |
| `reply_sent` | heavy | - | - | general×3 + pulse×3 |
| `voice_listen` | heavy | - | - | general×2 + pulse×2 |
| `voice_stop` | heavy | - | - | general×2 + pulse×2 |
| `mem_saved` | heavy | - | - | general×2 + pulse×2 |
| `net_degraded` | heavy | - | - | general×2 + pulse×2 |
| `net_ok` | heavy | - | - | general×2 + pulse×2 |
| `ask_cleared` | heavy | - | - | general×2 + pulse×2 |
| `sync_done` | heavy | - | - | general×2 + pulse×2 |

## Themes

One theme ships today - **Pulse** (alternate-seed renders of the same tone
recipes, audibly distinct takes). The device resolves any `/sfx/<theme>/`
directory by name, so new themes are a content drop, not a firmware change.

Events absent from the embedded tier are silent without an SD card - a
deliberate degradation, never an error.
