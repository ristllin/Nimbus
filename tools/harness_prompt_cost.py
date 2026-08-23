#!/usr/bin/env python3
"""Harness prompt cost instrument (lane N11).

Decomposes a composed orchestrator World prompt (the byte-pinned goldens, i.e.
the exact text the model receives) into labeled sections and reports the cost of
each: bytes (the device's native budget unit, see kContextBudgetMax=32768), word
count, and an estimated token count.

The composed prompt is produced by lib/core `assembleContext` from the same code
on host and device, so the goldens ARE the on-device bytes: a host measurement of
the golden equals the device cost. That is the point of golden-pinning.

Token estimate: tiktoken cl100k_base when available (a concrete, reproducible
tokenizer that approximates GPT-family counts within a few percent; Anthropic and
Mistral tokenize slightly differently, so treat tokens as an estimate and bytes as
exact). Falls back to a bytes/4 heuristic if tiktoken is absent.

Usage:
  tools/harness_prompt_cost.py [--json OUT.json] [FILE ...]

With no FILE, measures the two composed-prompt goldens plus the connector catalog:
  test/golden/orch_prompt_default.txt         (owner turn, tool loop ON)
  test/golden/orch_prompt_recall_loopoff.txt  (owner turn, tool loop OFF, +recall)
  test/golden/orch_catalog_full.txt           ([PROVIDERS & CONNECTORS], every turn)

Run from the repo root.
"""
import argparse
import json
import os
import sys

try:
    import tiktoken

    _ENC = tiktoken.get_encoding("cl100k_base")

    def count_tokens(s):
        return len(_ENC.encode(s))

    TOKENIZER = "tiktoken/cl100k_base"
except Exception:  # pragma: no cover - offline fallback
    def count_tokens(s):
        return round(len(s.encode("utf-8")) / 4)

    TOKENIZER = "bytes/4 (tiktoken unavailable)"


# Section header anchors, in emission order (see docs/turn-anatomy.md). Each is a
# stable, golden-pinned substring; a missing anchor means the section is absent.
HEADERS = [
    ("directive", "\n## DIRECTIVE"),
    ("capabilities", "\n## CAPABILITIES"),
    ("running_sessions", "\n## RUNNING SESSIONS"),
    ("conversation_summary", "\n## CONVERSATION SUMMARY"),
    ("recent_conversation", "\n## RECENT CONVERSATION"),
    ("scratchpad", "\n## SCRATCHPAD"),
    ("relevant_memories", "\n## RELEVANT MEMORIES"),
    ("memory_howto", "\n## HOW YOUR MEMORY WORKS"),
]
IDENTITY_ANCHOR = ", an always-on personal assistant"
FIELD_DOCS_START = "Fill the orch_turn fields:\n"
FIELD_DOCS_END = "\nWhen [FRESH RESULTS] appears"


def _line_start(text, idx):
    """Back up from idx to the start of its line."""
    nl = text.rfind("\n", 0, idx)
    return 0 if nl == -1 else nl + 1


def split_sections(text):
    """Return an ordered list of (label, chunk) covering the whole prompt."""
    # A standalone injected block (e.g. the connector catalog) has no role or
    # identity: label it by its leading tag rather than mislabeling it "role".
    if IDENTITY_ANCHOR not in text and "Fill the orch_turn fields" not in text:
        head = text.lstrip()[:32]
        label = "connector_catalog" if head.startswith("[PROVIDERS") else "block"
        return [(label, text)]
    marks = []  # (index, label)
    # identity block start (role ends where identity begins)
    ia = text.find(IDENTITY_ANCHOR)
    if ia != -1:
        marks.append((_line_start(text, ia), "identity_how_you_run"))
    for label, anchor in HEADERS:
        i = text.find(anchor)
        if i != -1:
            marks.append((i, label))
    marks.sort()
    sections = []
    prev_idx, prev_label = 0, "role_field_docs"
    for idx, label in marks:
        sections.append((prev_label, text[prev_idx:idx]))
        prev_idx, prev_label = idx, label
    sections.append((prev_label, text[prev_idx:]))
    return sections


