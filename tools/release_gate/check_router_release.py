#!/usr/bin/env python3
"""The router release gate: no provider ships on engine 2 while it rests on synthetic fixtures.

Router design section 11, "Release gate" (lane E2-R3b). For every slot tools/router/release_record.json
marks releasable, the gate fails while
  - a synthetic wire fixture is left for it: a file under test/support/fixtures/<prefix>_wire_*/
    whose name starts with "synthetic", whose folder ends in "_synthetic", or whose first line
    starts with "synthetic:" (prefix anthropic, openai, mistral, zai; custom counts for both custom
    slots; router for every slot whose rows allow the routed path), or
  - a request-shape feature its models.toml rows allow has no recorded 2xx in the record:
    structured_summary, replay_scope, image_in_tool_result, id_rule, routed_path, tool_choice_none,
unless the record waives that item with a reason. Every waiver is printed; a waiver that matches
nothing left, a citation to a repo path that does not exist, and a slot missing from the record
all fail. A slot not marked releasable is reported, never failed.

Usage: python3 tools/release_gate/check_router_release.py
Exit 0: every releasable slot is clear; 1: one is not (each reason listed).
"""

from __future__ import annotations

import json
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO / "tools" / "model_caps"))

import gen_caps  # noqa: E402

RECORD = REPO / "tools" / "router" / "release_record.json"
FIXTURES = REPO / "test" / "support" / "fixtures"
FEATURES = ("structured_summary", "replay_scope", "image_in_tool_result", "id_rule", "routed_path", "tool_choice_none")
PREFIX_SLOTS = {
    "anthropic": ("anthropic",),
    "openai": ("openai",),
    "mistral": ("mistral",),
    "zai": ("zai",),
    "custom": ("custom_chat", "custom_messages"),
}


def allowed_features(data: dict) -> "dict[str, set]":
    """Per slot, the request-shape features any of its rows allows (templates applied)."""
    templates = data.get("template", {})
    errs: list = []
    out = {s: {"id_rule"} for s in gen_caps.SLOTS}
    for p in data.get("provider", []):
        if p.get("slot") in out and (gen_caps.v(p, "routed_images_max", 0) or 0) > 0:
            out[p["slot"]].add("routed_path")
    for row in data.get("model", []):
        slot = row.get("slot")
        if slot not in out:
            continue
        r = gen_caps.resolve(row, templates, "row", errs)
        if gen_caps.v(r, "summarizer_structured", "none") != "none":
            out[slot].add("structured_summary")
        if gen_caps.v(r, "replay", "none") != "none":
            out[slot].add("replay_scope")
        if gen_caps.v(r, "image_in_tool_result", False):
            out[slot].add("image_in_tool_result")
        if "none" in (gen_caps.v(r, "tool_choice", []) or []):
            out[slot].add("tool_choice_none")
    return out


def is_synthetic(path: Path) -> bool:
    if path.name.startswith("synthetic") or path.parent.name.endswith("_synthetic"):
        return True
    try:
        with open(path, "rb") as fh:
            return fh.read(10) == b"synthetic:"
    except OSError:
        return False


def synthetic_fixtures(root: Path, allowed: dict) -> "dict[str, list]":
    """Per slot, the synthetic wire fixtures (repo-relative paths) that count against it."""
    out: dict = {s: [] for s in gen_caps.SLOTS}
    fixtures = root / "test" / "support" / "fixtures"
    if not fixtures.is_dir():
        return out
    for d in sorted(fixtures.iterdir()):
        if not d.is_dir() or "_wire" not in d.name:
            continue
        prefix = d.name.split("_wire", 1)[0]
        if prefix == "router":
            slots = tuple(s for s in gen_caps.SLOTS if "routed_path" in allowed.get(s, ()))
        else:
            slots = PREFIX_SLOTS.get(prefix, ())
        for f in sorted(p for p in d.rglob("*") if p.is_file()):
            if is_synthetic(f):
                rel = f.relative_to(root).as_posix()
                for s in slots:
                    out[s].append(rel)
    return out


def citation_ok(root: Path, cite: str) -> bool:
    """A repo path must exist; a 'P:' citation points outside the repo and is taken as written."""
    cite = (cite or "").strip()
    if not cite:
        return False
    if cite.startswith("P:"):
        return len(cite) > 2
    return (root / cite.split(" ", 1)[0].split(":", 1)[0]).exists()


def check_slot(slot: str, entry: dict, allowed: set, synthetic: list, root: Path) -> "tuple[list, list]":
    """(failures, waiver notes) for one slot of the record."""
    recorded, waived = entry.get("recorded", {}), entry.get("waived", {})
    fails, notes = [], []
    for feat, cite in recorded.items():
        if feat not in FEATURES:
            fails.append(f"{slot}: recorded feature '{feat}' is not one the gate knows {FEATURES}")
        elif not citation_ok(root, cite):
            fails.append(f"{slot}: recorded {feat} cites '{cite}', which does not exist")
    open_items = list(synthetic) + sorted(f for f in allowed if f not in recorded)
    for item, why in waived.items():
        if not (why or "").strip():
            fails.append(f"{slot}: waiver of {item} gives no reason")
        elif item not in open_items:
            fails.append(f"{slot}: stale waiver: {item} is no longer open (remove it)")
        else:
            notes.append(f"{slot}: waived {item}: {why}")
    if entry.get("releasable"):
        for item in open_items:
            if item in waived:
                continue
            if item in synthetic:
                fails.append(f"{slot}: releasable, but the synthetic fixture {item} is left")
            else:
                fails.append(f"{slot}: releasable, but its rows allow {item} and no recorded 2xx is entered")
    return fails, notes


def run(root: Path, record: dict, data: dict) -> "tuple[list, list]":
    """(failures, report lines) for the whole record."""
    allowed = allowed_features(data)
    synthetic = synthetic_fixtures(root, allowed)
    provs = record.get("providers", {})
    fails = [f"slot {s}: missing from the release record" for s in gen_caps.SLOTS if s not in provs]
    fails += [f"slot {s}: in the release record but not in models.toml" for s in provs if s not in gen_caps.SLOTS]
    report = []
    for slot in gen_caps.SLOTS:
        if slot not in provs:
            continue
        f, notes = check_slot(slot, provs[slot], allowed[slot], synthetic[slot], root)
        fails += f
        report += notes
        state = "releasable" if provs[slot].get("releasable") else "not releasable"
        unrecorded = sorted(x for x in allowed[slot] if x not in provs[slot].get("recorded", {}))
        report.append(
            f"{slot}: {state}; {len(synthetic[slot])} synthetic fixture(s); unrecorded: {unrecorded or 'none'}"
        )
    return fails, report


def main(argv=None) -> int:
    record = json.loads(RECORD.read_text(encoding="utf-8"))
    fails, report = run(REPO, record, gen_caps.load())
    for line in report:
        print("  " + line)
    for line in fails:
        print("FAIL " + line)
    print(f"check_router_release: {len(fails)} failure(s)")
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
