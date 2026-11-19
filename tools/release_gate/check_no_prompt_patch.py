#!/usr/bin/env python3
"""The no-prompt-patch gate (PRD section 11, the rule that stops prompt-patching).

Months of fixes each added prompt text or a guard, and each pushed more of the
conversation out of the request. This gate makes the rule mechanical over every
model-facing string the catalog generator extracts (gen_model_text_catalog.py):

1. Fresh: each catalog golden equals what its sources produce now.
2. Ceilings: every class is at or under its byte ceiling; an engine-1 ceiling is never
   above its baseline.
3. Ledger: a class whose text changed has an entry in coverage/PROMPT-CHANGES.md whose
   before state is the class's last accepted state and whose after state is the class
   now (bytes and sha8). Entries chain, so an old entry cannot cover a new change.
   - Growth fails, whatever the entry says, except an entry of kind build on a catalog in
     its build phase (engine 2 while it is being written to its spec).
   - kind swap (text added, at least as many bytes removed): needs the red scenario id
     (it resolves under tests/evals/**/scenarios), the commit where it was red, and the
     parity rates before and after.
   - kind remove: the class only shrank. With a git base, an added text fails it.
   - kind spec (ruling 18): planned work. It names a lane of the live plan and a PRD section,
     and may grow one class for one change (its ceiling rises by exactly that growth).
4. History (with a git base): entries and the baseline already merged are unchanged.
5. Register: a row of coverage/OWNER-ISSUES-REGISTER.md after OIR-55 names a scenario
   id that resolves and does not give a missing check as its root cause.

Usage (from the repo root):
  python3 tools/release_gate/check_no_prompt_patch.py [--base REF] [--no-base]
The base defaults to $NIMBUS_PROMPT_BASE, else the merge base with rb-main or main.
Exit 0 with NO PROMPT PATCH GREEN as the last line; exit 1 with NO PROMPT PATCH RED.
  python3 tools/release_gate/check_no_prompt_patch.py --release   # PROMPT TOTAL GREEN|RED
The release check (release_total_problems, for RB-R1) fails unless engine 1's total
model-facing bytes over all classes are lower than the baseline total.
"""

from __future__ import annotations

import argparse
import ast
import importlib.util
import json
import os
import re
import subprocess
import sys
from collections import Counter

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import gen_model_text_catalog as cat  # noqa: E402

REPO = cat.REPO
CHANGES = "coverage/PROMPT-CHANGES.md"
REGISTER = "coverage/OWNER-ISSUES-REGISTER.md"
SCENARIO_ROOT = "tests/evals"
EMPTY_SHA8 = cat.class_sha8([])
KINDS = ("swap", "remove", "build", "spec")
# The live plan: a spec entry names one of its lanes (ruling 18). Off this machine the plan's
# lanes are read from tools/engine/owners.json, which is built from it.
PLAN_JSON = os.environ.get(
    "RB_PLAN_JSON", "/Users/roydarnell/Projects/nimbus-project/program-2026-10-18/rebuild/PLAN.json"
)
# Lanes that wait outside PLAN.json (ruling 50): PLAN-pending.json and PLAN-future.json beside
# it (the RB-REC family lives in the future file). A spec entry may name one of them; a
# missing file adds nothing. Unset, each is read beside PLAN_JSON.
PLAN_PENDING_JSON = os.environ.get("RB_PLAN_PENDING_JSON")
PLAN_FUTURE_JSON = os.environ.get("RB_PLAN_FUTURE_JSON")
OWNERS = "tools/engine/owners.json"
# Rows up to this id predate the rule (18 Nov 2026); every later row needs a red scenario.
REGISTER_LAST_GRANDFATHERED = 55
MISSING_CHECK_PASSIVE = re.compile(
    r"\b(check|guard|test|assertion|validation|gate)s?\s+(was|were|is|are)\s+"
    r"(missing|absent|lacking)\b",
    re.I,
)
MISSING_CHECK = re.compile(
    r"\b(missing|no|lacked?|lacking|without( an?)?|absent)\s+(\w+\s+)?"
    r"(check|guard|test|assertion|validation|gate)s?\b",
    re.I,
)
BASE_REFS = ("rb-main", "origin/rb-main", "main", "origin/main")

