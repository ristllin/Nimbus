#!/usr/bin/env python3
"""Nimbus sound-pack palette - which events land in which pools.

Slug invariants (build_assets.py enforces them):
  * every slug MUST match lib/core/src/sfx_map.cpp exactly (the device derives
    WAV filenames from those slugs);
  * NO '-' inside a slug (the device parses "<slug>-<n>.wav" on the LAST dash);
  * <= 8 variants per (event, pool) - kMaxVariants in src/sfx/sound_fx.cpp.

Pools:
  basic    embedded in firmware flash (src/sfx/sfx_basic_data.h), 16 kHz.
           The full 24-event tone set exceeds the 200 KB flash budget, so the
           basic tier carries the 12 attention-critical events only; the rest
           resolve from SD or stay silent (fall-through is silent by design).
  general  SD tier fallback pool, 22.05 kHz, 2-3 tone variants per event.
  pulse    the default THEME: alternate-seed renders of the same recipes
           (micro-detune/timing variation), so theme != general audibly.

TODO(arcade theme): a Kenney-clip based "arcade" theme (CC0 downloads + an
owner listening pass) is planned but deliberately NOT wired here yet - when it
lands, add pool "arcade" below with SourceRefs into its curated clip set; the
device already resolves any /sfx/<theme>/ pool by name with no firmware change.
"""

# Basic tier (embedded): the attention-critical set that must work with zero
# SD + zero Wi-Fi, forever.
BASIC_EVENTS = [
    "boot",
    "wifi_up",
    "wifi_down",
    "ble_up",
    "ble_down",
    "ble_bond",
    "agent_spawn",
    "agent_done",
    "error",
    "needs_you",
    "low_battery",
    "mode_switch",
]

# All 24 slugs, in lib/core sfx_map.cpp enum order, with per-pool variant
# counts. Every event is a pure tone synth (gen_tones.RECIPES) - the pack is
# complete without any downloads.
EVENTS = {
    "boot": {"general": 2, "pulse": 2},
    "wifi_up": {"general": 2, "pulse": 2},
    "wifi_down": {"general": 2, "pulse": 2},
    "ble_up": {"general": 2, "pulse": 2},
    "ble_down": {"general": 2, "pulse": 2},
    "ble_bond": {"general": 2, "pulse": 2},
    "agent_spawn": {"general": 3, "pulse": 3},
    "agent_done": {"general": 3, "pulse": 3},
    "error": {"general": 2, "pulse": 2},
    "needs_you": {"general": 3, "pulse": 3},
    "low_battery": {"general": 2, "pulse": 2},
    "battery_ok": {"general": 2, "pulse": 2},
    "mode_switch": {"general": 2, "pulse": 2},
    "sd_mounted": {"general": 2, "pulse": 2},
    "sd_lost": {"general": 2, "pulse": 2},
    "turn_start": {"general": 3, "pulse": 3},
    "reply_sent": {"general": 3, "pulse": 3},
    "voice_listen": {"general": 2, "pulse": 2},
    "voice_stop": {"general": 2, "pulse": 2},
    "mem_saved": {"general": 2, "pulse": 2},
    "net_degraded": {"general": 2, "pulse": 2},
    "net_ok": {"general": 2, "pulse": 2},
    "ask_cleared": {"general": 2, "pulse": 2},
    "sync_done": {"general": 2, "pulse": 2},
}

SD_POOLS = ["general", "pulse"]

MAX_VARIANTS = 8  # kMaxVariants in src/sfx/sound_fx.cpp
MAX_BASIC_PCM_BYTES = 200 * 1024  # flash rodata budget for the embedded tier
