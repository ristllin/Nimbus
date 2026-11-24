#!/usr/bin/env python3
"""Rewrite the route goldens from the wire as it is, then say what moved.

The route goldens (test/support/goldens/wire/, router design section 11 layers L1 and L1p) pin
the bytes the engine-2 wire sends for fixed stored transcripts and what it reads from every
recorded answer. They are written by the native suite test_route_goldens itself: this tool runs
it once with ROUTE_GOLDENS_WRITE=1, removes every golden the suite's MANIFEST no longer names,
runs the suite again to compare, and prints each golden that is new, removed or changed (with
the first differing byte offset). The diff it leaves is part of the PR: a lane that changes a
golden names every changed file and why.

Usage (from anywhere; it works in the repository root):
  python3 tools/router/refresh_goldens.py            rewrite, prune, verify, report
  python3 tools/router/refresh_goldens.py --check    compare only (no file is written)

It runs pio; never run it while another pio command runs in the same worktree.
"""

from __future__ import annotations

import argparse
import os
import pathlib
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parents[2]
GOLDENS = ROOT / "test" / "support" / "goldens" / "wire"
SUITE = "test_route_goldens"


def snapshot() -> dict[str, bytes]:
    """Every golden file's bytes, by its path under GOLDENS."""
    if not GOLDENS.is_dir():
        return {}
    return {p.relative_to(GOLDENS).as_posix(): p.read_bytes() for p in GOLDENS.rglob("*") if p.is_file()}


def run_suite(write: bool) -> int:
    env = dict(os.environ)
    env.pop("ROUTE_GOLDENS_WRITE", None)
    if write:
        env["ROUTE_GOLDENS_WRITE"] = "1"
    cmd = ["pio", "test", "-e", "native", "-f", SUITE]
    print("[refresh_goldens] " + " ".join(cmd) + (" (write mode)" if write else ""), flush=True)
    return subprocess.run(cmd, cwd=ROOT, env=env, check=False).returncode


def manifest_names() -> set[str]:
    lines = (GOLDENS / "MANIFEST").read_text(encoding="utf-8").splitlines()
    return {ln for ln in lines if ln and not ln.startswith("#")}


def prune(names: set[str]) -> list[str]:
    """Remove every golden file the manifest does not name; the removed paths."""
    removed = []
    for p in sorted(GOLDENS.rglob("*")):
        rel = p.relative_to(GOLDENS).as_posix()
        if p.is_file() and rel != "MANIFEST" and rel not in names:
            p.unlink()
            removed.append(rel)
    for d in sorted((d for d in GOLDENS.rglob("*") if d.is_dir()), reverse=True):
        if not any(d.iterdir()):
            d.rmdir()
    return removed


def first_diff(a: bytes, b: bytes) -> int:
    for i, (x, y) in enumerate(zip(a, b)):
        if x != y:
            return i
    return min(len(a), len(b))


def report(before: dict[str, bytes], after: dict[str, bytes]) -> int:
    """Print what moved; the number of goldens that changed, appeared or went."""
    moved = 0
    for rel in sorted(set(before) | set(after)):
        old, new = before.get(rel), after.get(rel)
        if old == new:
            continue
        moved += 1
        if old is None:
            print(f"  new      {rel} ({len(new)} B)")
        elif new is None:
            print(f"  removed  {rel}")
        else:
            print(f"  changed  {rel}: first difference at byte {first_diff(old, new)} ({len(old)} B -> {len(new)} B)")
    unchanged = len(set(before) & set(after)) - sum(1 for r in before if r in after and before[r] != after[r])
    print(f"[refresh_goldens] {moved} golden file(s) moved, {unchanged} unchanged")
    return moved


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--check", action="store_true", help="compare only; write nothing")
    args = ap.parse_args()
    if args.check:
        return run_suite(write=False)
    before = snapshot()
    rc = run_suite(write=True)
    if rc != 0:
        print("[refresh_goldens] the write run failed; nothing pruned", file=sys.stderr)
        return rc
    removed = prune(manifest_names())
    if removed:
        print(f"[refresh_goldens] pruned {len(removed)} golden(s) no fixture renders")
    report(before, snapshot())
    rc = run_suite(write=False)
    if rc != 0:
        print("[refresh_goldens] the compare run after writing failed", file=sys.stderr)
    return rc


if __name__ == "__main__":
    sys.exit(main())
