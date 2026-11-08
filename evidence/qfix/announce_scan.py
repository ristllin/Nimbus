#!/usr/bin/env python3
"""Announcement-guard precision over every stored live reply of the lane (lane QFIX, fix pass).

Builds the standalone scanner (orch::announcesWorkNow from lib/core, no firmware, no network),
feeds it every turn reply stored under tests/evals/owner_regressions/runs/*/raw/*/*/rep*.transcript.json
and evidence/qfix-ab, and prints the counts the PR_BODY quotes:

  replies, replies with no tool call, flagged with no tool call, flagged on a turn that had tool calls.

Usage: python3 evidence/qfix/announce_scan.py [--list] [--out FILE]
  --list   print every flagged reply (path, tool-call state, the flagged sentence)
  --out    write the same listing to FILE
No model call is made. Run from the tree root.
"""

import argparse
import glob
import json
import os
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
SRC = ["evidence/qfix/announce_scan.cpp", "lib/core/src/orch_reply_guard.cpp"]
INC = ["-Ilib/core/include", "-Ilib/core/src", "-I.pio/libdeps/native/ArduinoJson/src"]


def build(binary):
    cmd = ["g++", "-std=c++17", "-O1"] + INC + SRC + ["-o", binary]
    subprocess.run(cmd, cwd=ROOT, check=True)


def replies():
    pats = [
        "tests/evals/owner_regressions/runs/*/raw/*/*/rep*.transcript.json",
        "evidence/qfix-ab/**/rep*.transcript.json",
    ]
    seen = set()
    for pat in pats:
        for p in sorted(glob.glob(os.path.join(ROOT, pat), recursive=True)):
            if p in seen:
                continue
            seen.add(p)
            try:
                with open(p, encoding="utf-8", errors="replace") as f:
                    d = json.load(f)
            except (OSError, ValueError):
                continue
            for i, t in enumerate(d.get("turns") or []):
                r = t.get("reply")
                if isinstance(r, str) and r:
                    yield os.path.relpath(p, ROOT), i, d.get("scenario", "?"), bool(t.get("tool_calls")), r


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--list", action="store_true")
    ap.add_argument("--out")
    a = ap.parse_args()
    tmp = tempfile.mkdtemp(prefix="qfix-scan-")
    binary = os.path.join(tmp, "scan")
    build(binary)
    rows = list(replies())
    payload = "".join(r.replace("\\", "\\\\").replace("\n", "\\n") + "\n" for _, _, _, _, r in rows)
    out = subprocess.run([binary], input=payload, capture_output=True, text=True, check=True).stdout.split("\n")
    n = len(rows)
    notool = sum(1 for r in rows if not r[3])
    flag_notool, flag_tool, lines = 0, 0, []
    for (p, i, sc, tools, reply), o in zip(rows, out):
        if not o.startswith("1"):
            continue
        if tools:
            flag_tool += 1
        else:
            flag_notool += 1
        lines.append(f"{'TOOLS' if tools else 'NOTOOL'}\t{sc}\t{p}#turn{i}\t{o.split(chr(9), 1)[1]}")
    summary = f"replies={n} no_tool_call={notool} flagged_no_tool={flag_notool} flagged_with_tool_calls={flag_tool}"
    print(summary)
    if a.list:
        print("\n".join(lines))
    if a.out:
        with open(os.path.join(ROOT, a.out), "w", encoding="utf-8") as f:
            f.write(summary + "\n" + "\n".join(lines) + "\n")
    return 0


if __name__ == "__main__":
    sys.exit(main())
