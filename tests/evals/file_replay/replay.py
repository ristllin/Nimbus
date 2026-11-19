#!/usr/bin/env python3
"""The file replay: real file shapes through the shared file aids, no model (README.md).

Each case is a stored file and one call: a day total (`sum`: read with column/from/to/sum), a
line-picked edit (`edit`) or an append (`append`). The driver runs lib/core's file aids, the
code both engines and nimbusd call; this script grades every answer against an oracle written
here in Python from the rules in file_aids.h, and prints the table:

- a total covers exactly the rows the result says it covers, and every other row of that day
  (a wrong field count, a blank or non-number cell) is named by line (ruling 49);
- an edit changes exactly the picked line or nothing, and a stale line number changes nothing;
- an append fences a torn tail and names an identical line;
- under a refusing allocator nothing changes and the result says so.

    python3 tests/evals/file_replay/replay.py [--tree <tree>] [--before <tree>]

Exit 1 when a case of the tree misses. `--before` adds a column for another tree (from RB-REC4 on).
"""

from __future__ import annotations

import argparse
import csv
import io
import json
import subprocess
import sys
import tempfile
from decimal import Decimal, InvalidOperation
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPO = HERE.parents[2]
sys.path.insert(0, str(HERE.parent / "families"))
import domains  # noqa: E402

SOURCES = ("lib/core/src/orch_file_aids.cpp", "lib/core/src/orch_csv_sum.cpp", "lib/core/src/orch_psram_alloc.cpp")
FIXED_DAYS = {"{D-2}": "2026-11-17", "{D-1}": "2026-11-18"}


def arduinojson(tree: Path) -> Path:
    for root in (tree, REPO, *sorted(REPO.parents[1].glob("*/nimbus"))):
        aj = root / ".pio/libdeps/native/ArduinoJson/src"
        if aj.is_dir():
            return aj
    sys.exit("no ArduinoJson under .pio/libdeps/native: run pio test -e native once in some tree")


def build(tree: Path, out: Path) -> Path:
    """Compile the driver against the tree's file aids (a few seconds; no lab build needed)."""
    inc = [f"-I{tree}/lib/core/include", f"-I{tree}/test/support", f"-I{arduinojson(tree)}"]
    cmd = ["c++", "-std=gnu++17", "-O1", "-DNIMBUS_NATIVE", *inc, str(HERE / "replay_driver.cpp")]
    cmd += [str(tree / s) for s in SOURCES] + ["-o", str(out)]
    subprocess.run(cmd, check=True)
    return out


def run(binary: Path, cases: list) -> dict:
    text = "".join(json.dumps(c) + "\n" for c in cases)
    done = subprocess.run([str(binary)], input=text, capture_output=True, text=True, check=True)
    return {r["id"]: r for r in map(json.loads, done.stdout.splitlines())}


# ---- cases ---------------------------------------------------------------------------


def corpus_cases() -> list:
    """The owner-shaped cases kept from the food replay (scrubbed in the repo since 1 Nov)."""
    out = []
    for c in json.loads((HERE / "corpus.json").read_text()):
        if c["kind"] != "sum":
            continue
        header = c["file"].split("\n", 1)[0].split(",")
        sums = [h for h in header if h in ("kcal", "protein_g")]
        out.append(
            {
                "id": c["id"],
                "kind": "sum",
                "seen": c["seen"],
                "file": c["file"],
                "column": header[0],
                "from": c["day"],
                "sum": sums,
            }
        )
    return out


