#!/usr/bin/env python3
"""Gate: a lane's merge touches only the files of its entry in tools/engine/owners.json.

The rebuild runs many lanes at once and merges them one at a time (PRD section 10). A lane
that edits a file another lane owns turns a clean merge into a conflict or, worse, a silent
overwrite of work merged before it. owners.json holds each lane's files, built from the
files_owned text of the program's PLAN.json; this gate reads the files the lane's branch
changed since its merge base with the integration branch (a move counts as both paths, a
deletion counts) and fails on any file outside:

- the lane's own entry (globs: '*' stays inside one folder, '**' spans folders),
  plus the files listed in each file its entry names under files_from;
- the shared entries every lane may touch (docs ride the change, the generated docs pack);
- the per-lane entries (its own tools/release_gate/image_budget.d/<lane>.json, ...);
- with --declared PR_BODY.md, the backticked paths under its "Cross-lane edits" heading,
  printed as DECLARED so the merge review sees each one.

Reports:
    --overlaps        every path two lanes own, and whether the hot-file order of PRD
                      section 10 (owners.json hot_files) sequences it
    --check-plan P    owners.json has one entry per lane of PLAN.json, with unchanged text

Usage:
    python3 tools/release_gate/check_lane_files.py [--lane RB-X] [--base-ref rb-main] [--declared PR_BODY.md]
Exit 0 with "LANE FILES GREEN <lane>", exit 1 with "LANE FILES RED <lane>".
"""

from __future__ import annotations

import argparse
import json
import os
import re
import subprocess
import sys

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
OWNERS = os.path.join(REPO, "tools", "engine", "owners.json")
THIS_REPO = "nimbus"
TAG = "[gate:lane-files]"


def load_owners(path: str) -> dict:
    with open(path, encoding="utf-8") as fh:
        return json.load(fh)


_REGEX_CACHE: "dict[str, re.Pattern[str]]" = {}


def glob_regex(pattern: str) -> "re.Pattern[str]":
    """'**' spans folders, '*' and '?' stay inside one."""
    if pattern not in _REGEX_CACHE:
        out = []
        for tok in re.split(r"(\*\*|\*|\?)", pattern):
            out.append({"**": ".*", "*": "[^/]*", "?": "[^/]"}.get(tok, re.escape(tok)))
        _REGEX_CACHE[pattern] = re.compile("".join(out))
    return _REGEX_CACHE[pattern]


def matches(pattern: str, path: str) -> bool:
    return pattern == path or glob_regex(pattern).fullmatch(path) is not None


def listed_files(root: "str | None", rel: str) -> "list[str]":
    """Paths listed one per line in a repo file ('#' comments), for files_from."""
    if not root or not os.path.isfile(os.path.join(root, rel)):
        return []
    with open(os.path.join(root, rel), encoding="utf-8") as fh:
        return [ln.split("#", 1)[0].strip() for ln in fh if ln.split("#", 1)[0].strip()]


def lane_globs(table: dict, lane: str, root: "str | None" = None) -> "list[str]":
    entry = table["lanes"][lane]
    globs = list(entry["files"])
    for rel in entry.get("files_from", []):
        globs += listed_files(root, rel)
    return globs


def common_globs(table: dict, lane: str) -> "list[str]":
    shared = [row["glob"] for row in table.get("shared", [])]
    return shared + [row["glob"].format(lane=lane) for row in table.get("per_lane", [])]


def owners_of(table: dict, path: str, root: "str | None") -> "list[str]":
    return [
        lane
        for lane, e in table["lanes"].items()
        if e.get("repo", THIS_REPO) == THIS_REPO and any(matches(g, path) for g in lane_globs(table, lane, root))
    ]


