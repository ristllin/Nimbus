#!/usr/bin/env python3
"""Release gate: the conversation summary (fold) can never stay off for days again.

Program 2026-10-18, history and summary fix (v5.1.2, lane E; owner ruling 2 of 15 Nov 2026).
On 2 Nov 2026 three failed folds on Lumi paused automatic summaries (CompactBreaker,
FoldStore breakerPaused) and nothing lifted the pause for 13 days. v5.1.2 removes every
permanent pause, keeps the daily cap and the owner alert in one device-wide file, and stays
readable by a v5.1.1 image on rollback. This gate checks the sources and the blob fixtures:

  1. no reachable permanent pause: the fold sources (the portable fold store and driver, the
     device binding, the hosted binding) never set a pause flag, never return ThrashPaused, and
     carry no owner copy that says summaries are paused;
  2. fold control present: the device keeps /fold_ctl.txt and the hosted instance a sibling
     file, and the shared record carries paidDay, paidCount, lastAlertDay, manualPendingChat and
     fwAtLastFail;
  3. blob round trip: every fold-blob fixture under test/support/fixtures (fold_blob_*.txt)
     reads under the v5.1.1 parser (FoldStore::begin at 591245f4: records split by 0x1E, eight
     fields split by 0x1F, numbers read up to the first non-digit, so a ';' suffix is skipped):
     every record keeps its chat id and summary, no number field is unreadable, the pause field
     is 0 for a candidate blob (a rollback must not start paused), and the blob fits the
     candidate's 64 KB. At least one v5.1.1 blob (fold_blob_v5_1_1*.txt) and one candidate blob
     (fold_blob_v5_1_2*.txt) must exist.

Usage:
    python3 tools/release_gate/check_fold_health.py [--root DIR]
Exit 0 = pass; exit 1 = a permanent pause is reachable, fold control is missing, or a blob
does not survive the round trip.
"""

from __future__ import annotations

import argparse
import glob
import os
import re
import sys

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

# The fold sources: every place a pause could be set, read or told to the owner.
FOLD_SOURCES = (
    "lib/core/include/nimbus/orch/compact.h",
    "lib/core/src/orch_compact.cpp",
    "lib/harness/include/nimbus/harness/fold_driver.h",
    "lib/harness/src/fold_driver.cpp",
    "lib/harness/src/engine_fold.cpp",
    "src/agent/orchestrator.cpp",
    "src/agent/orch_persist.cpp",
    "nimbusd/src/rig.h",
    "nimbusd/src/hosted_fold.h",
)
# A pause flag set to true, a thrash pause returned, a breaker that pauses.
PAUSE_CODE = (
    (re.compile(r"\b(?:breakerPaused|thrashPaused)\s*=\s*true\b"), "sets a pause flag"),
    (re.compile(r"\breturn\s+FoldDue::ThrashPaused\b"), "returns ThrashPaused"),
    (re.compile(r"\bpaused\s*=\s*true\s*;\s*return\s+true\b"), "a breaker that pauses on a failure"),
)
# Owner copy that says automatic summaries are off.
PAUSE_COPY = re.compile(r"summar(?:y|ies)[^\"\n]{0,80}\b(?:are|is)\s+paused", re.IGNORECASE)
FOLD_CTL_FIELDS = ("paidDay", "paidCount", "lastAlertDay", "manualPendingChat", "fwAtLastFail")
FOLD_BLOB_MAX = 64 * 1024
SUMMARY_MAX = 6144
LEGACY_FIELDS = 8  # chat, bytes, msgs, epoch[;thru], turns[;softStart], fails, paused, summary


def read(path: str) -> str:
    with open(path, encoding="utf-8", errors="replace") as f:
        return f.read()


def strip_comments(src: str) -> str:
    """Drop // and /* */ comments so a comment that names the old pause is not a finding."""
    src = re.sub(r"/\*.*?\*/", lambda m: "\n" * m.group(0).count("\n"), src, flags=re.S)
    return re.sub(r"//[^\n]*", "", src)


def pause_problems(root: str) -> "list[str]":
    errs = []
    for rel in FOLD_SOURCES:
        path = os.path.join(root, rel)
        if not os.path.exists(path):
            continue
        src = read(path)
        code = strip_comments(src)
        for rx, what in PAUSE_CODE:
            for m in rx.finditer(code):
                line = code.count("\n", 0, m.start()) + 1
                errs.append(f"{rel}:{line}: {what} - a permanent pause is reachable")
        for m in PAUSE_COPY.finditer(code):
            line = code.count("\n", 0, m.start()) + 1
            errs.append(f"{rel}:{line}: owner copy says summaries are paused")
    return errs