def domain_cases() -> list:
    """Each domain's two-day file with the owner's row classes, and the same calls in all three."""
    out = []
    for name, dom in domains.DOMAINS.items():
        day = FIXED_DAYS["{D-1}"]
        plain = domains.dated(domains.render(dom), FIXED_DAYS)
        dirty = domains.dated(domains.render(dom, comment_before=6, blank_after=(9, 19)), FIXED_DAYS)
        lines = plain.split("\n")
        row = lines[8]  # a clean row of the 15-row day
        out += [
            {
                "id": f"{name}-day",
                "kind": "sum",
                "seen": "a 15-row day, 2 rows with bare commas, 1 unknown cell",
                "file": plain,
                "column": "date",
                "from": day,
                "sum": list(dom.sum_cols),
            },
            {
                "id": f"{name}-day-dirty",
                "kind": "sum",
                "seen": "the same with a comment line and stray blank lines",
                "file": dirty,
                "column": "date",
                "from": day,
                "sum": list(dom.sum_cols),
            },
            {
                "id": f"{name}-two-days",
                "kind": "sum",
                "seen": "both days as a range",
                "file": plain,
                "column": "date",
                "from": FIXED_DAYS["{D-2}"],
                "to": day,
                "sum": list(dom.sum_cols),
            },
            {
                "id": f"{name}-relog",
                "kind": "append",
                "seen": "the same row logged again",
                "file": plain,
                "text": row + "\n",
            },
            {
                "id": f"{name}-torn",
                "kind": "append",
                "seen": "a file cut mid-row by a power loss",
                "file": plain + lines[8][:9],
                "text": row + "\n",
            },
            {
                "id": f"{name}-dup-pick",
                "kind": "edit",
                "seen": "two identical rows; remove the second by line",
                "file": plain + row + "\n",
                "old": row,
                "new": "",
                "line": len(lines),
            },
            {
                "id": f"{name}-stale-line",
                "kind": "edit",
                "seen": "a line number that now names another row",
                "file": plain,
                "old": row,
                "new": row.replace(day, day[:-1] + "9"),
                "line": 10,
            },
            {
                "id": f"{name}-move-day",
                "kind": "edit",
                "seen": "a row moved to the day before",
                "file": plain,
                "old": row,
                "new": row.replace(day, FIXED_DAYS["{D-2}"]),
                "line": 9,
            },
            {
                "id": f"{name}-refused-edit",
                "kind": "edit",
                "seen": "an edit under a refusing allocator",
                "file": plain,
                "old": row,
                "new": row + "x",
                "line": 9,
                "refuse_at": 1,
            },
            {
                "id": f"{name}-refused-sum",
                "kind": "sum",
                "seen": "a day total under a refusing allocator",
                "file": plain,
                "column": "date",
                "from": day,
                "sum": list(dom.sum_cols),
                "refuse_at": 1,
            },
        ]
    return out


# ---- the oracle ----------------------------------------------------------------------


def _dec(cell: str):
    s = cell.strip()
    if not s:
        return "blank"
    try:
        if not all(ch in "+-.0123456789" for ch in s) or s in (".", "+", "-"):
            return "nan"
        return Decimal(s)
    except InvalidOperation:
        return "nan"


def _same_shape(a: str, b: str) -> bool:
    """A digit where b has a digit, a letter where b has a letter, the same byte elsewhere."""
    return all((x.isdigit() and y.isdigit()) or (x.isalpha() and y.isalpha()) or x == y for x, y in zip(a, b))


def _in_range(key: str, lo: str, hi: str):
    """True, False, or None (not compared): the prefix rule of file_aids.h for date-like keys."""
    key = key.strip()
    if not key or len(key) < len(lo):
        return None
    if not _same_shape(key, lo) or (lo[-1].isdigit() and len(key) > len(lo) and key[len(lo)].isdigit()):
        return None
    return lo <= key[: len(lo)] and key[: len(hi)] <= hi


def _records(text: str):
    """(line number, cells) of every CSV record after the header; comment and empty lines are no records."""
    header = None
    for n, line in enumerate(text.split("\n"), 1):
        if not line.strip() or line.lstrip().startswith("#"):
            continue
        cells = next(csv.reader(io.StringIO(line)))
        if header is None:
            header = cells
            continue
        yield header, n, cells


def oracle_sum(c: dict) -> dict:
    """Sums over the rows a total may cover, and the day's rows it must name instead."""
    want = {s: Decimal(0) for s in c["sum"]}
    named = {"field_count": [], "blank": [], "nan": []}
    for header, n, cells in _records(c["file"]):
        at = header.index(c["column"])
        hit = _in_range(cells[at] if len(cells) > at else "", c["from"], c.get("to") or c["from"])
        if hit is not False and len(cells) != len(header):
            named["field_count"].append(n)
        elif hit:
            for s in c["sum"]:
                v = _dec(cells[header.index(s)])
                if isinstance(v, Decimal):
                    want[s] += v
                else:
                    named[v].append(n)
    return {
        "sums": want,
        "field_count": named["field_count"],
        "blank": sorted(set(named["blank"])),
        "not_number": sorted(set(named["nan"])),
    }