def check(table: dict, lane: str, changed: "list[str]", root: "str | None" = None, declared=frozenset()):
    """(ok, report lines, failures) for the files one lane's branch changed."""
    if lane not in table["lanes"]:
        return False, [], [f"{lane} has no entry in tools/engine/owners.json"]
    repo = table["lanes"][lane].get("repo", THIS_REPO)
    own = lane_globs(table, lane, root) if repo == THIS_REPO else []
    allowed = own + common_globs(table, lane)
    lines, fails = [f"{lane}: {len(changed)} file(s) changed since the merge base"], []
    for path in sorted(changed):
        if any(matches(g, path) for g in allowed):
            continue
        if path in declared:
            lines.append(
                f"DECLARED cross-lane edit: {path} (owners: {', '.join(owners_of(table, path, root)) or 'none'})"
            )
            continue
        who = [o for o in owners_of(table, path, root) if o != lane]
        owner = f"owned by {', '.join(who)}" if who else "owned by no lane"
        where = f"; {lane} owns files only in the {repo} repo" if repo != THIS_REPO else ""
        fails.append(f"{path} is outside {lane}'s entry ({owner}{where})")
    return not fails, lines, fails


def declared_paths(body: str) -> "set[str]":
    """Backticked paths under a 'Cross-lane edits' heading, up to the next heading."""
    out: "set[str]" = set()
    inside = False
    for line in body.splitlines():
        if re.match(r"^#{1,6}\s", line):
            inside = re.match(r"^#{1,6}\s*cross-lane edits\b", line, re.IGNORECASE) is not None
            continue
        if inside:
            out.update(tok for tok in re.findall(r"`([^`\s]+)`", line))
    return out


# --- reports: overlaps and plan coverage ---------------------------------------------------


def overlap_paths(table: dict) -> "list[str]":
    """The more specific path of every pair of globs from two lanes (same repo) that meet."""
    rows = [(lane, g, e.get("repo", THIS_REPO)) for lane, e in table["lanes"].items() for g in e["files"]]
    found: "dict[str, None]" = {}
    for i, (la, ga, ra) in enumerate(rows):
        for lb, gb, rb in rows[i + 1 :]:
            if la == lb or ra != rb:
                continue
            if matches(ga, gb):
                found[gb] = None
            elif matches(gb, ga):
                found[ga] = None
    return list(found)


def overlaps(table: dict) -> "list[dict]":
    """Each path two or more lanes own; sequenced when the hot-file order lists every one of them."""
    hot = table.get("hot_files", {})
    out = []
    for path in overlap_paths(table):
        lanes = [lane for lane, e in table["lanes"].items() if any(matches(g, path) for g in e["files"])]
        order = hot.get(path)
        outside = [lane for lane in lanes if order is None or lane not in order]
        out.append(
            {"path": path, "lanes": lanes, "sequenced": order is not None and not outside, "outside_order": outside}
        )
    return sorted(out, key=lambda r: (r["sequenced"], r["path"]))


def plan_problems(table: dict, plan: dict) -> "list[str]":
    errs = []
    plan_lanes = {row["id"]: row.get("files_owned", "") for row in plan["lanes"]}
    for lane, text in plan_lanes.items():
        entry = table["lanes"].get(lane)
        if entry is None:
            errs.append(f"{lane} is in PLAN.json but has no entry in owners.json")
        elif entry.get("plan_text", "") != text:
            errs.append(f"{lane}: its files_owned text in PLAN.json changed; rebuild its owners.json entry")
    errs += [
        f"{lane} has an owners.json entry but is not in the plan" for lane in table["lanes"] if lane not in plan_lanes
    ]
    return errs


def glob_problems(where: str, globs: "list") -> "list[str]":
    errs = []
    for g in globs:
        if not isinstance(g, str) or not g or g.startswith("/") or ".." in g.split("/"):
            errs.append(f"{where}: '{g}' must be a repo-relative path or glob")
    return errs


def table_problems(table: dict) -> "list[str]":
    errs = []
    for lane, e in table.get("lanes", {}).items():
        if not e.get("files"):
            errs.append(f"{lane}: no files")
        errs += glob_problems(lane, e.get("files", []) + e.get("files_from", []))
    errs += glob_problems("shared", [r.get("glob") for r in table.get("shared", [])])
    errs += glob_problems("per_lane", [r.get("glob") for r in table.get("per_lane", [])])
    for path, order in table.get("hot_files", {}).items():
        errs += [f"hot file {path}: {lane} has no entry" for lane in order if lane not in table.get("lanes", {})]
    return errs


# --- git and the command line ---------------------------------------------------------------


