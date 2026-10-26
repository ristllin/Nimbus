#!/usr/bin/env python3
"""Release notes gate: every OTA release carries one short line for its owners.

The OTA release manifest has a `notes` field that the device already reads into a
96-byte field (95 characters and the terminator, lib/core ota_logic.h), shows on the
web page's Software update panel, and hands the assistant after the update so it can
tell the owner what changed. The release workflow fills it from
docs/release-notes/<tag>.txt; this gate refuses a release whose line is missing or
would not survive that field intact.

The line must be: one line (a single trailing newline is fine), 1 to 95 bytes,
printable ASCII (the device screen and serial are ASCII only, and a multi-byte
character could be cut in half at the 95-byte edge), and in plain words: no ticket
numbers. A release candidate (vX.Y.Z-rcN) may use its own file or fall back to
vX.Y.Z.txt.

Usage:
    python3 tools/release_gate/check_release_notes.py --tag "$GITHUB_REF_NAME" [--print]
    python3 tools/release_gate/check_release_notes.py      # the include/version.h version
--print writes the line alone to stdout (what release.yml passes to make_manifest.py).
Exit 0 = the line is good; 1 = refused (a ::error:: line says why).
"""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
NOTES_DIR = REPO / "docs" / "release-notes"
VERSION_H = REPO / "include" / "version.h"
MAX_BYTES = 95  # the device's notes field is char[96] (ota_logic.h ManifestInfo::notes)

_VERSION_RE = re.compile(r'^\s*#define\s+NIMBUS_FW_VERSION\s+"([^"]*)"', re.MULTILINE)
_TICKET_RE = re.compile(r"\bCUM-\d+\b")  # the project tracker's issue ids


def current_version(text: str) -> str | None:
    m = _VERSION_RE.search(text)
    return m.group(1) if m and m.group(1) else None


def candidates(tag: str) -> list[Path]:
    """The files a tag may take its line from, in order: its own, then (for -rc) its version's."""
    out = [NOTES_DIR / f"{tag}.txt"]
    if "-rc" in tag:
        out.append(NOTES_DIR / f"{tag.split('-rc', 1)[0]}.txt")
    return out


def check_line(raw: bytes) -> tuple[str | None, str]:
    """(line, "") when `raw` is a good notes file, else (None, why)."""
    text = raw[:-1] if raw.endswith(b"\n") else raw
    if b"\n" in text or b"\r" in text:
        return None, "must be a single line"
    if not text.strip():
        return None, "is empty"
    if len(text) > MAX_BYTES:
        return None, f"is {len(text)} bytes; the device keeps {MAX_BYTES}"
    if any(b < 0x20 or b > 0x7E for b in text):
        return None, "must be printable ASCII (no tabs, accents, emoji or dashes outside ASCII)"
    line = text.decode("ascii")
    if line != line.strip():
        return None, "has spaces at an end"
    t = _TICKET_RE.search(line)
    if t:
        return None, f"names a ticket ({t.group(0)}); release notes are for owners, in plain words"
    return line, ""


def notes_for(tag: str) -> tuple[str | None, str]:
    """(line, "") for `tag`, else (None, why naming the file to write)."""
    for path in candidates(tag):
        if path.is_file():
            line, why = check_line(path.read_bytes())
            rel = path.relative_to(REPO)
            return (line, "") if line else (None, f"{rel} {why}")
    names = " or ".join(str(p.relative_to(REPO)) for p in candidates(tag))
    return None, f"no release notes for {tag}: write one line of at most {MAX_BYTES} bytes to {names}"


def main(argv: "list[str] | None" = None) -> int:
    ap = argparse.ArgumentParser(description="Release notes gate (one line per OTA release)")
    ap.add_argument("--tag", help="the release tag (default: include/version.h NIMBUS_FW_VERSION)")
    ap.add_argument("--print", action="store_true", help="print only the line, for the workflow")
    args = ap.parse_args(argv)
    tag = args.tag or current_version(VERSION_H.read_text())
    if not tag:
        print("::error::include/version.h has no NIMBUS_FW_VERSION")
        return 1
    line, why = notes_for(tag)
    if line is None:
        print(f"::error::release notes: {why}")
        return 1
    if args.print:
        print(line)
    else:
        print(f"release notes for {tag}: OK ({len(line)} bytes): {line}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