_ENTRY_HEAD = re.compile(r"^### (PC-\d{4})\b")
_REBASE_HEAD = re.compile(r"^### (RBL-\d{4})\b")
_REBASE_ROW = re.compile(r"^- (e\d) (\w+): (\d+) ([0-9a-f]{8}) -> (\d+) ([0-9a-f]{8})\b")
_FIELD = re.compile(r"^- (\w+):\s*(.*?)\s*$")
_BASELINE_ROW = re.compile(r"^\| (e\d) \| (\w+) \| (\d+) \| ([0-9a-f]{8}) \|\s*$")
_PAIR_INT = re.compile(r"^(\d+)\s*->\s*(\d+)$")
_PAIR_SHA = re.compile(r"^([0-9a-f]{8})\s*->\s*([0-9a-f]{8})$")
_RATES = re.compile(r"^before=(0(?:\.\d+)?|1(?:\.0+)?)\s+after=(0(?:\.\d+)?|1(?:\.0+)?)\b")
_COMMIT = re.compile(r"^[0-9a-f]{7,40}$")


# ---------------------------------------------------------------------------
# PROMPT-CHANGES.md
# ---------------------------------------------------------------------------


def _baseline_line(out: dict, line: str) -> None:
    m = _BASELINE_ROW.match(line)
    if not m:
        return
    key = (m.group(1), m.group(2))
    if key in out["baseline"]:
        out["errors"].append(f"baseline: {key[0]} {key[1]} listed twice")
    out["baseline"][key] = (int(m.group(3)), m.group(4))


def _block_line(blocks: list, head: re.Pattern, line: str) -> None:
    """Fields of an entry or re-baseline record: '### ID' starts one, '- key: value' lines fill it."""
    m = head.match(line)
    if m:
        blocks.append({"id": m.group(1), "_text": [line], "rows": {}})
        return
    if not blocks:
        return
    cur = blocks[-1]
    cur["_text"].append(line)
    r = _REBASE_ROW.match(line)
    if r:
        cur["rows"][(r.group(1), r.group(2))] = ((int(r.group(3)), r.group(4)), (int(r.group(5)), r.group(6)))
    f = _FIELD.match(line)
    if f and f.group(1) not in cur:
        cur[f.group(1)] = f.group(2)


def parse_changes(text: str) -> dict:
    """{"baseline": {(catalog, class): (bytes, sha8)}, "entries": [dict], "rebaselines": [dict], "errors": [str]}.

    The baseline table always holds the current baseline; each re-baseline record states what
    moved (old -> new per class) and which entries it supersedes."""
    out = {"baseline": {}, "entries": [], "rebaselines": [], "errors": []}
    section = ""
    for raw in text.splitlines():
        line = raw.rstrip()
        if line.startswith("## "):
            section = line[3:].strip().lower()
        elif section.startswith("baseline"):
            _baseline_line(out, line)
        elif section.startswith("re-baselines"):
            _block_line(out["rebaselines"], _REBASE_HEAD, line)
        elif section.startswith("changes"):
            _block_line(out["entries"], _ENTRY_HEAD, line)
    ids = Counter(e["id"] for e in out["entries"])
    out["errors"] += [f"{i}: entry id used {n} times" for i, n in ids.items() if n > 1]
    for e in out["entries"] + out["rebaselines"]:
        e["_text"] = "\n".join(e["_text"]).strip()
    return out


SCENARIO_EXT = (".json", ".yaml", ".yml")


def scenario_exists(root: str, sid: str) -> bool:
    """A scenario id resolves when a .json or .yaml file in a scenarios folder under tests/evals declares it."""
    if not re.fullmatch(r"[a-z0-9][a-z0-9._-]{1,80}", sid):
        return False
    decl = re.compile(r'(?:^|[{,])\s*"?id"?\s*:\s*"?' + re.escape(sid) + r'"?\s*(?:,|\}|$)', re.M)
    top = os.path.join(root, SCENARIO_ROOT)
    for dirpath, dirnames, files in os.walk(top):
        dirnames.sort()
        if "scenarios" not in os.path.relpath(dirpath, top).replace(os.sep, "/").split("/"):
            continue
        for f in files:
            if not f.endswith(SCENARIO_EXT):
                continue
            try:
                with open(os.path.join(dirpath, f), encoding="utf-8", errors="replace") as fh:
                    if decl.search(fh.read(4096)):
                        return True
            except OSError:
                continue
    return False