def git(root: str, *args: str) -> "tuple[int, str]":
    r = subprocess.run(["git", "-C", root, *args], capture_output=True, text=True)
    return r.returncode, (r.stdout if r.returncode == 0 else r.stderr).strip()


def default_base_ref(root: str) -> str:
    return "rb-main" if git(root, "rev-parse", "--verify", "-q", "rb-main^{commit}")[0] == 0 else "main"


def changed_files(root: str, base_ref: str, head: str) -> "tuple[list[str], str | None]":
    rc, mb = git(root, "merge-base", head, base_ref)
    if rc != 0:
        return [], f"cannot find the merge base of {head} and '{base_ref}': {mb}"
    rc, out = git(root, "diff", "--name-only", "--no-renames", mb, head)
    if rc != 0:
        return [], f"git diff failed: {out}"
    return [ln for ln in out.splitlines() if ln], None


def lane_from_branch(root: str) -> "str | None":
    rc, branch = git(root, "rev-parse", "--abbrev-ref", "HEAD")
    return branch[len("lane/") :] if rc == 0 and branch.startswith("lane/") else None


def print_overlaps(table: dict) -> int:
    for row in overlaps(table):
        state = "sequenced (hot-file order)" if row["sequenced"] else "NOT SEQUENCED"
        extra = (
            f"; outside the order: {', '.join(row['outside_order'])}"
            if row["outside_order"] and not row["sequenced"]
            else ""
        )
        print(f"{TAG} overlap {row['path']}: {', '.join(row['lanes'])}: {state}{extra}")
    return 0


def print_plan(table: dict, plan_path: str) -> int:
    with open(plan_path, encoding="utf-8") as fh:
        errs = plan_problems(table, json.load(fh)) + table_problems(table)
    for e in errs:
        print(f"{TAG} FAIL: {e}")
    print(f"{TAG} OWNERS {'RED' if errs else 'GREEN'} against {plan_path} ({len(table['lanes'])} lanes)")
    return 1 if errs else 0


def run_gate(args, table: dict, root: str) -> int:
    lane = args.lane or lane_from_branch(root)
    if not lane:
        print(f"{TAG} FAIL: not on a lane/<id> branch: pass --lane")
        print(f"{TAG} LANE FILES RED ?")
        return 1
    changed, err = changed_files(root, args.base_ref or default_base_ref(root), args.head)
    declared: "set[str]" = set()
    if args.declared:
        with open(args.declared, encoding="utf-8") as fh:
            declared = declared_paths(fh.read())
    ok, lines, fails = check(table, lane, changed, root, declared)
    if err:
        ok, fails = False, [err, *fails]
    for line in lines:
        print(f"{TAG} {line}")
    for f in fails:
        print(f"{TAG} FAIL: {f}")
    print(f"{TAG} LANE FILES {'GREEN' if ok else 'RED'} {lane}")
    return 0 if ok else 1


def main(argv: "list[str] | None" = None) -> int:
    ap = argparse.ArgumentParser(description="Lane file-ownership gate (lane RB-GATE0)")
    ap.add_argument("--root", default=REPO, help="repo root (default: this file's repo)")
    ap.add_argument("--owners", default=None, help="owners table (default: <root>/tools/engine/owners.json)")
    ap.add_argument("--lane", help="lane id (default: from a lane/<id> branch)")
    ap.add_argument("--base-ref", help="integration branch (default rb-main, else main)")
    ap.add_argument("--head", default="HEAD", help="the lane tip to check (default HEAD)")
    ap.add_argument("--declared", help="PR_BODY.md whose 'Cross-lane edits' section declares edits outside the entry")
    ap.add_argument("--overlaps", action="store_true", help="report every path two lanes own")
    ap.add_argument("--check-plan", metavar="PLAN_JSON", help="check owners.json covers every lane of PLAN.json")
    args = ap.parse_args(argv)
    root = os.path.abspath(args.root)
    table = load_owners(args.owners or os.path.join(root, "tools", "engine", "owners.json"))
    if args.overlaps:
        return print_overlaps(table)
    if args.check_plan:
        return print_plan(table, args.check_plan)
    return run_gate(args, table, root)


if __name__ == "__main__":
    sys.exit(main())
