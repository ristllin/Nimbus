#!/usr/bin/env python3
"""Merge-time check: the images this tree just built stay under their static DRAM ceilings.

check_static_dram.py is otherwise only run by the release workflow, on a tag. A merge that grows
static internal RAM then fails at the worst moment: one-shot tag, the release already under way.
This runs the same check at merge time, on what a battery built, and adds what a merge needs:
  - every required image must exist (a missing one fails, it is never skipped);
  - every required image must be this commit's (the build embeds `git describe` as
    NIMBUS_FW_BUILD), so a stale .pio directory cannot pass for a fresh build.

Run it after the battery's device builds, in the tree they built:
    python3 tools/release_gate/merge_static_dram.py [--tree PATH] [--require VARIANT,VARIANT]
The default required images are the two the battery builds (nimbus-tft, freenove-28). The other
variants in static_dram_ceilings.json are checked too when this commit built them.
The last line is the marker: STATIC DRAM GREEN or STATIC DRAM RED. Exit 0 / 1.
"""

from __future__ import annotations

import argparse
import os
import subprocess
import sys
from typing import List, Optional

import check_static_dram as dram

DEFAULT_REQUIRED = ("nimbus-tft", "freenove-28")


def build_id(tree: str) -> Optional[str]:
    """The commit's id as tools/git_version.py writes it into the image, without "-dirty"."""
    try:
        return subprocess.check_output(
            ["git", "-C", tree, "describe", "--tags", "--always", "--match", "v[0-9]*"],
            text=True,
            stderr=subprocess.DEVNULL,
        ).strip()
    except (OSError, subprocess.CalledProcessError):
        return None


def judge_image(path: str, variant: str, want_id: str, ceilings: dict) -> "tuple[bool, str]":
    """(ok, message) for one built image: this commit's, and under its ceiling."""
    try:
        with open(path, "rb") as f:
            blob = f.read()
        used = dram.static_dram(blob)
    except (OSError, ValueError) as e:
        return False, f"{variant}: {path}: {e}"
    if want_id.encode() not in blob:
        return False, f"{variant}: {path} is not this commit's build (no {want_id} inside): build it first"
    return dram.judge(variant, used, ceilings)


def check_tree(tree: str, required: "tuple[str, ...]", ceilings: dict, want_id: Optional[str]) -> bool:
    if not want_id:
        print(f"FAIL static DRAM: {tree}: no git build id")
        return False
    unknown = [v for v in required if v not in ceilings]
    ok = True
    for v in unknown:
        print(f"FAIL {v}: no recorded ceiling")
        ok = False
    for variant, row in ceilings.items():
        path = os.path.join(tree, ".pio", "build", row["env"], "firmware.elf")
        must = variant in required
        if not must and not os.path.exists(path):
            print(f"SKIP {variant}: not built here")
            continue
        passed, msg = judge_image(path, variant, want_id, ceilings)
        if not must and not passed and "not this commit's" in msg:
            print(f"SKIP {msg}")
            continue
        print(("PASS " if passed else "FAIL ") + msg)
        ok = ok and passed
    return ok


def main(argv: "List[str] | None" = None) -> int:
    ap = argparse.ArgumentParser(description="Static DRAM ceilings on the images this commit built")
    ap.add_argument("--tree", default=dram.REPO, help="the built tree (default: this repo)")
    ap.add_argument("--require", default=",".join(DEFAULT_REQUIRED), help="variants that must be present")
    ap.add_argument("--ceilings", default=dram.CEILINGS, help=argparse.SUPPRESS)
    args = ap.parse_args(argv)
    required = tuple(v for v in args.require.split(",") if v)
    try:
        ceilings = dram.load_ceilings(args.ceilings)
    except (OSError, ValueError, KeyError) as e:
        print(f"FAIL static DRAM: cannot read {args.ceilings}: {e}")
        print("STATIC DRAM RED")
        return 1
    ok = bool(required) and check_tree(args.tree, required, ceilings, build_id(args.tree))
    print("STATIC DRAM GREEN" if ok else "STATIC DRAM RED")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