def _first_word(value: str) -> str:
    return value.split()[0] if value.split() else ""


def _identity_errors(e: dict) -> "list[str]":
    eid, catalog, cls, kind = e["id"], e.get("catalog", ""), e.get("class", ""), e.get("kind", "")
    errs = []
    if catalog not in cat.CATALOGS:
        errs.append(f"{eid}: catalog must be one of {', '.join(cat.CATALOGS)} (got {catalog!r})")
    elif cls not in cat.CATALOGS[catalog]["classes"]:
        errs.append(f"{eid}: class {cls!r} is not a class of {catalog}")
    if kind not in KINDS:
        errs.append(f"{eid}: kind must be one of {', '.join(KINDS)} (got {kind!r})")
    for field in ("lane", "why"):
        if not e.get(field):
            errs.append(f"{eid}: {field} is required")
    return errs


def _lanes_of(path: str) -> "set[str] | None":
    """The lane ids of one plan file, or None when it is missing or unreadable."""
    try:
        with open(path, encoding="utf-8") as f:
            plan = json.load(f)
    except (OSError, ValueError):
        return None
    return {lane["id"] for lane in plan.get("lanes", []) if "id" in lane}


def plan_lanes(root: str) -> "set[str]":
    """Lane ids of the live plan plus the pending and future plans beside it (off this machine,
    as tools/engine/owners.json lists them)."""
    lanes = _lanes_of(PLAN_JSON)
    if lanes is None:  # GitHub's checkout has no plan file: owners.json is built from it
        owners = json.loads(cat._read(root, OWNERS) or "{}")
        return set(owners.get("lanes", {}))
    beside = os.path.dirname(PLAN_JSON)
    for waiting, name in ((PLAN_PENDING_JSON, "PLAN-pending.json"), (PLAN_FUTURE_JSON, "PLAN-future.json")):
        lanes |= _lanes_of(waiting or os.path.join(beside, name)) or set()
    return lanes


def _proof_errors(root: str, e: dict) -> "list[str]":
    """A swap's red-scenario-first proof; a build or spec entry's PRD section; a spec entry's planned lane."""
    eid, kind = e["id"], e.get("kind", "")
    errs = []
    if kind in ("build", "spec") and not re.search(r"\bsection \d+", e.get("prd", "")):
        errs.append(f"{eid}: a {kind} entry names the PRD section it implements (prd: section N)")
    if kind == "spec" and _first_word(e.get("lane", "")) not in plan_lanes(root):
        errs.append(f"{eid}: a spec entry names a lane of the live plan ({e.get('lane', '')!r} is not in it)")
    if kind != "swap":
        return errs
    sid = _first_word(e.get("scenario", ""))
    if not sid:
        errs.append(f"{eid}: a swap needs the red scenario id (scenario:)")
    elif not scenario_exists(root, sid):
        errs.append(f"{eid}: scenario {sid!r} does not resolve under {SCENARIO_ROOT}/**/scenarios")
    if not _COMMIT.match(_first_word(e.get("red_at", ""))):
        errs.append(f"{eid}: a swap needs red_at: the commit where its scenario was red")
    rates = _RATES.match(e.get("rates", ""))
    if not rates:
        errs.append(f"{eid}: a swap needs the parity rates: 'rates: before=<0..1> after=<0..1>'")
    elif float(rates.group(2)) < float(rates.group(1)):
        errs.append(f"{eid}: the parity rate fell ({rates.group(1)} -> {rates.group(2)}); a swap may not lower it")
    return errs


def _size_errors(e: dict, b0: int, b1: int) -> "list[str]":
    eid, catalog, cls, kind = e["id"], e["catalog"], e["class"], e["kind"]
    if kind == "spec":
        return []  # planned work may grow one class for one change (ruling 18)
    if kind == "build":
        phase = cat.CATALOGS[catalog]["phase"]
        return [] if phase == "build" else [f"{eid}: kind build is refused: catalog {catalog} is {phase}"]
    if b1 > b0:
        return [
            f"{eid}: {catalog} {cls} grows {b0} -> {b1} B (+{b1 - b0}); growth fails unless equal bytes are "
            "removed in the same class"
        ]
    if kind == "remove" and b1 == b0:
        return [f"{eid}: kind remove must shrink the class ({b0} -> {b1})"]
    return []


