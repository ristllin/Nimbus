#!/usr/bin/env python3
"""Nimbus docs pack generator - curated docs/ -> lib/core/include/nimbus/docs_pack_data.h

Gives the Orchestrator MODEL access to the device's own documentation on-board
(the docs.list / docs.search / docs.read tools). The pack is a table of
{id, title, body} sections embedded as flash rodata, so:
  - it rides firmware.bin (an OTA update ships MATCHING docs - no version skew,
    no SD sync, no LittleFS image);
  - it is scanned in place (no gzip: there is no inflate buffer to spare on the
    ~47 KB internal heap).

Sections are split at BUILD time on `##` headings; anything larger than
MAX_BODY (~3.5 KB) is further split at `###` sub-headings, then at paragraph
boundaries, so every body fits comfortably under the head-loop's per-tool-result
clamp (4 KB floor). Section id = `<file-slug>#<heading-slug>`.

Deterministic + idempotent: pure function of the curated inputs - running it
twice produces a byte-identical header. The output is COMMITTED (like
include/web/ui_logo.h and src/sfx/sfx_basic_data.h) so CI builds without python.

Run from the repo root:  python3 tools/gen_docs_pack.py
"""

from __future__ import annotations

import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
OUT = ROOT / "lib" / "core" / "include" / "nimbus" / "docs_pack_data.h"

# Curated: capability-relevant docs the model should consult before claiming
# what it can or cannot do. Maintainer-only notes, hardware/build-* fab guides,
# and prompt samples are excluded on purpose (not about the shipped device).
CURATED = [
    "docs/orchestrator-world.md",
    "docs/reference/tool-catalog.md",
    "docs/tools-and-commands.md",
    "docs/hardware.md",
    "docs/reference/turn-contract.md",
    "docs/sub-sessions.md",
    "docs/connectors.md",
    "docs/modes-and-signals.md",
    "docs/led-ux.md",
    "docs/memory.md",
    "docs/turn-anatomy.md",
    "docs/people-and-privacy.md",
    "docs/notifier-status-language.md",
]

MAX_BODY = 3500  # target ceiling per section body (bytes, UTF-8)
RAW_DELIM = "NIMBUSDOC"  # raw-string delimiter - must not occur in any doc


def slugify(text: str) -> str:
    """Heading text -> id slug: lowercase ascii, runs of non-alnum -> '-'."""
    s = text.lower()
    s = re.sub(r"[^a-z0-9]+", "-", s)
    return s.strip("-") or "section"


def is_fence(line: str) -> bool:
    ls = line.lstrip()
    return ls.startswith("```") or ls.startswith("~~~")


def split_top(lines: list[str], marker: str) -> list[tuple[str | None, list[str]]]:
    """Split lines at headings of the given marker (e.g. '## ') OUTSIDE code
    fences. Returns [(heading_text_or_None, body_lines), ...]; the first tuple
    is the preamble (heading None), possibly empty."""
    chunks: list[tuple[str | None, list[str]]] = [(None, [])]
    fenced = False
    for line in lines:
        if is_fence(line):
            fenced = not fenced
            chunks[-1][1].append(line)
            continue
        if not fenced and line.startswith(marker):
            chunks.append((line[len(marker) :].strip(), []))
        else:
            chunks[-1][1].append(line)
    return chunks


def strip_blank(lines: list[str]) -> list[str]:
    a, b = 0, len(lines)
    while a < b and not lines[a].strip():
        a += 1
    while b > a and not lines[b - 1].strip():
        b -= 1
    return lines[a:b]


def paragraphs(lines: list[str]) -> list[list[str]]:
    """Split at blank lines OUTSIDE code fences; a fenced block stays atomic."""
    out: list[list[str]] = []
    cur: list[str] = []
    fenced = False
    for line in lines:
        if is_fence(line):
            fenced = not fenced
            cur.append(line)
            continue
        if not fenced and not line.strip():
            if cur:
                out.append(cur)
                cur = []
        else:
            cur.append(line)
    if cur:
        out.append(cur)
    return out


def body_bytes(lines: list[str]) -> int:
    return len("\n".join(lines).encode("utf-8"))


def hard_split(lines: list[str]) -> list[list[str]]:
    """Last resort: pack LINES greedily up to MAX_BODY (a single paragraph /
    fenced block / table bigger than the cap). Never splits mid-line, so a
    multi-byte codepoint can never be cut."""
    parts: list[list[str]] = []
    cur: list[str] = []
    for line in lines:
        if cur and body_bytes(cur + [line]) > MAX_BODY:
            parts.append(cur)
            cur = []
        cur.append(line)
    if cur:
        parts.append(cur)
    return parts


def pack_paragraphs(lines: list[str]) -> list[list[str]]:
    """Split an oversized body at paragraph boundaries, packing greedily."""
    parts: list[list[str]] = []
    cur: list[str] = []
    for para in paragraphs(lines):
        if body_bytes(para) > MAX_BODY:
            if cur:
                parts.append(cur)
                cur = []
            parts.extend(hard_split(para))
            continue
        cand = cur + ([""] if cur else []) + para
        if cur and body_bytes(cand) > MAX_BODY:
            parts.append(cur)
            cur = list(para)
        else:
            cur = cand
    if cur:
        parts.append(cur)
    return parts


class Section:
    def __init__(self, sid: str, title: str, body: str):
        self.id = sid
        self.title = title
        self.body = body


