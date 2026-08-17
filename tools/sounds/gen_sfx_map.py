#!/usr/bin/env python3
"""Generate docs/sfx-map.md - the full action -> audio map, per theme + mode + level.

Single source of truth, so the doc can't drift:
  - curation (which events land in which pools + the embedded basic set):
    palette.py
  - per-mode level thresholds (which verbosity voices an event): the kRows
    table in ../../lib/core/src/sfx_map.cpp (parsed, not duplicated).

Run: python3 gen_sfx_map.py   (writes ../../docs/sfx-map.md)
Re-run after editing palette.py or the sfx_map.cpp rank table.
"""

import pathlib
import re
import sys

HERE = pathlib.Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
from palette import BASIC_EVENTS, EVENTS  # noqa: E402

RANK_CPP = HERE / "../../lib/core/src/sfx_map.cpp"
OUT = HERE / "../../docs/sfx-map.md"

LEVELS = {0: "none", 1: "light", 2: "medium", 3: "heavy", 255: "-"}
RANK_CONST = {"kLevelNone": 0, "kLevelLight": 1, "kLevelMedium": 2, "kLevelHeavy": 3, "kNever": 255}
THEMES = ["pulse"]


def parse_ranks():
    """{slug: (orchRank, notifRank)} from the kRows table."""
    text = RANK_CPP.read_text()
    ranks = {}
    for m in re.finditer(r'\{Ev::\w+,\s*"([a-z_]+)",\s*(kLevel\w+|kNever),\s*(kLevel\w+|kNever)\}', text):
        slug, orch, notif = m.group(1), m.group(2), m.group(3)
        ranks[slug] = (RANK_CONST[orch], RANK_CONST[notif])
    return ranks


def lvl(rank):
    return LEVELS.get(rank, "?")


def main():
    ranks = parse_ranks()
    missing = set(EVENTS) - set(ranks)
    if missing:
        sys.exit(f"gen_sfx_map: palette slugs missing from sfx_map.cpp: {sorted(missing)}")
    lines = []
    w = lines.append

    w("# Nimbus SFX - action → audio map")
    w("")
    w("> **Generated** by `tools/sounds/gen_sfx_map.py` from `palette.py` (curation)")
    w("> and `lib/core/src/sfx_map.cpp` (per-mode level thresholds). Do not edit by")
    w("> hand - re-run the generator.")
    w("")
    w("Every device action maps to a short, wordless sound cue - synthesized tones")
    w("sharing one musical palette, so the device sounds like one instrument. What")
    w("you actually hear depends on three axes:")
    w("")
    w("- **Mode** - **Notifier** (status display; deliberately sparse - the broker")
    w("  floods job events during a coding session) vs **Orchestrator** (the agent).")
    w("- **Level (verbosity)** - `none` (0, silent) · `light` (1) · `medium` (2) ·")
    w("  `heavy` (3); shown in the UI as **Off / Low / Medium / High**. An action")
    w("  fires when the mode's level ≥ its threshold below. Defaults: Notifier")
    w("  **none** (silent out of the box), Orchestrator **medium**.")
    w("- **Storage tier** - with **no SD card** the device plays the small **embedded**")
    w("  set (the attention-critical events below); with an SD card it resolves")
    w("  `custom → theme → general → embedded → silence`, adding variants and the")
    w("  full 24-event coverage. Missing files fall through silently by design.")
    w("")
    w("**Your own sounds:** drop 22.05 kHz mono 16-bit WAVs named `<slug>-<n>.wav`")
    w("(n from 0) into `/sfx/custom/` on the SD card - they win over every built-in")
    w("pool and are never touched by the background sync.")
    w("")
    w("## Event map")
    w("")
    w("| event | Orchestrator ≥ | Notifier ≥ | embedded | SD variants (general + theme) |")
    w("|---|---|---|---|---|")
    for slug in EVENTS:
        orch, notif = ranks[slug]
        pools = EVENTS[slug]
        sd = " + ".join(f"{p}×{n}" for p, n in pools.items())
        emb = "✓" if slug in BASIC_EVENTS else "-"
        w(f"| `{slug}` | {lvl(orch)} | {lvl(notif)} | {emb} | {sd} |")
    w("")
    w("## Themes")
    w("")
    w("One theme ships today - **Pulse** (alternate-seed renders of the same tone")
    w("recipes, audibly distinct takes). The device resolves any `/sfx/<theme>/`")
    w("directory by name, so new themes are a content drop, not a firmware change.")
    w("")
    w("Events absent from the embedded tier are silent without an SD card - a")
    w("deliberate degradation, never an error.")
    w("")
    OUT.write_text("\n".join(lines))
    print(f"wrote {OUT} ({len(EVENTS)} events, themes: {', '.join(THEMES)})")


if __name__ == "__main__":
    main()