def check_entry(root: str, e: dict) -> "tuple[list, tuple | None]":
    """Field problems of one entry, and its parsed transition (catalog, class, b0, b1, s0, s1)."""
    errs = _identity_errors(e) + _proof_errors(root, e)
    mb, ms = _PAIR_INT.match(e.get("bytes", "")), _PAIR_SHA.match(e.get("sha8", ""))
    if not mb:
        errs.append(f"{e['id']}: bytes must read '<before> -> <after>'")
    if not ms:
        errs.append(f"{e['id']}: sha8 must read '<before> -> <after>' (8 hex each)")
    if errs:
        return errs, None
    b0, b1 = int(mb.group(1)), int(mb.group(2))
    errs = _size_errors(e, b0, b1)
    return errs, (e["catalog"], e["class"], b0, b1, ms.group(1), ms.group(2))


def live_entries(changes: dict) -> list:
    """The entries the chain still applies: every engine-2 entry, and the engine-1 entries after the
    latest re-baseline's 'supersedes' id. A re-baseline re-measures engine 1 only (its table has no e2
    rows), so it never supersedes an engine-2 entry (lane RB-SYNC: RBL-0002 was the first record
    made with e2 build entries in the ledger)."""
    if not changes["rebaselines"]:
        return list(changes["entries"])
    last = _first_word(changes["rebaselines"][-1].get("supersedes", ""))
    ids = [e["id"] for e in changes["entries"]]
    if last in ("", "none") or last not in ids:
        return list(changes["entries"])
    cut = ids.index(last) + 1
    return [e for i, e in enumerate(changes["entries"]) if i >= cut or e.get("catalog") != "e1"]


def rebaseline_errors(changes: dict) -> "list[str]":
    """Each record is well formed; the latest one ends at the baseline table, and records chain."""
    errs = []
    ids = {e["id"] for e in changes["entries"]}
    prev = None
    for r in changes["rebaselines"]:
        sup = _first_word(r.get("supersedes", ""))
        if not r["rows"] or not r.get("why") or not r.get("base") or (sup not in ("none",) and sup not in ids):
            errs.append(
                f"{r['id']}: a re-baseline names its base, the entry it supersedes (or none), every class "
                "old -> new, and why"
            )
        if prev is not None and any(r["rows"].get(k, (None,))[0] != v[1] for k, v in prev["rows"].items()):
            errs.append(f"{r['id']}: its old figures are not where {prev['id']} ended")
        prev = r
    if prev is not None and {k: v[1] for k, v in prev["rows"].items()} != changes["baseline"]:
        errs.append(f"{prev['id']}: the latest re-baseline does not end at the baseline table")
    return errs


def check_ledger(root: str, changes: dict, current: dict) -> "list[str]":
    """current: {catalog: {class: {bytes, sha8, ...}}} of the live catalogs."""
    errs = list(changes["errors"])
    state = {k: v for k, v in changes["baseline"].items()}
    errs += rebaseline_errors(changes)
    allowance = spec_allowance(root, changes)
    for (catalog, cls), (b, _s) in changes["baseline"].items():
        if catalog not in cat.CATALOGS or cls not in cat.CATALOGS[catalog]["classes"]:
            errs.append(f"baseline: {catalog} {cls} is not a catalog class")
    for e in live_entries(changes):
        problems, tr = check_entry(root, e)
        errs += problems
        if tr is None:
            continue
        catalog, cls, b0, b1, s0, s1 = tr
        prev = state.get((catalog, cls), (0, EMPTY_SHA8))
        if (b0, s0) != prev:
            errs.append(
                f"{e['id']}: before state {b0} B {s0} is not the last accepted state of {catalog} {cls} "
                f"({prev[0]} B {prev[1]}); entries chain in file order"
            )
        ceiling = cat.ceilings(catalog).get(cls, 0) + allowance.get((catalog, cls), 0)
        if b1 > ceiling:
            errs.append(f"{e['id']}: {catalog} {cls} after {b1} B is over its ceiling {ceiling} B")
        state[(catalog, cls)] = (b1, s1)
    for catalog, classes in current.items():
        for cls, st in classes.items():
            want = state.get((catalog, cls), (0, EMPTY_SHA8))
            if (st["bytes"], st["sha8"]) != want:
                errs.append(
                    f"{catalog} {cls}: model-facing text changed ({want[0]} B {want[1]} -> {st['bytes']} B "
                    f"{st['sha8']}) with no entry in {CHANGES}; a fix changes a tool, the transcript or "
                    "the model, not the text (PRD section 11)"
                )
    return errs


