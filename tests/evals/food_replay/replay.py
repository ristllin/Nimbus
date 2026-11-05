#!/usr/bin/env python3
"""Run the food replay on one or two trees and print the before and after table (README.md)."""

from __future__ import annotations

import argparse
import glob
import json
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent


def build(tree: Path, out: Path) -> Path:
    """Link the driver against the tree's harness-lab objects (make -C tools/harness-lab first)."""
    lab = tree / "tools/harness-lab/build"
    objs = [o for o in glob.glob(str(lab / "*.o")) if not o.endswith(("/main.o", "/compose.o"))]
    compose = lab / "compose.o"
    if compose.exists():
        objs.append(str(compose))
    if not objs:
        sys.exit(f"no lab objects under {lab}: run make -C tools/harness-lab there first")
    aj = tree / ".pio/libdeps/native/ArduinoJson/src"
    inc = [f"-I{tree}/lib/core/include", f"-I{tree}/lib/harness/include", f"-I{tree}/include", f"-I{aj}"]
    cmd = [
        "c++",
        "-std=gnu++17",
        "-O1",
        "-DNIMBUS_NATIVE",
        *inc,
        str(HERE / "replay_driver.cpp"),
        *objs,
        "-lcurl",
        "-o",
        str(out),
    ]
    subprocess.run(cmd, check=True)
    return out


def run(binary: Path, cases: list[dict]) -> dict:
    text = "".join(json.dumps(c) + "\n" for c in cases)
    done = subprocess.run([str(binary)], input=text, capture_output=True, text=True, check=True)
    return {r["id"]: r for r in map(json.loads, done.stdout.splitlines())}


def said(case: dict, res: dict) -> str:
    """What the device says about this case, in a table cell."""
    if case["kind"] == "fold":
        return f"{res.get('kcal'):g} kcal from {res.get('rows')} rows ({res.get('folded')} folded, {res.get('dropped')} dropped)"
    if case["kind"] == "sum":
        if res.get("unread"):
            return f"not known: {res.get('kcal'):g} counted, {res['unread']} unread"
        return f"{res.get('receipt', res.get('kcal')):g} kcal"
    text = res.get("reply" if case["kind"] == "append" else "tail", "").strip().replace("\n", " ")
    if case["kind"] == "append":
        at = text.find("- a row dated")
        return text[at : at + 70] + "..." if at >= 0 else "no note"
    return text or "nothing added"


def good(case: dict, res: dict) -> bool:
    want = case["want"]
    if case["kind"] in ("sum", "fold"):
        if "kcal" in want and abs(res.get("kcal", -1) - want["kcal"]) > 0.05:
            return False
        if "protein" in want and abs(res.get("protein", -1) - want["protein"]) > 0.05:
            return False
        if "unread" in want and res.get("unread") != want["unread"]:
            return False
        return all(p in res.get("block", "") for p in want.get("block_has", []))
    text = res.get("reply" if case["kind"] == "append" else "tail", "")
    if "has" in want and want["has"] not in text:
        return False
    return not ("nothing" in want and text.strip())


def case_table(cases: list[dict], after: dict, before: dict | None) -> tuple[list[str], int]:
    lines = ["| case | what happened | before | after | pass |", "|---|---|---|---|---|"]
    missed = 0
    for c in cases:
        ok = good(c, after[c["id"]])
        missed += 0 if ok else 1
        was = said(c, before[c["id"]]) if before else ""
        lines.append(f"| {c['id']} | {c['seen']} | {was} | {said(c, after[c['id']])} | {'PASS' if ok else 'FAIL'} |")
    if before:
        passed = sum(good(c, before[c["id"]]) for c in cases)
        lines.append(f"\n{len(cases)} cases: before {passed} pass, after {len(cases) - missed} pass.")
    return lines, missed


def full_table(path: str, after_bin: Path, before_bin: Path | None) -> list[str]:
    rows = [json.loads(x) for x in Path(path).read_text(encoding="utf-8").splitlines() if x.strip()]
    res = run(after_bin, rows)
    was = run(before_bin, rows) if before_bin else {}
    lines = [f"\n## Every delivered reply ({len(rows)})\n\n| id | reply | before | after |\n|---|---|---|---|"]
    changed = 0
    for r in rows:
        a, b = res[r["id"]].get("tail", "").strip(), was.get(r["id"], {}).get("tail", "").strip()
        if a != b:
            changed += 1
            lines.append(f"| {r['id']} | {r['reply'][:90]!r} | {b[:80]} | {a[:100]} |")
    lines.append(f"\n{changed} of {len(rows)} replies would read differently.")
    return lines


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--after", required=True)
    ap.add_argument("--before")
    ap.add_argument("--work", default="/tmp")
    ap.add_argument("--full", help="a private JSONL of delivered replies: {id, owner, reply}")
    ap.add_argument("--out", help="write the table here as well")
    args = ap.parse_args()
    cases = json.loads((HERE / "corpus.json").read_text(encoding="utf-8"))
    work = Path(args.work)
    after_bin = build(Path(args.after).resolve(), work / "replay_after")
    before_bin = build(Path(args.before).resolve(), work / "replay_before") if args.before else None
    lines, missed = case_table(cases, run(after_bin, cases), run(before_bin, cases) if before_bin else None)
    if args.full:
        lines += full_table(args.full, after_bin, before_bin)
    text = "\n".join(lines) + "\n"
    print(text)
    if args.out:
        Path(args.out).write_text(text, encoding="utf-8")
    return 1 if missed else 0


if __name__ == "__main__":
    sys.exit(main())
