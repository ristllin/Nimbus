#!/usr/bin/env python3
"""Release tag shape gate: which release channel does a pushed tag drive?

.github/workflows/release.yml serves two release channels, each started by its own tag:

  ota      vX.Y.Z (or vX.Y.Z-rcN)     the firmware OTA release AND the nimbusd:<tag> image
  virtual  vn-vX.Y.Z or vn-vX.Y.Z-N   ONLY the nimbusd:<tag> Virtual Nimbus image: no
                                      firmware build, no OTA manifest, no web-flash

Both live in release.yml because the registry credential (keyless workload identity) is
issued only to that workflow on a pushed refs/tags/v* tag, and "vn-..." starts with "v".
The registry never moves an existing tag, so a repeat virtual release for the same
firmware version takes the next -N (vn-v4.5.7, vn-v4.5.7-1, vn-v4.5.7-2, ...).

Either shape must name the CURRENT include/version.h NIMBUS_FW_VERSION (the image reports
it as `fw`), and the tag becomes the image tag, so it must be a valid image tag. Anything
else is refused, so no job runs on a tag it was not meant for.

Usage:
    python3 tools/release_gate/tag_shape.py --tag "$GITHUB_REF_NAME"
Prints `channel=<ota|virtual>` and appends that line to $GITHUB_OUTPUT when it is set.
Exit 0 = accepted, 1 = refused (a ::error:: line says why).
"""

from __future__ import annotations

import argparse
import os
import re
import sys
from pathlib import Path

VERSION_H = Path(__file__).resolve().parents[2] / "include" / "version.h"

OTA = "ota"
VIRTUAL = "virtual"
CHANNELS = (OTA, VIRTUAL)
VIRTUAL_PREFIX = "vn-"

_VERSION_RE = re.compile(r'^\s*#define\s+NIMBUS_FW_VERSION\s+"([^"]*)"', re.MULTILINE)
# What a container registry accepts as a tag (the git tag is pushed as nimbusd:<tag>).
_IMAGE_TAG_RE = re.compile(r"^[A-Za-z0-9_][A-Za-z0-9_.-]{0,127}$")
# The -N of a repeat virtual release: a positive integer, no leading zero.
_SEQ_RE = re.compile(r"^[1-9][0-9]*$")


def read_version(text: str) -> str | None:
    """NIMBUS_FW_VERSION from the text of include/version.h, or None if absent/empty."""
    m = _VERSION_RE.search(text)
    return m.group(1) if m and m.group(1) else None


def classify(tag: str, version: str) -> tuple[str | None, str]:
    """(channel, reason) for `tag` against NIMBUS_FW_VERSION `version`; channel None = refused."""
    if not version:
        return None, "include/version.h has no NIMBUS_FW_VERSION"
    if not _IMAGE_TAG_RE.match(tag):
        return None, f"tag {tag!r} is not a valid image tag (letters, digits, '_', '.', '-'; at most 128)"
    if tag.startswith(VIRTUAL_PREFIX):
        return _classify_virtual(tag, version)
    # Firmware tags: exactly the pre-existing gate (the tag may carry -rcN on the same version).
    if tag == version or tag.startswith(version + "-rc"):
        return OTA, f"firmware release {tag} of {version}"
    return None, (
        f"tag {tag} != NIMBUS_FW_VERSION {version} (firmware: {version} or {version}-rcN; "
        f"virtual: {VIRTUAL_PREFIX}{version} or {VIRTUAL_PREFIX}{version}-N)"
    )


def _classify_virtual(tag: str, version: str) -> tuple[str | None, str]:
    rest = tag[len(VIRTUAL_PREFIX) :]
    if rest == version:
        return VIRTUAL, f"virtual release {tag} of {version}"
    if rest.startswith(version + "-"):
        seq = rest[len(version) + 1 :]
        if _SEQ_RE.match(seq):
            return VIRTUAL, f"virtual release {tag} of {version}"
        return None, f"virtual tag {tag}: -{seq} is not a release number (use -1, -2, ...)"
    return None, (
        f"virtual tag {tag} does not name NIMBUS_FW_VERSION {version} "
        f"(expected {VIRTUAL_PREFIX}{version} or {VIRTUAL_PREFIX}{version}-N)"
    )


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description="Decide the release channel of a pushed tag.")
    ap.add_argument("--tag", default=os.environ.get("GITHUB_REF_NAME", ""), help="default: $GITHUB_REF_NAME")
    ap.add_argument("--version-h", type=Path, default=VERSION_H, help="default: include/version.h")
    args = ap.parse_args(argv)

    try:
        version = read_version(args.version_h.read_text(encoding="utf-8"))
    except OSError as e:
        print(f"::error::cannot read {args.version_h}: {e}")
        return 1
    print(f"tag={args.tag} version.h={version}")
    channel, reason = classify(args.tag, version or "")
    if channel is None:
        print(f"::error::{reason}")
        return 1

    out = os.environ.get("GITHUB_OUTPUT")
    if out:
        with open(out, "a", encoding="utf-8") as f:
            f.write(f"channel={channel}\n")
    elif os.environ.get("GITHUB_ACTIONS") == "true":
        # In a workflow the jobs gate on this output; without it every job would skip
        # and the run would read green having shipped nothing. Fail closed instead.
        print("::error::GITHUB_OUTPUT is not set, so the channel cannot reach the jobs")
        return 1
    print(f"channel={channel}")
    print(reason)
    return 0


if __name__ == "__main__":
    sys.exit(main())