def spec_allowance(root: str, changes: dict) -> dict:
    """{(catalog, class): bytes} the well-formed live spec entries grew a class by: its ceiling rises by
    that much. A spec entry a re-baseline superseded is in the baseline already and raises nothing."""
    out: dict = {}
    for e in live_entries(changes):
        if e.get("kind") != "spec":
            continue
        problems, tr = check_entry(root, e)
        if problems or tr is None:
            continue
        catalog, cls, b0, b1 = tr[:4]
        out[(catalog, cls)] = out.get((catalog, cls), 0) + max(0, b1 - b0)
    return out


def check_ceilings(current: dict, baseline: dict, allowance: "dict | None" = None) -> "list[str]":
    errs = []
    allowance = allowance or {}
    for catalog, classes in current.items():
        ceil = cat.ceilings(catalog)
        for cls, st in classes.items():
            limit = ceil.get(cls, 0) + allowance.get((catalog, cls), 0)
            if cls not in ceil:
                errs.append(f"{catalog} {cls}: no byte ceiling")
            elif st["bytes"] > limit:
                errs.append(f"{catalog} {cls}: {st['bytes']} B is over its ceiling {limit} B")
    e1_rows = {cls: row for (catalog, cls), row in baseline.items() if catalog == "e1"}
    if e1_rows != cat.E1_BASELINE:
        errs.append(
            f"{CHANGES}: the e1 baseline table differs from E1_BASELINE in gen_model_text_catalog.py; "
            "both are written once"
        )
    for cls, (size, _sha) in cat.E1_BASELINE.items():
        if cat.ceilings("e1").get(cls, 0) > size:
            errs.append(
                f"e1 {cls}: ceiling {cat.ceilings('e1')[cls]} B is above its baseline {size} B (engine 1 only shrinks)"
            )
    return errs


# ---------------------------------------------------------------------------
# The register (format: a red scenario per row, no 'missing check' root cause)
# ---------------------------------------------------------------------------

_ROW = re.compile(r"^\| (OIR-(\d+)) \|")


def check_register(root: str, text: "str | None" = None) -> "list[str]":
    if text is None:
        text = cat._read(root, REGISTER) or ""
    errs = []
    for line in text.splitlines():
        m = _ROW.match(line)
        if not m or int(m.group(2)) <= REGISTER_LAST_GRANDFATHERED:
            continue
        cells = [c.strip() for c in line.split("|")[1:-1]]
        rid = m.group(1)
        if len(cells) < 8:
            errs.append(f"{rid}: a register row has 8 columns")
            continue
        ids = re.findall(r"\b[a-z0-9][a-z0-9._-]+\b", cells[7])
        if not any(scenario_exists(root, i) for i in ids):
            errs.append(
                f"{rid}: the live case column names no scenario that resolves under {SCENARIO_ROOT}/**/"
                "scenarios; every owner-visible failure gets a red multi-turn scenario before its fix"
            )
        if MISSING_CHECK.search(cells[4]) or MISSING_CHECK_PASSIVE.search(cells[4]):
            errs.append(
                f"{rid}: 'missing check' is not an accepted root cause (say why the product did the "
                f"wrong thing): {cells[4][:80]!r}"
            )
    return errs


# ---------------------------------------------------------------------------
# History: what is merged stays as it was
# ---------------------------------------------------------------------------


def git(root: str, *args: str) -> "str | None":
    try:
        r = subprocess.run(["git", "-C", root, *args], capture_output=True, text=True, timeout=60)
    except (OSError, subprocess.TimeoutExpired):
        return None
    return r.stdout if r.returncode == 0 else None


