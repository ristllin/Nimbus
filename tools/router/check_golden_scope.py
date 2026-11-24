#!/usr/bin/env python3
"""The golden scope gate: a change for one provider cannot move another provider's bytes unnoticed.

Router design section 11, layer L5 (lane E2-R3b). The route goldens live in
test/support/goldens/wire/<dialect>-<slot>-<route>/ (written by test_route_goldens through
tools/router/refresh_goldens.py). For a change, every golden it adds, removes or edits must belong
to something the change names:
  - a slot whose rows moved in tools/model_caps/models.toml (provider row, model rows after their
    templates are applied, or error markers), compared old against new;
  - the dialect of a codec file the change edits (plain_wire_anthropic.cpp names messages,
    plain_wire_openai.cpp responses, plain_wire_chat.cpp chat);
  - a line `Golden-scope: <name>, <name>` in a commit message of the range, in the PR body
    (--pr-body) or, for a staged commit, in the GOLDEN_SCOPE variable. A name is a slot
    (anthropic), a dialect (chat), a signature (chat-mistral-direct) or one golden
    (chat-mistral-direct/one_call.req). A planner, classifier or exchange change names nothing by
    itself, so it lists what it moved this way.
A golden of a slot marked releasable in tools/router/release_record.json also needs a ruling:
`Golden-ruling: <slot> <ruling id>` (same places; GOLDEN_RULING for a staged commit).

Usage:
  python3 tools/router/check_golden_scope.py                    the staged change (pre-commit)
  python3 tools/router/check_golden_scope.py --range A..B [--pr-body FILE]
Exit 0: every moved golden is named; 1: one is not (each listed); 2: usage or git error.
"""

from __future__ import annotations

import argparse
import json
import os
import re
import subprocess
import sys
import tomllib
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools" / "model_caps"))

import gen_caps  # noqa: E402

GOLDENS = "test/support/goldens/wire/"
MODELS = "tools/model_caps/models.toml"
RECORD = ROOT / "tools" / "router" / "release_record.json"
CODEC_DIALECT = {
    "lib/harness/src/providers/plain_wire_anthropic.cpp": "messages",
    "lib/harness/src/providers/plain_wire_openai.cpp": "responses",
    "lib/harness/src/providers/plain_wire_chat.cpp": "chat",
}
SCOPE_LINE = re.compile(r"^\s*Golden-scope:\s*(.+?)\s*$", re.M)
RULING_LINE = re.compile(r"^\s*Golden-ruling:\s*([a-z_]+)\s+(\S.*?)\s*$", re.M)


def golden_of(path: str):
    """(signature, dialect, slot) of a golden path, None for a file outside the goldens or the
    MANIFEST; a signature that names no known dialect or slot gives (signature, None, None)."""
    if not path.startswith(GOLDENS):
        return None
    rel = path[len(GOLDENS) :]
    if "/" not in rel:
        return None
    sig = rel.split("/", 1)[0]
    parts = sig.split("-")
    if len(parts) < 3 or parts[0] not in gen_caps.DIALECTS or parts[1] not in gen_caps.SLOTS:
        return (sig, None, None)
    return (sig, parts[0], parts[1])


def slot_fingerprints(data: dict) -> dict:
    """Per slot, everything the table says about it, templates applied: a slot whose print moves
    is named by the change."""
    templates = data.get("template", {})
    errs: list = []
    out = {}
    for slot in gen_caps.SLOTS:
        provider = [p for p in data.get("provider", []) if p.get("slot") == slot]
        models = sorted(
            (gen_caps.resolve(r, templates, "row", errs) for r in data.get("model", []) if r.get("slot") == slot),
            key=lambda r: (str(r.get("kind")), str(r.get("match"))),
        )
        markers = [m for m in data.get("marker", []) if m.get("slot") == slot]
        out[slot] = json.dumps([provider, models, markers], sort_keys=True, default=str)
    return out


def slots_moved(old_text, new_text) -> set:
    """Slots whose rows differ between two models.toml texts (None: the file is absent there)."""
    if old_text == new_text:
        return set()
    if old_text is None or new_text is None:
        return set(gen_caps.SLOTS)
    try:
        old, new = slot_fingerprints(tomllib.loads(old_text)), slot_fingerprints(tomllib.loads(new_text))
    except tomllib.TOMLDecodeError:
        return set(gen_caps.SLOTS)
    return {s for s in gen_caps.SLOTS if old[s] != new[s]}


