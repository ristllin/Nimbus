#!/usr/bin/env python3
"""Release gate: no reply-check line is in a device image (lane RB-Q1b, v5.2.0).

The old engine's reply guards read one turn and matched words: they sent correct answers back
or appended "(Correction: ...)" and "(Note: ...)" lines to them (rebuild/evidence/DIAGNOSIS.md
RC-09: 39 of 702 delivered replies carried such a line). RB-Q1b deleted the claim scanner, the
claim check, the spawn-claim and owner-voice checks; this gate keeps them deleted. It reads the
built images (what is flashed, not the sources: a file no image links, such as a guard source a
later lane deletes, is not in the build) and fails on any text that starts "(Correction" or
"(Note" unless ALLOWED names it.

ALLOWED only shrinks. Each entry is a text the image still holds for a stated reason, with the
lane that removes it or why it is not a reply check. An entry that no scanned image holds any
more fails too ("stale: remove it"), so a deletion always removes its row.

Usage:
    python3 tools/release_gate/check_no_harness_lines.py                # .pio/build/<env>/firmware.bin
    python3 tools/release_gate/check_no_harness_lines.py --image <bin> [--image <bin> ...]
Exit 0 with "NO HARNESS LINES GREEN", 1 with "NO HARNESS LINES RED" (a line, a stale entry, or
no built image to read).
"""

from __future__ import annotations

import argparse
import os
import sys

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
ENVS = ("esp32s3", "esp32s3-cyd", "test", "test-cyd")
LEADS = (b"(Correction", b"(Note")

# The texts an image may still hold, by their start. Owner: the lane that deletes it, or
# "device" for a line that states the device's own record of its own action. Empty since
# RB-Q1c deleted the reply guard's later-promise line and reworded the two speech notes into
# the plain device-line form ("Note: ...", no parenthesis; ruling 34): an entry may only be
# added back with an orchestrator ruling.
ALLOWED: "tuple[dict, ...]" = ()   # empty since RB-Q1c (v5.2.0): no image holds such a line


def found_lines(blob: bytes) -> "list[str]":
    """Every text in the image that starts with a lead: up to its NUL or a byte that is not text."""
    out = []
    for lead in LEADS:
        at = blob.find(lead)
        while at >= 0:
            end = at
            while end < len(blob) and end - at < 400 and blob[end] != 0 and (blob[end] >= 0x20 or blob[end] in (9, 10)):
                end += 1
            out.append(blob[at:end].decode("utf-8", "replace"))
            at = blob.find(lead, at + 1)
    return out


def problems(images: "dict[str, bytes]", allowed=ALLOWED) -> "list[str]":
    errs = []
    if not images:
        return ["no built image to read: build the device envs first (pio run -e esp32s3-cyd ...)"]
    used = set()
    for name, blob in sorted(images.items()):
        for line in found_lines(blob):
            hit = next((i for i, a in enumerate(allowed) if line.startswith(a["text"])), None)
            if hit is None:
                errs.append(f"{name}: a reply-check line is in the image: {line[:120]!r}")
            else:
                used.add(hit)
    for i, a in enumerate(allowed):
        if i not in used:
            errs.append(f"stale entry, no image holds it any more (remove it): {a['text'][:80]!r} ({a['owner']})")
        if not a.get("owner") or not a.get("why"):
            errs.append(f"entry without an owner and a reason: {a['text'][:80]!r}")
    return errs


def default_images(root: str = REPO) -> "dict[str, bytes]":
    out = {}
    for env in ENVS:
        p = os.path.join(root, ".pio", "build", env, "firmware.bin")
        if os.path.exists(p):
            with open(p, "rb") as f:
                out[env] = f.read()
    return out


def main(argv: "list[str] | None" = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--image", action="append", default=[], help="an image to read (default: the built envs)")
    args = ap.parse_args(argv)
    if args.image:
        images = {}
        for p in args.image:
            with open(p, "rb") as f:
                images[os.path.basename(os.path.dirname(p)) or p] = f.read()
    else:
        images = default_images()
    errs = problems(images)
    for e in errs:
        print(f"no-harness-lines: {e}")
    print(f"read {len(images)} image(s): {', '.join(sorted(images)) or 'none'}")
    print("NO HARNESS LINES GREEN" if not errs else "NO HARNESS LINES RED")
    return 0 if not errs else 1


if __name__ == "__main__":
    sys.exit(main())
