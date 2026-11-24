#!/usr/bin/env python3
"""The router release gate: no provider route ships on engine 2 while it rests on synthetic fixtures.

Router design section 11, "Release gate" (lanes E2-R3b, E2-R3c; ruling 125 item 2). The record,
tools/router/release_record.json, judges each slot per route: "direct", and "routed" (the Cumulo
hosted router) for a slot whose provider row allows it. For every route it marks releasable, the
gate fails while
  - a synthetic wire fixture is left for it: a file under test/support/fixtures/<prefix>_wire_*/
    whose name starts with "synthetic", whose path has a folder ending in "_synthetic", or whose
    first line starts with "synthetic:". A provider prefix (anthropic, openai, mistral, zai;
    custom for both custom slots) counts for that slot's routes; router counts only for routed
    routes; or
  - a request-shape feature the slot's models.toml rows allow has no recorded 2xx in the record:
    structured_summary, replay_scope, image_in_tool_result, id_rule, tool_choice_none, and
    routed_path on the routed route,
unless the route waives the item. A waiver is {"why": ..., "until": ...} (the step that ends it,
e.g. "R10") and names one item exactly, or a class: a folder ending in "/" or a glob (* ? [).
Every waived item is printed with its class, so a NEW file in a waived class is visible without
failing. An exact waiver that matches nothing, a waiver without why or until, a citation to a repo
path that does not exist, and a slot or route missing from the record all fail. A route not marked
releasable is reported, never failed.

Usage: python3 tools/release_gate/check_router_release.py
Exit 0: every releasable route is clear; 1: one is not (each reason listed).
"""

from __future__ import annotations

import fnmatch
import json
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO / "tools" / "model_caps"))

import gen_caps  # noqa: E402

RECORD = REPO / "tools" / "router" / "release_record.json"
FEATURES = ("structured_summary", "replay_scope", "image_in_tool_result", "id_rule", "routed_path", "tool_choice_none")
PREFIX_SLOTS = {
    "anthropic": ("anthropic",),
    "openai": ("openai",),
    "mistral": ("mistral",),
    "zai": ("zai",),
    "custom": ("custom_chat", "custom_messages"),
}


def routes_of(data: dict) -> "dict[str, tuple]":
    """Per slot, its routes: direct, plus routed where the provider row allows the hosted router."""
    out = {s: ("direct",) for s in gen_caps.SLOTS}
    for p in data.get("provider", []):
        if p.get("slot") in out and (gen_caps.v(p, "routed_images_max", 0) or 0) > 0:
            out[p["slot"]] = ("direct", "routed")
    return out


def allowed_features(data: dict) -> "dict[str, set]":
    """Per slot, the request-shape features any of its rows allows (templates applied); routed_path
    when the slot has a routed route."""
    templates = data.get("template", {})
    errs: list = []
    out = {s: {"id_rule"} for s in gen_caps.SLOTS}
    for slot, routes in routes_of(data).items():
        if "routed" in routes:
            out[slot].add("routed_path")
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


def route_features(allowed: set, route: str) -> set:
    return set(allowed) if route == "routed" else set(allowed) - {"routed_path"}


def is_synthetic(path: Path, top: Path) -> bool:
    if path.name.startswith("synthetic"):
        return True
    if any(p.endswith("_synthetic") for p in path.relative_to(top).parts[:-1]):
        return True
    try:
        with open(path, "rb") as fh:
            return fh.read(10) == b"synthetic:"
    except OSError:
        return False


def counted_for(prefix: str, routes: dict) -> "list[tuple]":
    """The (slot, route) pairs a fixture folder prefix counts against."""
    if prefix == "router":
        return [(s, "routed") for s, rs in routes.items() if "routed" in rs]
    return [(s, r) for s in PREFIX_SLOTS.get(prefix, ()) for r in routes.get(s, ())]


def synthetic_fixtures(root: Path, routes: dict) -> "dict[tuple, list]":
    """Per (slot, route), the synthetic wire fixtures (repo-relative paths) that count against it."""
    out: dict = {(s, r): [] for s, rs in routes.items() for r in rs}
    fixtures = root / "test" / "support" / "fixtures"
    folders = sorted(d for d in fixtures.iterdir() if d.is_dir() and "_wire" in d.name) if fixtures.is_dir() else []
    for d in folders:
        keys = counted_for(d.name.split("_wire", 1)[0], routes)
        for f in sorted(p for p in d.rglob("*") if p.is_file() and is_synthetic(p, fixtures)):
            for k in keys:
                out[k].append(f.relative_to(root).as_posix())
    return out