def resolve_base(root: str, arg: "str | None") -> "str | None":
    ref = arg or os.environ.get("NIMBUS_PROMPT_BASE")
    if ref:
        if ref.startswith("-"):
            return None
        out = git(root, "rev-parse", "--verify", "--end-of-options", ref + "^{commit}")
        return out.strip() if out else None
    for cand in BASE_REFS:
        out = git(root, "merge-base", "HEAD", cand)
        if out:
            return out.strip()
    return None


def show(root: str, base: str, rel: str) -> "str | None":
    return git(root, "show", f"{base}:{rel}")


def _merged_entry_errors(changes: dict, old: dict) -> "list[str]":
    errs = []
    old_rbl = {r["id"]: r["_text"] for r in old.get("rebaselines", [])}
    new_rbl = [r for r in changes["rebaselines"] if r["id"] not in old_rbl]
    if old["baseline"] != changes["baseline"]:
        # Only a re-baseline record added in this change, starting at the base's baseline, may move it.
        starts = new_rbl and {k: v[0] for k, v in new_rbl[0]["rows"].items()} == old["baseline"]
        if not starts:
            errs.append(
                f"{CHANGES}: the baseline differs from the base commit's without a re-baseline record that "
                "starts there (gen_model_text_catalog.py --rebaseline)"
            )
    now_rbl = {r["id"]: r["_text"] for r in changes["rebaselines"]}
    for rid, text in old_rbl.items():
        if now_rbl.get(rid) != text:
            errs.append(f"{rid}: a merged re-baseline was edited or removed")
    now = {e["id"]: e["_text"] for e in changes["entries"]}
    for e in old["entries"]:
        if now.get(e["id"]) != e["_text"]:
            errs.append(f"{e['id']}: a merged entry was edited or removed; add a new entry instead")
    return errs


def _added_texts(root: str, base: str, catalog: str, cls: str) -> Counter:
    old_golden = show(root, base, cat.CATALOGS[catalog]["golden"])
    new_golden = cat._read(root, cat.CATALOGS[catalog]["golden"])
    if old_golden is None or new_golden is None:
        return Counter()
    was = Counter(x.text for x in cat.parse(old_golden)["entries"] if x.cls == cls)
    now = Counter(x.text for x in cat.parse(new_golden)["entries"] if x.cls == cls)
    return now - was


def _base_e1_sources(root: str, base: str) -> "set[str] | None":
    """The E1_SOURCES paths of the base commit's generator, read without running it."""
    src = show(root, base, "tools/release_gate/gen_model_text_catalog.py")
    if src is None:
        return None
    for node in ast.parse(src).body:
        if isinstance(node, ast.Assign) and any(getattr(t, "id", "") == "E1_SOURCES" for t in node.targets):
            return {p for paths in ast.literal_eval(node.value).values() for p in paths}
    return None


def check_sources_history(root: str, base: str) -> "list[str]":
    """No engine-1 source leaves the list while it exists; a new file's prose is catalogued or declared."""
    errs = []
    was = _base_e1_sources(root, base)
    now = cat.catalogued_paths()
    for rel in sorted((was or set()) - now):
        if os.path.isfile(os.path.join(root, rel)):
            errs.append(
                f"{rel}: dropped from E1_SOURCES while it still exists; its text would leave the catalog unseen"
            )
    added = (git(root, "diff", "--name-only", "--diff-filter=A", base, "--", *cat.SCAN_DIRS) or "").split()
    added += (git(root, "ls-files", "--others", "--exclude-standard", "--", *cat.SCAN_DIRS) or "").split()
    for rel in sorted(set(added)):
        if not rel.endswith((".cpp", ".h", ".hpp", ".cc")) or rel in now or rel in cat.NON_MODEL_FILES:
            continue
        if cat._is_e2_engine_file(rel) or not os.path.isfile(os.path.join(root, rel)):
            continue
        lits = cat.model_literals(rel, cat._read(root, rel) or "")
        if lits:
            errs.append(
                f"{rel}: a new file with prose literals outside a log call ({lits[0].text[:50]!r}); list it in "
                "E1_SOURCES, move the text to texts.h, or name it in NON_MODEL_FILES with the reason"
            )
    return errs