def grade_sum(c: dict, r: dict) -> tuple:
    if c.get("refuse_at"):
        return (not r.get("ok") and "not enough memory" in r.get("error", "")), r.get("error", "")
    if not r.get("ok"):
        return False, r.get("error", "no answer")
    o = oracle_sum(c)
    for s, v in o["sums"].items():
        if Decimal(r["sums"].get(s, "nan")) != v:
            return False, f"{s}={r['sums'].get(s)} but the covered rows add to {v}"
    for key in ("field_count", "blank", "not_number"):
        named = r[key]["lines"]
        if r[key]["count"] != len(o[key]) or (len(o[key]) <= 8 and named != o[key]):
            return False, f"{key} names {named} (count {r[key]['count']}), the day has {o[key]}"
    return True, r["tail"]


def grade_edit(c: dict, r: dict) -> tuple:
    if c.get("refuse_at"):
        return (not r.get("ok") and not r.get("changed")), r.get("result", "")
    lines = c["file"].split("\n")
    picked = c["line"] - 1
    if 0 <= picked < len(lines) and lines[picked] == c["old"]:
        if c["new"] == "":
            want = "\n".join(lines[:picked] + lines[picked + 1 :])
        else:
            want = "\n".join(lines[:picked] + [c["new"]] + lines[picked + 1 :])
        ok = r.get("ok") and r.get("file") == want
        return ok, r.get("result", "")
    ok = not r.get("ok") and not r.get("changed") and "line" in r.get("result", "")
    return ok, r.get("result", "")


def grade_append(c: dict, r: dict) -> tuple:
    torn = not c["file"].endswith("\n")
    body = c["file"] + ("\n" if torn else "")
    same = [n for n, ln in enumerate(body.split("\n"), 1) if ln == c["text"].rstrip("\n")]
    if r.get("fence") != torn or r.get("file") != body + c["text"]:
        return False, f"fence={r.get('fence')} for a {'torn' if torn else 'whole'} tail"
    if same and f"identical to line {same[-1]}" not in r.get("note", ""):
        return False, f"note {r.get('note')!r} does not name line {same[-1]}"
    return True, r.get("note", "").strip("; ") or "no note"


GRADERS = {"sum": grade_sum, "edit": grade_edit, "append": grade_append}


def grade(cases: list, results: dict) -> list:
    rows = []
    for c in cases:
        r = results.get(c["id"], {})
        ok, said = GRADERS[c["kind"]](c, r) if r else (False, "no result")
        rows.append({"id": c["id"], "kind": c["kind"], "seen": c["seen"], "ok": bool(ok), "said": said})
    return rows


def table(after: list, before: list | None) -> list:
    head = "| case | shape | after |" + (" before |" if before else "") + " what the tree says |"
    out = [head, "|" + "---|" * (head.count("|") - 1)]
    prior = {r["id"]: r for r in before or []}
    for r in after:
        cell = "PASS" if r["ok"] else "MISS"
        extra = f" {'PASS' if prior[r['id']]['ok'] else 'MISS'} |" if before else ""
        out.append(f"| {r['id']} | {r['seen']} | {cell} |{extra} {str(r['said'])[:140].replace('|', '/')} |")
    return out


def all_cases() -> list:
    return corpus_cases() + domain_cases()


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--tree", type=Path, default=REPO)
    ap.add_argument("--before", type=Path)
    args = ap.parse_args(argv)
    cases = all_cases()
    with tempfile.TemporaryDirectory() as tmp:
        after = grade(cases, run(build(args.tree.resolve(), Path(tmp) / "after"), cases))
        before = grade(cases, run(build(args.before.resolve(), Path(tmp) / "before"), cases)) if args.before else None
    print("\n".join(table(after, before)))
    missed = [r["id"] for r in after if not r["ok"]]
    print(
        f"\n{len(after) - len(missed)} of {len(after)} cases pass"
        + (f"; missed: {', '.join(missed)}" if missed else "")
    )
    return 1 if missed else 0


if __name__ == "__main__":
    sys.exit(main())
