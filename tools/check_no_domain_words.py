#!/usr/bin/env python3
"""Domain-words lint: no use case is named in the core (owner ruling 41, ruling 42 item 4).

Nimbus is a general harness. Food tracking, expenses and a training log are validation
scenarios, not built-ins, so no firmware code, tool text, error string, prompt or shipped
docs page in the scanned scope may name one of them.

Scope: lib/core, lib/harness, src, nimbusd (its tests excepted: tests carry the three
validation domains on purpose) and the generated docs pack (lib/core/include/nimbus/docs_pack_data.h
sits inside lib/core). Matching is by whole token: identifiers are split on underscores
and case changes (foodLedger -> food, ledger; FOOD_LOG -> food, log), so "themeAlertHue"
or "centsPerKCalls" never match. "ledger" is not a domain word (it names the usage and
spend ledgers all over the core), and neither is "macro" (it names the C preprocessor's
macros all over the core).

Usage:
  check_no_domain_words.py [--root DIR] [--report]
Exit 0 when no hit (or always with --report); 1 when a hit is found.
"""

from __future__ import annotations

import argparse
import os
import re
import sys

WORDS = frozenset(
    {
        "food",
        "foods",
        "meal",
        "meals",
        "kcal",
        "calorie",
        "calories",
        "protein",
        "proteins",
        "nutrition",
        "nutritional",
        "kj",
    }
)

SCOPE = ("lib/core", "lib/harness", "src", "nimbusd")
# Paths (relative, "/"-separated) never scanned. Tests hold the validation domains by design.
EXCLUDE_PREFIXES = ("nimbusd/tests/",)
# A name collision, not a use case: the usage ledger tracks spend (ruling 42 item 4).
ALLOWLIST = ("lib/core/src/orch_usage_ledger.cpp", "lib/core/include/nimbus/orch/usage_ledger.h")
EXTENSIONS = (".c", ".cc", ".cpp", ".h", ".hpp", ".py", ".js", ".mjs", ".md", ".txt", ".json", ".html", ".css")

_TOKEN = re.compile(r"[A-Za-z0-9]+")
_PART = re.compile(r"[A-Z]?[a-z]+|[A-Z]+(?![a-z])|[0-9]+")


def tokens(text: str):
    """Whole tokens of a line, identifiers split on underscores and case changes, lower-cased."""
    for m in _TOKEN.finditer(text):
        word = m.group(0)
        # "kJ" is its own unit token; the case split would cut it in two.
        if word in ("kJ", "KJ", "kj"):
            yield "kj"
            continue
        for part in _PART.findall(word):
            yield part.lower()


def line_hits(line: str) -> list[str]:
    return sorted({t for t in tokens(line) if t in WORDS})


def scanned_files(root: str):
    for top in SCOPE:
        base = os.path.join(root, top)
        for dirpath, dirnames, filenames in os.walk(base):
            dirnames.sort()
            for name in sorted(filenames):
                if not name.endswith(EXTENSIONS):
                    continue
                rel = os.path.relpath(os.path.join(dirpath, name), root).replace(os.sep, "/")
                if rel.startswith(EXCLUDE_PREFIXES) or rel in ALLOWLIST:
                    continue
                yield rel


def scan(root: str) -> list[tuple[str, int, list[str], str]]:
    hits = []
    for rel in scanned_files(root):
        with open(os.path.join(root, rel), encoding="utf-8", errors="replace") as f:
            for n, line in enumerate(f, 1):
                found = line_hits(line)
                if found:
                    hits.append((rel, n, found, line.strip()))
    return hits


def main(argv: "list[str] | None" = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--root", default=os.path.normpath(os.path.join(os.path.dirname(__file__), "..")))
    ap.add_argument("--report", action="store_true", help="list every hit and exit 0")
    args = ap.parse_args(argv)
    hits = scan(args.root)
    files = sorted({h[0] for h in hits})
    for rel, n, found, text in hits:
        print(f"{rel}:{n}: [{','.join(found)}] {text[:160]}")
    print(f"domain words: {len(hits)} lines in {len(files)} files (scope: {', '.join(SCOPE)})")
    if not hits:
        print("NO DOMAIN WORDS GREEN")
        return 0
    if args.report:
        print("NO DOMAIN WORDS REPORT (not enforced)")
        return 0
    print("NO DOMAIN WORDS RED: name the general behavior, not a use case (owner ruling 41)")
    return 1


if __name__ == "__main__":
    sys.exit(main())