def _red_at_errors(root: str, entries: list) -> "list[str]":
    errs = []
    for e in entries:
        sha = _first_word(e.get("red_at", ""))
        if e.get("kind") == "swap" and _COMMIT.match(sha) and git(root, "cat-file", "-e", sha + "^{commit}") is None:
            errs.append(f"{e['id']}: red_at {sha} is not a commit in this repository")
    return errs


def check_history(root: str, base: str, changes: dict) -> "list[str]":
    """Merged entries and the baseline are unchanged; a remove entry hides no added text."""
    old_text = show(root, base, CHANGES)
    old = parse_changes(old_text) if old_text is not None else {"baseline": changes["baseline"], "entries": []}
    errs = _merged_entry_errors(changes, old) + check_sources_history(root, base)
    old_ids = {e["id"] for e in old["entries"]}
    errs += _red_at_errors(root, [e for e in changes["entries"] if e["id"] not in old_ids])
    kinds: dict = {}
    for e in changes["entries"]:
        if e["id"] not in old_ids:
            kinds.setdefault((e.get("catalog"), e.get("class")), set()).add(e.get("kind"))
    for (catalog, cls), ks in kinds.items():
        if ks != {"remove"} or catalog not in cat.CATALOGS:
            continue
        added = _added_texts(root, base, catalog, cls)
        if added:
            size = sum(len(t.encode("utf-8")) for t in added.elements())
            errs.append(
                f"{catalog} {cls}: logged as remove but text was added ({size} B, e.g. "
                f"{next(iter(added))[:70]!r}); log a swap with its red scenario and rates"
            )
    return errs


# ---------------------------------------------------------------------------


def current_stats(root: str) -> "tuple[dict, list]":
    """({catalog: class stats} of the live catalogs, freshness problems)."""
    stats, errs = {}, []
    built = cat.build_all(root)
    for name, entries in built.items():
        golden_rel = cat.CATALOGS[name]["golden"]
        golden = cat._read(root, golden_rel)
        if entries is None:
            if golden is not None:
                errs.append(f"{golden_rel} exists but the {name} sources do not")
            continue
        stats[name] = cat.class_stats(name, entries)
        if golden != cat.render(name, entries):
            errs.append(
                f"{golden_rel} is stale: regenerate with gen_model_text_catalog.py --write and log the "
                f"change in {CHANGES}"
            )
    return stats, errs


def run(root: str, base: "str | None", use_base: bool = True, quiet: bool = False) -> "list[str]":
    try:
        stats, errs = current_stats(root)
    except cat.CatalogError as e:
        return [str(e)]
    changes = parse_changes(cat._read(root, CHANGES) or "")
    if not changes["baseline"]:
        errs.append(f"{CHANGES}: no baseline table")
    errs += check_ceilings(stats, changes["baseline"], spec_allowance(root, changes))
    errs += check_ledger(root, changes, stats)
    errs += check_register(root)
    if use_base:
        if base:
            errs += check_history(root, base, changes)
        elif not os.environ.get("GITHUB_ACTIONS"):
            errs.append("no git base found (pass --base REF, or --no-base to skip the history checks)")
        elif not quiet:
            print("history checks skipped: no git base in this checkout")
    if not quiet:
        for name, classes in stats.items():
            for cls, st in classes.items():
                print(
                    f"  {name} {cls:18} {st['bytes']:7d} B  ceiling {cat.ceilings(name).get(cls, 0):7d} B  "
                    f"sha8 {st['sha8']}"
                )
    return errs


# The release reference (ruling 43): the last RELEASED firmware's engine-1 model-facing bytes,
# all classes together. v5.1.2 is main a01ef7fb, which had no generator of its own; the figure
# is the generator of rb-main 4e7f4ac27 (the last one whose list still names every v5.1.2
# source) over a clean checkout of a01ef7fb:
#   check_no_prompt_patch.py --measure-release-reference <a01ef7fb tree> --generator <its copy>
# (sources the rebuild added since, such as orch_truth.cpp, did not exist then and count 0).
# A release must come in strictly below it. A re-baseline of rb-main never moves it.
RELEASE_REFERENCE = {
    "release": "v5.1.2",
    "sha": "a01ef7fb99eb7ecf01f6f51c60ef628b695f775c",
    "generator": "rb-main 4e7f4ac27",
    "bytes": 197695,
}


