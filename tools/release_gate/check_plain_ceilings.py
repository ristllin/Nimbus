#!/usr/bin/env python3
"""Gate: the plain engine's request fits its ceilings on the real build (PRD sections 2, 4, 11; lane RB-E2).

Two inputs, both generated and both checked for freshness elsewhere:
- test/golden/plain_prompt_core.txt: the figures test_plain_request measures on the real
  build (lib/harness/src/plain_request.cpp composing a synthetic Lumi-shaped state with
  notes at their cap), one `measure` line per role and host (one fixed tool pack for every
  device and person, ruling 41: a line that names a pack is malformed). The native suite fails
  when the golden differs from what the build composes, so these are the build's numbers.
- test/golden/model_text_catalog.txt: every engine-2 model-facing literal by class
  (gen_model_text_catalog.py).

It fails (PLAIN CEILINGS RED) when:
- a role or host has no measure line, or a line is malformed;
- a measured figure passes its ceiling: core 3,072 B including the tools index of 512 B;
  system (core, skills index, pinned notes) 8,192 B; at most 14 tools in 10,240 B; a
  description over 240 B; the digest block over the standard 6,744 B plus its two headers;
- an engine-2 catalog class passes its ceiling, or the catalog is stale;
- an engine-2 class changed with no entry in coverage/PROMPT-CHANGES.md (the
  no-prompt-patch ledger, read for engine 2 only), so a seeded 300 B addition is red.

Usage: python3 tools/release_gate/check_plain_ceilings.py [--root DIR]
Exit 0 with "PLAIN CEILINGS GREEN", 1 with "PLAIN CEILINGS RED".
"""

from __future__ import annotations

import argparse
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

import check_no_prompt_patch as npp  # noqa: E402
import gen_model_text_catalog as cat  # noqa: E402

REPO = os.path.dirname(os.path.dirname(HERE))
GOLDEN = "test/golden/plain_prompt_core.txt"

ROLES = ("owner", "member", "guest")
HOSTS = ("120s", "60s")
# Figure -> ceiling (PRD section 4 and section 2's fixed overhead; 30 B: the two block headers).
CEILINGS = {
    "core": 3072,
    "index": 512,
    "system": 8192,
    "tools": 14,
    "tools_bytes": 10240,
    "longest_description": 240,
    "digests": 6744 + 30,
}
_MEASURE = re.compile(r"^measure role=(\w+) host=(\w+)((?: \w+=\d+)+)$")


def measure_problems(text: str) -> "list[str]":
    errs, seen = [], set()
    for line in text.splitlines():
        if not line.startswith("measure "):
            continue
        m = _MEASURE.match(line)
        if not m:
            errs.append(f"{GOLDEN}: malformed line: {line}")
            continue
        key = m.group(1, 2)
        figures = dict((k, int(v)) for k, v in (kv.split("=") for kv in m.group(3).split()))
        if key in seen:
            errs.append(f"{GOLDEN}: {' '.join(key)} measured twice")
        seen.add(key)
        for fig, ceiling in CEILINGS.items():
            if fig not in figures:
                errs.append(f"{' '.join(key)}: no {fig} figure")
            elif figures[fig] > ceiling:
                errs.append(f"{' '.join(key)}: {fig} {figures[fig]} is over its ceiling {ceiling}")
    for key in ((r, h) for r in ROLES for h in HOSTS):
        if key not in seen:
            errs.append(f"{GOLDEN}: no measure line for role={key[0]} host={key[1]}")
    return errs


def catalog_problems(root: str) -> "list[str]":
    try:
        entries = cat.build_e2(root)
    except cat.CatalogError as e:
        return [str(e)]
    if entries is None:
        return [f"{cat.E2_TEXTS} does not exist"]
    golden_rel = cat.CATALOGS["e2"]["golden"]
    errs = []
    if cat._read(root, golden_rel) != cat.render("e2", entries):
        errs.append(f"{golden_rel} is stale: regenerate with gen_model_text_catalog.py --write")
    stats = cat.class_stats("e2", entries)
    for cls, st in stats.items():
        if st["bytes"] > cat.E2_CEILINGS[cls]:
            errs.append(f"e2 {cls}: {st['bytes']} B is over its ceiling {cat.E2_CEILINGS[cls]} B")
    changes = npp.parse_changes(cat._read(root, npp.CHANGES) or "")
    errs += [e for e in npp.check_ledger(root, changes, {"e2": stats}) if e.startswith(("e2 ", "PC-"))]
    return errs


def run(root: str) -> "list[str]":
    text = cat._read(root, GOLDEN)
    errs = [f"{GOLDEN} is missing: GOLDEN_UPDATE=1 pio test -e native -f test_plain_request"] if text is None else []
    return errs + measure_problems(text or "") + catalog_problems(root)


def main(argv: "list[str] | None" = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--root", default=REPO)
    args = ap.parse_args(argv)
    errs = run(args.root)
    for e in errs:
        print(f"plain-ceilings: {e}")
    print("PLAIN CEILINGS RED" if errs else "PLAIN CEILINGS GREEN")
    return 1 if errs else 0


if __name__ == "__main__":
    sys.exit(main())
