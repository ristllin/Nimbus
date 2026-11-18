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
4. History (with a git base): entries and the baseline already merged are unchanged.
5. Register: a row of coverage/OWNER-ISSUES-REGISTER.md after OIR-55 names a scenario
   id that resolves and does not give a missing check as its root cause.

Usage (from the repo root):
  python3 tools/release_gate/check_no_prompt_patch.py [--base REF] [--no-base]
The base defaults to $NIMBUS_PROMPT_BASE, else the merge base with rb-main or main.
Exit 0 with NO PROMPT PATCH GREEN as the last line; exit 1 with NO PROMPT PATCH RED.
"""

from __future__ import annotations

import argparse
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
KINDS = ("swap", "remove", "build")
# Rows up to this id predate the rule (18 Nov 2026); every later row needs a red scenario.
REGISTER_LAST_GRANDFATHERED = 55
MISSING_CHECK = re.compile(
    r"\b(missing|no|lacked?|lacking|without( an?)?|absent)\s+(\w+\s+)?"
    r"(check|guard|test|assertion|validation|gate)s?\b",
    re.I,
)
BASE_REFS = ("rb-main", "origin/rb-main", "main", "origin/main")

_ENTRY_HEAD = re.compile(r"^### (PC-\d{4})\b")
_FIELD = re.compile(r"^- (\w+):\s*(.*?)\s*$")
_BASELINE_ROW = re.compile(r"^\| (e\d) \| (\w+) \| (\d+) \| ([0-9a-f]{8}) \|\s*$")
_PAIR_INT = re.compile(r"^(\d+)\s*->\s*(\d+)$")
_PAIR_SHA = re.compile(r"^([0-9a-f]{8})\s*->\s*([0-9a-f]{8})$")
_RATES = re.compile(r"^before=(0(?:\.\d+)?|1(?:\.0+)?)\s+after=(0(?:\.\d+)?|1(?:\.0+)?)\b")
_COMMIT = re.compile(r"^[0-9a-f]{7,40}$")


# ---------------------------------------------------------------------------
# PROMPT-CHANGES.md
# ---------------------------------------------------------------------------


def parse_changes(text: str) -> dict:
    """{"baseline": {(catalog, class): (bytes, sha8)}, "entries": [dict], "errors": [str]}."""
    baseline, entries, errors = {}, [], []
    section = ""
    cur = None
    for raw in text.splitlines():
        line = raw.rstrip()
        if line.startswith("## "):
            section = line[3:].strip().lower()
            cur = None
            continue
        if section.startswith("baseline"):
            m = _BASELINE_ROW.match(line)
            if m:
                key = (m.group(1), m.group(2))
                if key in baseline:
                    errors.append(f"baseline: {key[0]} {key[1]} listed twice")
                baseline[key] = (int(m.group(3)), m.group(4))
            continue
        if not section.startswith("changes"):
            continue
        m = _ENTRY_HEAD.match(line)
        if m:
            cur = {"id": m.group(1), "_text": [line]}
            entries.append(cur)
            continue
        if cur is None:
            continue
        cur["_text"].append(line)
        m = _FIELD.match(line)
        if m and m.group(1) not in cur:
            cur[m.group(1)] = m.group(2)
    ids = Counter(e["id"] for e in entries)
    errors += [f"{i}: entry id used {n} times" for i, n in ids.items() if n > 1]
    for e in entries:
        e["_text"] = "\n".join(e["_text"]).strip()
    return {"baseline": baseline, "entries": entries, "errors": errors}


def scenario_exists(root: str, sid: str) -> bool:
    """A scenario id resolves when a file under tests/evals/**/scenarios is named for it or declares it."""
    if not re.fullmatch(r"[a-z0-9][a-z0-9._-]{1,80}", sid):
        return False
    decl = re.compile(r"^\s*\"?id\"?\s*:\s*\"?" + re.escape(sid) + r"\"?\s*,?\s*$", re.M)
    for dirpath, dirnames, files in os.walk(os.path.join(root, SCENARIO_ROOT)):
        dirnames.sort()
        if "scenarios" not in dirpath.replace(os.sep, "/").split("/"):
            continue
        for f in files:
            stem = os.path.splitext(f)[0]
            if stem == sid or stem.split("-", 1)[-1] == sid and stem.split("-", 1)[0].isdigit():
                return True
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


def _proof_errors(root: str, e: dict) -> "list[str]":
    """A swap's red-scenario-first proof; a build entry's PRD section."""
    eid, kind = e["id"], e.get("kind", "")
    errs = []
    if kind == "build" and not re.search(r"\bsection \d+", e.get("prd", "")):
        errs.append(f"{eid}: a build entry names the PRD section it implements (prd: section N)")
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


def check_ledger(root: str, changes: dict, current: dict) -> "list[str]":
    """current: {catalog: {class: {bytes, sha8, ...}}} of the live catalogs."""
    errs = list(changes["errors"])
    state = {k: v for k, v in changes["baseline"].items()}
    for (catalog, cls), (b, _s) in changes["baseline"].items():
        if catalog not in cat.CATALOGS or cls not in cat.CATALOGS[catalog]["classes"]:
            errs.append(f"baseline: {catalog} {cls} is not a catalog class")
    for e in changes["entries"]:
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
        if b1 > cat.ceilings(catalog).get(cls, 0):
            errs.append(
                f"{e['id']}: {catalog} {cls} after {b1} B is over its ceiling {cat.ceilings(catalog).get(cls, 0)} B"
            )
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


def check_ceilings(current: dict, baseline: dict) -> "list[str]":
    errs = []
    for catalog, classes in current.items():
        ceil = cat.ceilings(catalog)
        for cls, st in classes.items():
            if cls not in ceil:
                errs.append(f"{catalog} {cls}: no byte ceiling")
            elif st["bytes"] > ceil[cls]:
                errs.append(f"{catalog} {cls}: {st['bytes']} B is over its ceiling {ceil[cls]} B")
    for (catalog, cls), (b, _s) in baseline.items():
        if catalog == "e1" and cat.ceilings("e1").get(cls, 0) > b:
            errs.append(
                f"e1 {cls}: ceiling {cat.ceilings('e1')[cls]} B is above its baseline {b} B (engine 1 only shrinks)"
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
        if MISSING_CHECK.search(cells[4]):
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
    if old["baseline"] != changes["baseline"]:
        errs.append(f"{CHANGES}: the baseline differs from the base commit's; it is written once")
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


def check_history(root: str, base: str, changes: dict) -> "list[str]":
    """Merged entries and the baseline are unchanged; a remove entry hides no added text."""
    old_text = show(root, base, CHANGES)
    old = parse_changes(old_text) if old_text is not None else {"baseline": changes["baseline"], "entries": []}
    errs = _merged_entry_errors(changes, old)
    old_ids = {e["id"] for e in old["entries"]}
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
    errs += check_ceilings(stats, changes["baseline"])
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


def main(argv: "list[str] | None" = None) -> int:
    ap = argparse.ArgumentParser(description="No-prompt-patch gate (PRD section 11)")
    ap.add_argument("--root", default=REPO)
    ap.add_argument("--base", help="git ref the branch is compared with (default: merge base with rb-main or main)")
    ap.add_argument("--no-base", action="store_true", help="skip the checks that need git history")
    ap.add_argument("--quiet", action="store_true")
    args = ap.parse_args(argv)
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