def tree_e1_total(root: str, gen=None) -> "tuple[int, list[str]]":
    """Engine 1's model-facing bytes of a tree, all classes, by a generator's source list (this
    one by default); a listed source the tree does not hold counts 0 and is named, and a tool
    file the list misses is an error (the figure would be short)."""
    gen = gen or cat
    total, missing = 0, []
    for cls, rel in gen.e1_sources():
        src = gen._read(root, rel)
        if src is None:
            missing.append(rel)
            continue
        total += sum(e.size for e in gen._e1_file_entries(cls, rel, src))
    stray = gen.uncatalogued_tool_files(root)
    if stray:
        raise gen.CatalogError("a tool file is in no catalog class of that generator: " + ", ".join(stray))
    return total, missing


def load_generator(path: str):
    """A generator module from a file (an older copy, for a released tree)."""
    spec = importlib.util.spec_from_file_location("gen_model_text_catalog_ref", path)
    mod = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = mod  # dataclasses look their module up while it loads
    spec.loader.exec_module(mod)
    return mod


def release_total_problems(root: str, reference: "dict | None" = None) -> "list[str]":
    """The release check (ruling 43, for RB-R1): engine 1's model-facing bytes, all classes
    together, are strictly lower than the last released firmware's (RELEASE_REFERENCE)."""
    ref = reference or RELEASE_REFERENCE
    try:
        entries = cat.build_e1(root)
    except cat.CatalogError as e:
        return [str(e)]
    total = sum(e.size for e in entries)
    if total < ref["bytes"]:
        return []
    return [
        f"engine 1 holds {total} B of model-facing text, not lower than {ref['release']}'s {ref['bytes']} B "
        f"(main {ref['sha'][:9]}; +{total - ref['bytes']} B); the release's deletions must outweigh its additions"
    ]


def main(argv: "list[str] | None" = None) -> int:
    ap = argparse.ArgumentParser(description="No-prompt-patch gate (PRD section 11)")
    ap.add_argument("--root", default=REPO)
    ap.add_argument("--base", help="git ref the branch is compared with (default: merge base with rb-main or main)")
    ap.add_argument("--no-base", action="store_true", help="skip the checks that need git history")
    ap.add_argument("--quiet", action="store_true")
    ap.add_argument(
        "--release",
        action="store_true",
        help="the release check only: engine 1's total below the last release's (ruling 43)",
    )
    ap.add_argument(
        "--measure-release-reference",
        metavar="TREE",
        help="print engine 1's total over a clean checkout of a released sha (the reference's source)",
    )
    ap.add_argument("--generator", help="with --measure-release-reference: the generator file to use")
    args = ap.parse_args(argv)
    if args.measure_release_reference:
        gen = load_generator(args.generator) if args.generator else None
        total, missing = tree_e1_total(args.measure_release_reference, gen)
        print(f"engine 1 total {total} B; listed sources not in that tree: {', '.join(missing) or 'none'}")
        return 0
    if args.release:
        errs = release_total_problems(args.root)
        for e in errs:
            print(f"no-prompt-patch release: {e}")
        total = sum(e.size for e in cat.build_e1(args.root)) if not errs else 0
        ref = RELEASE_REFERENCE
        print(
            "PROMPT TOTAL RED"
            if errs
            else f"PROMPT TOTAL GREEN ({total} B < {ref['release']} {ref['bytes']} B, main {ref['sha'][:9]})"
        )
        return 1 if errs else 0
    base = None if args.no_base else resolve_base(args.root, args.base)
    if args.base and not base:
        print(f"no-prompt-patch: base {args.base!r} does not resolve")
        print("NO PROMPT PATCH RED")
        return 1
    errs = run(args.root, base, use_base=not args.no_base, quiet=args.quiet)
    for e in errs:
        print(f"no-prompt-patch: {e}")
    print("NO PROMPT PATCH RED" if errs else f"NO PROMPT PATCH GREEN (base {base[:10] if base else 'none'})")
    return 1 if errs else 0


if __name__ == "__main__":
    sys.exit(main())