def parse_names(texts) -> "tuple[set, dict]":
    """Golden-scope names and Golden-ruling slots (slot -> ruling id) from free texts."""
    names, rulings = set(), {}
    for t in texts:
        for m in SCOPE_LINE.finditer(t or ""):
            names |= {n.strip() for n in m.group(1).split(",") if n.strip()}
        for m in RULING_LINE.finditer(t or ""):
            rulings[m.group(1)] = m.group(2)
    return names, rulings


def releasable_slots(record: dict) -> set:
    return {s for s, p in record.get("providers", {}).items() if p.get("releasable")}


def verdict(changed, models_slots, names, rulings, releasable) -> "list[str]":
    """Every failure line for a change: `changed` is the list of paths it touches."""
    dialects = {CODEC_DIALECT[p] for p in changed if p in CODEC_DIALECT}
    errors = []
    for path in sorted(set(changed)):
        g = golden_of(path)
        if g is None:
            continue
        sig, dialect, slot = g
        if dialect is None:
            errors.append(f"{path}: signature '{sig}' names no known dialect and slot (<dialect>-<slot>-<route>)")
            continue
        rel = path[len(GOLDENS) :]
        covered = slot in models_slots or dialect in dialects or {slot, dialect, sig, rel, path} & names
        if not covered:
            errors.append(
                f"{path}: moves {slot} bytes, but the change names only "
                f"{sorted(models_slots | dialects | names) or 'nothing'} (add 'Golden-scope: {slot}' if meant)"
            )
        elif slot in releasable and slot not in rulings:
            errors.append(
                f"{path}: {slot} is releasable; its goldens move only with 'Golden-ruling: {slot} <ruling id>'"
            )
    return errors


def git(*args: str) -> str:
    return subprocess.run(["git", *args], cwd=ROOT, check=True, capture_output=True, text=True).stdout


def git_show(spec: str):
    r = subprocess.run(["git", "show", spec], cwd=ROOT, capture_output=True, text=True)
    return r.stdout if r.returncode == 0 else None


def gather(rng, pr_body) -> "tuple[list, set, set, dict]":
    if rng:
        a, b = rng.split("..", 1)
        changed = git("diff", "--name-only", "--no-renames", f"{a}..{b}").split()
        old, new = git_show(f"{a}:{MODELS}"), git_show(f"{b}:{MODELS}")
        texts = [git("log", "--format=%B", f"{a}..{b}")]
    else:
        changed = git("diff", "--cached", "--name-only", "--no-renames").split()
        old, new = git_show(f"HEAD:{MODELS}"), git_show(f":{MODELS}")
        texts = ["Golden-scope: " + os.environ.get("GOLDEN_SCOPE", "")]
        if os.environ.get("GOLDEN_RULING"):
            texts += ["Golden-ruling: " + r.strip() for r in os.environ["GOLDEN_RULING"].split(";")]
    if pr_body:
        texts.append(Path(pr_body).read_text(encoding="utf-8"))
    names, rulings = parse_names(texts)
    moved = slots_moved(old, new) if MODELS in changed else set()
    return changed, moved, names, rulings


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--range", help="A..B: judge the commits from A to B (default: the staged change)")
    ap.add_argument("--pr-body", help="a file whose Golden-scope and Golden-ruling lines also count")
    args = ap.parse_args(argv)
    try:
        changed, moved, names, rulings = gather(args.range, args.pr_body)
        record = json.loads(RECORD.read_text(encoding="utf-8"))
    except (subprocess.CalledProcessError, OSError, ValueError) as e:
        print(f"check_golden_scope: {e}", file=sys.stderr)
        return 2
    errors = verdict(changed, moved, names, rulings, releasable_slots(record))
    goldens = [p for p in changed if golden_of(p)]
    for e in errors:
        print("FAIL " + e)
    print(f"check_golden_scope: {len(goldens)} golden(s) moved, {len(errors)} outside the change's scope")
    return 1 if errors else 0


if __name__ == "__main__":
    sys.exit(main())