def citation_ok(root: Path, cite: str) -> bool:
    """A repo path must exist; a 'P:' citation points outside the repo and is taken as written."""
    cite = (cite or "").strip()
    if not cite:
        return False
    if cite.startswith("P:"):
        return len(cite) > 2
    return (root / cite.split(" ", 1)[0].split(":", 1)[0]).exists()


def is_class(key: str) -> bool:
    return key.endswith("/") or any(ch in key for ch in "*?[")


def matches(key: str, item: str) -> bool:
    if key.endswith("/"):
        return item.startswith(key)
    return fnmatch.fnmatchcase(item, key) if is_class(key) else item == key


def waiver_terms(w) -> "tuple[str, str]":
    """(why, until) of a waiver; empty strings where it lacks one."""
    if not isinstance(w, dict):
        return "", ""
    return str(w.get("why", "")).strip(), str(w.get("until", "")).strip()


def check_waivers(where: str, waived: dict, open_items: list) -> "tuple[list, list, set]":
    """(failures, notes, items covered) for one route's waivers."""
    fails, notes, covered = [], [], set()
    for key, w in waived.items():
        why, until = waiver_terms(w)
        if not why or not until:
            fails.append(f"{where}: waiver {key} needs a 'why' and an 'until'")
            continue
        hit = [i for i in open_items if matches(key, i)]
        tag = f" (class {key})" if is_class(key) else ""
        notes += [f"{where}: waived {i}{tag} until {until}: {why}" for i in hit]
        if not hit:
            if is_class(key):
                notes.append(f"{where}: class waiver {key} matches nothing yet (until {until})")
            else:
                fails.append(f"{where}: stale waiver: {key} is no longer open (remove it)")
        covered |= set(hit)
    return fails, notes, covered


def check_route(where: str, entry: dict, features: set, synthetic: list, root: Path) -> "tuple[list, list]":
    """(failures, notes) for one slot's route."""
    recorded = entry.get("recorded", {})
    fails = []
    for feat, cite in recorded.items():
        if feat not in FEATURES:
            fails.append(f"{where}: recorded feature '{feat}' is not one the gate knows {FEATURES}")
        elif not citation_ok(root, cite):
            fails.append(f"{where}: recorded {feat} cites '{cite}', which does not exist")
    open_items = list(synthetic) + sorted(f for f in features if f not in recorded)
    wfails, notes, covered = check_waivers(where, entry.get("waived", {}), open_items)
    fails += wfails
    if entry.get("releasable"):
        for item in open_items:
            if item in covered:
                continue
            if item in synthetic:
                fails.append(f"{where}: releasable, but the synthetic fixture {item} is left")
            else:
                fails.append(f"{where}: releasable, but its rows allow {item} and no recorded 2xx is entered")
    return fails, notes


def shape_failures(record: dict, routes: dict) -> "list[str]":
    provs = record.get("providers", {})
    fails = [f"slot {s}: missing from the release record" for s in gen_caps.SLOTS if s not in provs]
    fails += [f"slot {s}: in the release record but not in models.toml" for s in provs if s not in gen_caps.SLOTS]
    for s in gen_caps.SLOTS:
        if s in provs and set(provs[s]) != set(routes[s]):
            fails.append(f"slot {s}: the record must list exactly its routes {list(routes[s])}, has {sorted(provs[s])}")
    return fails


def run(root: Path, record: dict, data: dict) -> "tuple[list, list]":
    """(failures, report lines) for the whole record."""
    routes = routes_of(data)
    allowed = allowed_features(data)
    synthetic = synthetic_fixtures(root, routes)
    provs = record.get("providers", {})
    fails = shape_failures(record, routes)
    if fails:
        return fails, []
    report = []
    for slot in gen_caps.SLOTS:
        for route in routes[slot]:
            entry, where = provs[slot][route], f"{slot} {route}"
            feats = route_features(allowed[slot], route)
            f, notes = check_route(where, entry, feats, synthetic[(slot, route)], root)
            fails += f
            report += notes
            state = "releasable" if entry.get("releasable") else "not releasable"
            unrecorded = sorted(x for x in feats if x not in entry.get("recorded", {}))
            n = len(synthetic[(slot, route)])
            report.append(f"{where}: {state}; {n} synthetic fixture(s); unrecorded: {unrecorded or 'none'}")
    return fails, report


def releasable_routes(record: dict) -> "set[str]":
    """'<slot>-<route>' for every route the record marks releasable (the scope gate reads it)."""
    return {
        f"{s}-{r}"
        for s, p in record.get("providers", {}).items()
        for r, e in p.items()
        if isinstance(e, dict) and e.get("releasable")
    }


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