def unique(slug: str, seen: set[str]) -> str:
    out = slug
    n = 2
    while out in seen:
        out = f"{slug}-{n}"
        n += 1
    seen.add(out)
    return out


def sections_for_file(path: pathlib.Path, fslug: str) -> tuple[str, list[Section]]:
    text = path.read_text(encoding="utf-8")
    lines = text.split("\n")

    # File title: the first H1 outside a fence; the line is consumed.
    ftitle = fslug
    fenced = False
    for i, line in enumerate(lines):
        if is_fence(line):
            fenced = not fenced
            continue
        if not fenced and line.startswith("# "):
            ftitle = line[2:].strip()
            lines = lines[:i] + lines[i + 1 :]
            break

    seen: set[str] = set()
    secs: list[Section] = []

    def emit(slug_base: str, title: str, body_lines: list[str]) -> None:
        body_lines = strip_blank(body_lines)
        if not body_lines:
            return
        if body_bytes(body_lines) <= MAX_BODY:
            sid = f"{fslug}#{unique(slug_base, seen)}"
            secs.append(Section(sid, title, "\n".join(body_lines)))
            return
        parts = pack_paragraphs(body_lines)
        n = len(parts)
        for k, part in enumerate(parts, start=1):
            slug = slug_base if k == 1 else f"{slug_base}-{k}"
            sid = f"{fslug}#{unique(slug, seen)}"
            secs.append(Section(sid, f"{title} ({k}/{n})", "\n".join(part)))

    for h2, h2_lines in split_top(lines, "## "):
        if h2 is None:
            emit("intro", ftitle, h2_lines)
            continue
        h2_slug = slugify(h2)
        if body_bytes(strip_blank(h2_lines)) <= MAX_BODY:
            emit(h2_slug, h2, h2_lines)
            continue
        # Oversized H2: split at ### sub-headings first (each gets its own id).
        subs = split_top(h2_lines, "### ")
        if len(subs) == 1:
            emit(h2_slug, h2, h2_lines)  # no sub-headings -> paragraph packing
            continue
        for h3, h3_lines in subs:
            if h3 is None:
                emit(h2_slug, h2, h3_lines)
            else:
                emit(f"{h2_slug}-{slugify(h3)}", f"{h2} - {h3}", h3_lines)

    return ftitle, secs


def cstr(s: str) -> str:
    return '"' + s.replace("\\", "\\\\").replace('"', '\\"') + '"'


def main() -> int:
    files: list[tuple[str, str, list[Section]]] = []  # (slug, title, sections)
    total_raw = 0
    for rel in CURATED:
        p = ROOT / rel
        if not p.is_file():
            print(f"missing curated doc: {rel}", file=sys.stderr)
            return 1
        total_raw += p.stat().st_size
        fslug = p.stem
        if any(f[0] == fslug for f in files):
            print(f"file-slug collision: {fslug}", file=sys.stderr)
            return 1
        ftitle, secs = sections_for_file(p, fslug)
        for s in secs:
            if RAW_DELIM in s.body:
                print(f"raw-string delimiter {RAW_DELIM} appears in {s.id}", file=sys.stderr)
                return 1
        files.append((fslug, ftitle, secs))

    nsec = sum(len(f[2]) for f in files)
    pack_bytes = sum(
        len(s.body.encode("utf-8")) + len(s.id) + len(s.title.encode("utf-8")) for f in files for s in f[2]
    )

    w: list[str] = []
    w.append("// GENERATED by tools/gen_docs_pack.py - edit docs/, re-run. DO NOT EDIT.")
    w.append("// The device's own documentation, embedded as flash rodata so it rides")
    w.append("// firmware.bin (OTA updates it for free; it can never version-skew).")
    w.append(
        f"// {len(files)} files, {nsec} sections, ~{pack_bytes / 1024:.0f} KB "
        f"(from {total_raw / 1024:.0f} KB of markdown)."
    )
    w.append("#pragma once")
    w.append('#include "nimbus/docs_pack.h"')
    w.append("")
    w.append("namespace nimbus {")
    w.append("namespace docs {")
    w.append("")
    w.append("inline constexpr DocSection kDocsSections[] = {")
    for fslug, ftitle, secs in files:
        w.append(f"  // ---- {fslug} ----")
        for s in secs:
            w.append(f"  {{{cstr(s.id)}, {cstr(s.title)},")
            w.append(f'   R"{RAW_DELIM}({s.body}){RAW_DELIM}"}},')
    w.append("};")
    w.append(f"inline constexpr size_t kDocsSectionCount = {nsec};")
    w.append("")
    w.append("inline constexpr DocFile kDocsFiles[] = {")
    first = 0
    for fslug, ftitle, secs in files:
        w.append(f"  {{{cstr(fslug)}, {cstr(ftitle)}, {first}, {len(secs)}}},")
        first += len(secs)
    w.append("};")
    w.append(f"inline constexpr size_t kDocsFileCount = {len(files)};")
    w.append("")
    w.append("}  // namespace docs")
    w.append("}  // namespace nimbus")
    w.append("")

    OUT.parent.mkdir(parents=True, exist_ok=True)
    OUT.write_text("\n".join(w), encoding="utf-8")
    print(
        f"wrote {OUT.relative_to(ROOT)}: {len(files)} files, {nsec} sections, "
        f"~{pack_bytes / 1024:.0f} KB pack ({total_raw / 1024:.0f} KB raw markdown)"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