def field_docs_span(text):
    """Bytes of the inlined ORCH_FIELD_DOCS block within role_field_docs, or None."""
    s = text.find(FIELD_DOCS_START)
    e = text.find(FIELD_DOCS_END)
    if s == -1 or e == -1 or e <= s:
        return None
    block = text[s + len(FIELD_DOCS_START):e]
    return block


def measure(label, chunk):
    b = len(chunk.encode("utf-8"))
    return {
        "section": label,
        "bytes": b,
        "words": len(chunk.split()),
        "tokens": count_tokens(chunk),
    }


def measure_file(path):
    with open(path, "rb") as f:
        text = f.read().decode("utf-8")
    total_b = len(text.encode("utf-8"))
    rows = [measure(lbl, chunk) for lbl, chunk in split_sections(text)]
    fd = field_docs_span(text)
    field_docs = measure("(of which: orch_turn field-docs)", fd) if fd is not None else None
    return {
        "file": os.path.basename(path),
        "total_bytes": total_b,
        "total_words": len(text.split()),
        "total_tokens": count_tokens(text),
        "sections": rows,
        "field_docs": field_docs,
    }


def print_report(results):
    print(f"# Harness prompt cost report   (tokenizer: {TOKENIZER})")
    print(f"# Byte budget ceiling kContextBudgetMax = 32768 B\n")
    all_rows = []
    for r in results:
        print(f"## {r['file']}")
        print(f"   total: {r['total_bytes']:>6} B   {r['total_words']:>5} words   {r['total_tokens']:>5} tokens")
        for row in sorted(r["sections"], key=lambda x: -x["bytes"]):
            share = 100.0 * row["bytes"] / r["total_bytes"] if r["total_bytes"] else 0
            print(f"     {row['bytes']:>6} B  {row['tokens']:>5} tok  {share:5.1f}%  {row['section']}")
            all_rows.append((r["file"], row))
        if r["field_docs"]:
            fd = r["field_docs"]
            share = 100.0 * fd["bytes"] / r["total_bytes"] if r["total_bytes"] else 0
            print(f"     {fd['bytes']:>6} B  {fd['tokens']:>5} tok  {share:5.1f}%  {fd['section']}")
        print()
    # Top heaviest contributors, deduplicated by section (max across prompts) and
    # with the inlined field-docs split out of its enclosing role block, so the
    # ranking names distinct pieces rather than the same section twice.
    print("## Top 5 heaviest distinct contributors (max bytes across measured prompts)")
    best = {}  # label -> (bytes, tokens)
    for _fname, row in all_rows:
        lbl = row["section"]
        if row["bytes"] > best.get(lbl, (0, 0))[0]:
            best[lbl] = (row["bytes"], row["tokens"])
    # split field-docs out of role_field_docs
    for r in results:
        if r.get("field_docs"):
            fd = r["field_docs"]
            best["orch_turn field-docs"] = max(
                best.get("orch_turn field-docs", (0, 0)), (fd["bytes"], fd["tokens"])
            )
            if "role_field_docs" in best:
                rb, rt = best["role_field_docs"]
                best["role framing (excl. field-docs)"] = (rb - fd["bytes"], rt - fd["tokens"])
                del best["role_field_docs"]
    ranked = sorted(best.items(), key=lambda kv: -kv[1][0])[:5]
    for i, (lbl, (b, t)) in enumerate(ranked, 1):
        print(f"   {i}. {b:>6} B  {t:>5} tok  {lbl}")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("files", nargs="*", help="composed-prompt text files to measure")
    ap.add_argument("--json", help="write machine-readable results to this path")
    args = ap.parse_args()

    files = args.files or [
        "test/golden/orch_prompt_default.txt",
        "test/golden/orch_prompt_recall_loopoff.txt",
        "test/golden/orch_catalog_full.txt",
    ]
    missing = [f for f in files if not os.path.exists(f)]
    if missing:
        print(f"error: file(s) not found (run from repo root): {missing}", file=sys.stderr)
        return 2
    results = [measure_file(f) for f in files]
    print_report(results)
    if args.json:
        with open(args.json, "w") as f:
            json.dump({"tokenizer": TOKENIZER, "results": results}, f, indent=2)
        print(f"\nwrote {args.json}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