def tree_text(root: str, sub: str, exts=(".cpp", ".h")) -> str:
    out = []
    for dirpath, _, files in os.walk(os.path.join(root, sub)):
        for name in files:
            if name.endswith(exts):
                out.append(read(os.path.join(dirpath, name)))
    return "\n".join(out)


def fold_ctl_problems(root: str) -> "list[str]":
    errs = []
    device = strip_comments(tree_text(root, "src"))
    hosted = strip_comments(tree_text(root, "nimbusd/src"))
    shared = strip_comments(tree_text(root, "lib"))
    if "/fold_ctl.txt" not in device:
        errs.append("the device keeps no /fold_ctl.txt (src/): the daily cap and the alert day would ride per chat")
    if "fold_ctl" not in hosted:
        errs.append("the hosted instance keeps no fold control file (nimbusd/src/)")
    for f in FOLD_CTL_FIELDS:
        if not re.search(rf"\b{f}\b", shared):
            errs.append(f"the shared fold control record has no {f} (lib/)")
    return errs


def to_u32(s: str) -> "int | None":
    """v5.1.1 toU32: digits up to the first non-digit; None when it starts with none."""
    m = re.match(r"\d+", s)
    return int(m.group(0)) & 0xFFFFFFFF if m else None


def legacy_records(blob: str) -> "list[list[str]]":
    """The records a v5.1.1 image reads from a blob (FoldStore::begin)."""
    return [r.split("\x1f") for r in blob.split("\x1e") if r.strip()]


def blob_problems(path: str, candidate: bool) -> "list[str]":
    name = os.path.basename(path)
    with open(path, "rb") as f:
        raw = f.read()
    errs = []
    if len(raw) > FOLD_BLOB_MAX:
        errs.append(f"{name}: {len(raw)} B, over the {FOLD_BLOB_MAX} B blob cap")
    blob = raw.decode("utf-8", errors="replace")
    recs = legacy_records(blob)
    if not recs:
        errs.append(f"{name}: no record a v5.1.1 image can read")
    for i, f in enumerate(recs):
        where = f"{name} record {i + 1}"
        if len(f) < LEGACY_FIELDS:
            errs.append(f"{where}: {len(f)} fields, a v5.1.1 image needs {LEGACY_FIELDS}")
            continue
        if not f[0].strip():
            errs.append(f"{where}: no chat id")
        for k, label in ((1, "bytes"), (2, "messages"), (3, "fold epoch"), (4, "turns"), (5, "failures")):
            if to_u32(f[k]) is None:
                errs.append(f"{where}: the {label} field {f[k][:20]!r} reads as no number on v5.1.1")
        paused = to_u32(f[6])
        if paused is None or paused > 2:
            errs.append(f"{where}: pause field {f[6][:10]!r} is not 0, 1 or 2")
        elif candidate and paused != 0:
            errs.append(f"{where}: a candidate blob stores pause {paused}; a rollback would start paused")
        summary = "\x1f".join(f[7:])
        if len(summary.encode()) > SUMMARY_MAX:
            errs.append(f"{where}: summary {len(summary.encode())} B, over {SUMMARY_MAX} B")
    return errs


def round_trip_problems(root: str) -> "list[str]":
    fx = os.path.join(root, "test", "support", "fixtures")
    legacy = sorted(glob.glob(os.path.join(fx, "fold_blob_v5_1_1*.txt")))
    cand = sorted(glob.glob(os.path.join(fx, "fold_blob_v5_1_2*.txt")))
    errs = []
    if not legacy:
        errs.append("no v5.1.1 fold blob fixture (test/support/fixtures/fold_blob_v5_1_1*.txt)")
    if not cand:
        errs.append(
            "no candidate fold blob fixture (test/support/fixtures/fold_blob_v5_1_2*.txt): the rollback read is unproven"
        )
    for p in legacy:
        errs.extend(blob_problems(p, candidate=False))
    for p in cand:
        errs.extend(blob_problems(p, candidate=True))
    return errs


def problems(root: str) -> "list[str]":
    return pause_problems(root) + fold_ctl_problems(root) + round_trip_problems(root)


def main(argv: "list[str] | None" = None) -> int:
    ap = argparse.ArgumentParser(description="Fold health release gate (v5.1.2)")
    ap.add_argument("--root", default=REPO, help="the tree to check (default: this repository)")
    args = ap.parse_args(argv)
    errs = problems(args.root)
    for e in errs:
        print(f"FAIL fold health: {e}")
    if errs:
        print(f"FAIL fold health: {len(errs)} problem(s), release blocked")
        return 1
    print("PASS fold health: no permanent pause is reachable, fold control is device-wide, the blobs round-trip")
    return 0


if __name__ == "__main__":
    sys.exit(main())
