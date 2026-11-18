#!/usr/bin/env python3
"""Tool results state facts and never give orders (PRD section 4, a build-time lint).

Reads the tool result and error templates of every live catalog (gen_model_text_catalog.py:
class tool_results, and stubs on engine 2) and flags a template that tells the model
what to do: a sentence that opens with an order ("Call files.read first", "Do not retry",
"ask the owner ...") or a directive phrase ("you must", "make sure", "be sure to").

Engine 1 predates the rule. Its flagged templates are listed in the "Imperative
baseline" table of coverage/PROMPT-CHANGES.md by source and sha8; that list only
shrinks: a flagged template not on it fails, a listed one that is gone or no longer
flagged fails until its row is deleted, and (with a git base) a row the base did not
have fails. Engine 2 has no baseline.

Usage: python3 tools/release_gate/check_tool_result_imperatives.py [--base REF] [--no-base]
Exit 0 with TOOL RESULT IMPERATIVES GREEN as the last line; 1 with TOOL RESULT IMPERATIVES RED.
"""

from __future__ import annotations

import argparse
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import check_no_prompt_patch as gate  # noqa: E402
import gen_model_text_catalog as cat  # noqa: E402

LINT_CLASSES = {"e1": ("tool_results",), "e2": ("tool_results", "stubs")}

# Verbs that open an order when they start a sentence (base form only: "saved", "added"
# and "read 120 B" state facts).
ORDER_VERBS = (
    "add",
    "answer",
    "append",
    "ask",
    "avoid",
    "call",
    "check",
    "confirm",
    "consider",
    "continue",
    "delete",
    "do",
    "don't",
    "edit",
    "ensure",
    "fetch",
    "finish",
    "fix",
    "follow",
    "give",
    "include",
    "keep",
    "let",
    "list",
    "look",
    "make",
    "mention",
    "move",
    "never",
    "always",
    "note",
    "pass",
    "pick",
    "please",
    "prefer",
    "put",
    "quote",
    "re-run",
    "rerun",
    "read",
    "remember",
    "remove",
    "reply",
    "report",
    "request",
    "retry",
    "return",
    "run",
    "save",
    "say",
    "search",
    "send",
    "set",
    "show",
    "split",
    "start",
    "state",
    "stop",
    "switch",
    "tell",
    "try",
    "use",
    "verify",
    "wait",
    "write",
)
_START = r"(?:^|[.!?;:]\s+|\n\s*|\(\s*|-\s+|\]\s*)(?:(?:now|then|first|just|also|so|instead)\s+)?"
# The verb must be followed by a word, a quote or a bracket ("read 120 B" states a fact), and
# not by a word that makes it a noun or a report: a status or past participle ("write failed",
# "Report saved", "Reply sent"), "of" ("List of files"), or a noun ("Search results", "Start time").
_STATUS = (
    r"(?:\w+ed|not|is|was|were|are|has|had|sent|done|made|kept|set|run|ran|read|written|found|held|left|built|"
    r"put|ok|of|results?|time|code|count|size|bytes|status|mode|id|ids|errors?|limit|list|text|line|rows?)\b"
)
ORDER_START = re.compile(
    _START + r"(" + "|".join(re.escape(v) for v in ORDER_VERBS) + r")\s+"
    r"(?!" + _STATUS + r")(?=[A-Za-z_'\"`(\[])",
    re.I,
)
DIRECTIVE = re.compile(
    r"\b(you (?:must|should|need to|have to|may not|cannot skip)|make sure|be sure|"
    r"do not|don't|never (?:call|say|tell|claim|use|retry)|instead,? (?:call|use|ask))\b",
    re.I,
)
_BASE_ROW = re.compile(r"^\| (\S+) \| ([0-9a-f]{8}) \|")


def order_in(text: str) -> "str | None":
    """The order a template gives, or None when it states facts."""
    m = DIRECTIVE.search(text)
    if m:
        return m.group(0)
    for m in ORDER_START.finditer(text):
        nxt = re.match(r"\S+", text[m.end() :])
        return f"{m.group(1)} {nxt.group(0) if nxt else ''}".strip()
    return None


def parse_baseline(text: str) -> "list[tuple[str, str]]":
    rows, section = [], ""
    for line in text.splitlines():
        if line.startswith("## "):
            section = line[3:].strip().lower()
            continue
        if section.startswith("imperative baseline"):
            m = _BASE_ROW.match(line)
            if m:
                rows.append((m.group(1), m.group(2)))
    return rows


def flagged(root: str) -> "list[tuple[str, str, str, str, str]]":
    """(catalog, source, sha8, order, text) of every template that gives an order."""
    out = []
    for name, entries in cat.build_all(root).items():
        if entries is None:
            continue
        for e in entries:
            if e.cls in LINT_CLASSES.get(name, ()):
                order = order_in(e.text)
                if order:
                    out.append((name, e.path, e.sha8, order, e.text))
    return out


def run(root: str, base: "str | None", use_base: bool = True) -> "list[str]":
    try:
        hits = flagged(root)
    except cat.CatalogError as e:
        return [str(e)]
    changes = cat._read(root, gate.CHANGES) or ""
    baseline = parse_baseline(changes)
    errs = []
    if len(set(baseline)) != len(baseline):
        errs.append("imperative baseline: a row is listed twice")
    allowed = set(baseline)
    seen = set()
    for name, path, sha, order, text in hits:
        key = (path, sha)
        if name == "e1" and key in allowed:
            seen.add(key)
            continue
        errs.append(
            f"{path} {sha}: a tool result gives an order ({order!r}): {text[:90]!r}; state the fact "
            "and let the model decide"
        )
    for key in sorted(allowed - seen):
        errs.append(f"imperative baseline: {key[0]} {key[1]} no longer gives an order or is gone; delete its row")
    if use_base and base:
        old = gate.show(root, base, gate.CHANGES)
        if old is not None:
            for key in sorted(allowed - set(parse_baseline(old))):
                errs.append(f"imperative baseline: {key[0]} {key[1]} was added; the baseline only shrinks")
    elif use_base and not os.environ.get("GITHUB_ACTIONS"):
        errs.append("no git base found (pass --base REF, or --no-base to skip the history check)")
    return errs


def baseline_rows(root: str) -> str:
    """The table rows for the current engine-1 hits (used once, when the gate was introduced)."""
    rows = []
    for name, path, sha, order, text in flagged(root):
        if name == "e1":
            snippet = text[:60].replace("|", "/").replace("\n", " ").encode("ascii", "replace").decode()
            rows.append(f"| {path} | {sha} | {order} | {snippet} |")
    return "\n".join(sorted(set(rows)))


def main(argv: "list[str] | None" = None) -> int:
    ap = argparse.ArgumentParser(description="Tool results state facts, never orders")
    ap.add_argument("--root", default=gate.REPO)
    ap.add_argument("--base")
    ap.add_argument("--no-base", action="store_true")
    ap.add_argument("--print-baseline", action="store_true", help=argparse.SUPPRESS)
    args = ap.parse_args(argv)
    if args.print_baseline:
        print(baseline_rows(args.root))
        return 0
    base = None if args.no_base else gate.resolve_base(args.root, args.base)
    errs = run(args.root, base, use_base=not args.no_base)
    for e in errs:
        print(f"imperatives: {e}")
    print("TOOL RESULT IMPERATIVES RED" if errs else "TOOL RESULT IMPERATIVES GREEN")
    return 1 if errs else 0


if __name__ == "__main__":
    sys.exit(main())
