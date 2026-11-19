#!/usr/bin/env python3
"""Gate: the old engine's request stays under its measured ceilings (lane RB-Q3b, PRD section 11).

The owner's device sent a 44 KB system prompt and 56 registry tools on every owner turn. RB-Q3a cut
the per-turn text; this gate keeps the weight from creeping back (RB-Q3b measured hiding the admin
tools behind `more` and ruling 65 kept every tool always on for engine 1). Its input is measured on the real build, not estimated:

- test/golden/e1_request_ceiling.txt: the figures test_e1_request_ceiling measures when the synthetic
  twin of a busy owner device (test/support/fixtures/lumi_state_prompt) sends an owner text turn
  through the real TurnEngine and the real Anthropic adapter. The native suite fails when the golden
  differs from what the build sends, so these are the build's numbers.
- test/support/fixtures/lumi_state_prompt/device_tools.json: the device's tool registrations that run
  feeds the engine. This gate proves it is the source's: every description and schema there is a
  string literal of the source (or a join of literals, for a text built at run time), and every tool
  the device registers is there. A tool that grows in the source therefore makes this gate red until
  the fixture and the golden carry the growth, and then the ceiling decides.

It fails (PROMPT CEILING RED) when:
- the golden is missing or malformed, or names a tool the fixture does not hold;
- the fixture is stale against the source (above), or a device tool is missing from it;
- a figure (prompt, prompt_demand, text, tools, tools_wire) passes its ceiling: the figure measured
  at RB-Q3b plus MARGIN_BYTES, plus the bytes every allowance file
  tools/release_gate/prompt_ceiling.d/<lane>.json grants it; or the prompt passes its 30 KB cap,
  which no allowance raises;
- an allowance file is malformed, named for another lane than it says, or for a lane that is not in
  tools/engine/owners.json, or grants more than 1,024 B to a figure without naming a ruling;
- a source file cannot be read, a tool is registered under a name that is not a literal (and not
  on DYNAMIC_NAMES), or the fixture names a tool no source registers.

Raising the ceiling: a lane that adds model-facing bytes on purpose adds its own allowance file, with
the measured bytes and why, in the same commit as the growth and the re-blessed golden (the image
allowance files work the same way: tools/release_gate/image_budget.d).

Usage: python3 tools/release_gate/check_prompt_ceiling.py [--root DIR]
Exit 0 with "PROMPT CEILING GREEN", 1 with "PROMPT CEILING RED".
"""

from __future__ import annotations

import argparse
import glob
import json
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

import gen_model_text_catalog as cat  # noqa: E402

REPO = os.path.dirname(os.path.dirname(HERE))
GOLDEN = "test/golden/e1_request_ceiling.txt"
FIXTURE = "test/support/fixtures/lumi_state_prompt/device_tools.json"
ALLOWANCES = "tools/release_gate/prompt_ceiling.d"
OWNERS = "tools/engine/owners.json"
BLESS = "GOLDEN_UPDATE=1 pio test -e native -f test_e1_request_ceiling"

# The figures RB-Q3b measured on its tip (test/golden/e1_request_ceiling.txt), the margin above them,
# and the definition-of-done caps. prompt_demand is the prompt at the largest context budget an owner
# can set: on the busy twin the head's budget clips the static block, so text added there shows in
# prompt_demand while prompt stays put; both are held. A figure over measured + margin needs an allowance file; a figure
# over its cap needs an orchestrator ruling that changes the cap here.
MEASURED = {"prompt": 29420, "prompt_demand": 30590, "text": 33841, "tools": 27008, "tools_wire": 33601}
# An allowance over this many bytes for one figure names the orchestrator ruling that grants it.
ALLOWANCE_RULING_BYTES = 1024
MARGIN_BYTES = 128
CAPS = {"prompt": 30720}
# Sources whose `.add("a.b", ...)` calls register the device's tools on an owner turn's registry.
REGISTRATION_DIRS = ("src", "lib/core/src", "lib/core/include", "lib/harness/src", "lib/harness/include")
# Names an add() call takes that are not tools of an owner turn's tools array, each with its reason.
NOT_LISTED = {
    "session.spawn": "registered for the LAN MCP endpoint; the head spawns through session_ops (loopToolHidden)",
    "session.tell": "registered for the LAN MCP endpoint; loopToolHidden keeps it off the head's list",
    "session.poll": "registered for the LAN MCP endpoint; loopToolHidden keeps it off the head's list",
    "reasoning.encrypted_content": "an OpenAI include[] value added to a JSON array, not a tool",
    "more": "lib/core's router (orch_tool_groups.cpp registerMore): engine 2 registers it; the old engine does "
    "not (ruling 65)",
}
# Sources compiled only into an engine-2 image (tools/engine_filter.py drops them from an engine-1 one).
ENGINE2_ONLY = "tools/engine/engine2_only.txt"
_MEASURE = re.compile(r"^measure turn=owner_text((?: \w+=\d+)+)$")
_TOOL = re.compile(r"^tool name=(\S+) group=(\S+) raw=(\d+) sent=([01])$")


# ---- the golden ---------------------------------------------------------------------------------


def parse_golden(text: str) -> "tuple[dict, list, list]":
    """The measure figures, the tool rows (name, group, raw, sent) and any errors."""
    figures, tools, errs = {}, [], []
    for line in text.splitlines():
        if line.startswith("measure "):
            m = _MEASURE.match(line)
            if not m:
                errs.append(f"{GOLDEN}: malformed line: {line}")
                continue
            figures = dict((k, int(v)) for k, v in (kv.split("=") for kv in m.group(1).split()))
        elif line.startswith("tool "):
            m = _TOOL.match(line)
            if not m:
                errs.append(f"{GOLDEN}: malformed line: {line}")
                continue
            tools.append((m.group(1), m.group(2), int(m.group(3)), m.group(4) == "1"))
    for fig in MEASURED:
        if fig not in figures:
            errs.append(f"{GOLDEN}: no {fig} figure on the measure line")
    return figures, tools, errs


# ---- allowances ---------------------------------------------------------------------------------


def load_allowances(root: str) -> "tuple[dict, list]":
    """Bytes granted per figure by the allowance files, and any errors."""
    granted, errs = {fig: 0 for fig in MEASURED}, []
    owners = cat._read(root, OWNERS)
    lanes = set(json.loads(owners).get("lanes", {})) if owners else set()
    for path in sorted(glob.glob(os.path.join(root, ALLOWANCES, "*.json"))):
        rel = os.path.relpath(path, root)
        try:
            with open(path, encoding="utf-8") as fh:
                a = json.load(fh)
        except (OSError, ValueError) as e:
            errs.append(f"{rel}: not JSON ({e})")
            continue
        lane = os.path.splitext(os.path.basename(path))[0]
        if not isinstance(a, dict) or a.get("lane") != lane:
            errs.append(f"{rel}: its lane field must be {lane}")
            continue
        if lanes and lane not in lanes:
            errs.append(f"{rel}: {lane} is not a lane of {OWNERS}")
        if not str(a.get("why", "")).strip():
            errs.append(f"{rel}: says nothing about why")
        for fig in granted:
            v = a.get(f"{fig}_bytes", 0)
            if not isinstance(v, int) or v < 0:
                errs.append(f"{rel}: {fig}_bytes must be a whole number of bytes, at least 0")
                continue
            if v > ALLOWANCE_RULING_BYTES and not re.match(r"^Ruling \d+", str(a.get("ruling", ""))):
                errs.append(
                    f"{rel}: {fig}_bytes {v} is over {ALLOWANCE_RULING_BYTES} B: name the ruling "
                    "that grants it (\"ruling\": \"Ruling NN\")"
                )
            granted[fig] += v
    return granted, errs


def ceiling_problems(figures: dict, granted: dict) -> "list[str]":
    errs = []
    for fig, measured in MEASURED.items():
        if fig not in figures:
            continue
        ceiling = measured + MARGIN_BYTES + granted.get(fig, 0)
        if figures[fig] > ceiling:
            errs.append(
                f"{fig}: {figures[fig]} B is over its ceiling {ceiling} B (measured {measured} + margin "
                f"{MARGIN_BYTES} + allowances {granted.get(fig, 0)}): take bytes out, or add "
                f"{ALLOWANCES}/<lane>.json with the measured growth and why"
            )
        if fig in CAPS and figures[fig] > CAPS[fig]:
            errs.append(f"{fig}: {figures[fig]} B is over the {CAPS[fig]} B cap (RB-Q3b definition of done)")
    return errs


# ---- the fixture against the source -------------------------------------------------------------
#
# Every ToolRegistry::add call of the device sources (three or more arguments: name, description,
# handler[, schema]) is read as source text. Its description and schema arguments become a pattern:
# a string literal, std::string("...") or a constant whose literal the sources define is fixed text;
# anything else (a function's result, a variable) is a hole that matches any text. The fixture's
# description and schema must match the pattern of the tool's registration exactly, so a literal
# added, removed or changed anywhere in the argument makes the gate red. A registration whose name is
# not a literal must be on DYNAMIC_NAMES with its reason (a runtime tool the twin cannot hold).

DYNAMIC_NAMES = {
    ("src/agent/connectors.cpp", "regName"): "a connected MCP server's remote tools, registered at run time "
    "under mcp.<server>.<tool>; the twin has no connected server (its connectors attach on the provider side)",
    ("lib/harness/include/nimbus/harness/plain/request.h", "d->name"): "engine 2's plain tool pack (not linked into "
    "an engine-1 image; its ceilings are check_plain_ceilings.py's)",
}
# A ToolRegistry receiver: the device's registry (g_reg, registry()), a registration function's
# parameter (reg, reg_). Other classes' add( calls (effect ledgers, batches, spend) are not tools.
_ADD_CALL = re.compile(r"(?:\bg_reg|\breg_?|\bregistry\(\))\s*(?:\.|->)\s*(add|addOwnerOnly)\s*\(")
_IDENT = re.compile(r"^(?:[A-Za-z_]\w*::)*([A-Za-z_]\w*)$")
_STRING_CTOR = re.compile(r"^(?:std::)?(?:string|String)\s*\((.*)\)$", re.S)


def _skip_literal(src: str, i: int) -> int:
    """The index after the string or char literal (or comment) starting at i, else i."""
    if src.startswith("//", i):
        j = src.find("\n", i)
        return len(src) if j < 0 else j
    if src.startswith("/*", i):
        j = src.find("*/", i + 2)
        return len(src) if j < 0 else j + 2
    m = re.match(r'(?:u8|u|U|L)?R"([^(\s]*)\(', src[i : i + 40]) if src[i] in 'uULR' else None
    if m and (i == 0 or not (src[i - 1].isalnum() or src[i - 1] == "_")):
        end = src.find(")" + m.group(1) + '"', i + m.end())
        return len(src) if end < 0 else end + len(m.group(1)) + 2
    if src[i] in "\"'":
        q, j = src[i], i + 1
        while j < len(src) and src[j] != q:
            j += 2 if src[j] == "\\" else 1
        return j + 1
    return i


def call_args(src: str, open_paren: int) -> "list[str] | None":
    """The top-level arguments of the call whose '(' is at open_paren, as source text."""
    depth, i, start, args = 0, open_paren, open_paren + 1, []
    while i < len(src):
        j = _skip_literal(src, i)
        if j != i:
            i = j
            continue
        c = src[i]
        if c in "([{":
            depth += 1
        elif c in ")]}":
            depth -= 1
            if depth == 0:
                args.append(src[start:i].strip())
                return args
        elif c == "," and depth == 1:
            args.append(src[start:i].strip())
            start = i + 1
        i += 1
    return None


def _top_level_terms(expr: str) -> "list[str]":
    terms, depth, i, start = [], 0, 0, 0
    while i < len(expr):
        j = _skip_literal(expr, i)
        if j != i:
            i = j
            continue
        if expr[i] in "([{":
            depth += 1
        elif expr[i] in ")]}":
            depth -= 1
        elif expr[i] == "+" and depth == 0:
            terms.append(expr[start:i].strip())
            start = i + 1
        i += 1
    terms.append(expr[start:].strip())
    return terms


def _literal_text(term: str) -> "str | None":
    """The text of a term made of string literals only, else None."""
    try:
        lits = cat.extract_literals(term)
    except (ValueError, IndexError):
        return None
    if not lits:
        return None
    rest = term
    for m in re.finditer(r'(?:u8|u|U|L)?R"([^(\s]*)\((?s:.*?)\)\1"|(?:u8|u|U|L)?"(?:\\.|[^"\\])*"', term):
        rest = rest.replace(m.group(0), "", 1)
    return "".join(lit.text for lit in lits) if not rest.strip() else None


def pattern_of(expr: str, constants: dict) -> "tuple[re.Pattern, bool]":
    """The regex an argument expression's text must match, and whether it holds any fixed text."""
    parts, fixed = [], False
    for term in _top_level_terms(expr):
        m = _STRING_CTOR.match(term)
        inner = m.group(1).strip() if m else term
        text = _literal_text(inner)
        if text is None:
            ident = _IDENT.match(inner)
            text = constants.get(ident.group(1)) if ident else None
        if text is None:
            parts.append("(?s:.*?)")
        else:
            parts.append(re.escape(text))
            fixed = True
    return re.compile("".join(parts) + r"\Z", re.S), fixed


class Registration:
    def __init__(self, rel: str, name: str, desc: str, schema: str):
        self.rel, self.name, self.desc, self.schema = rel, name, desc, schema


def _scan_file(rel: str, src: str, regs: list, constants: dict, errs: list) -> None:
    """One source's constants and ToolRegistry add( calls (as argument texts)."""
    try:
        lits = cat.extract_literals(src)
    except (ValueError, IndexError) as e:
        errs.append(f"{rel}: its string literals cannot be read ({e}), so its tools cannot be checked")
        return
    for lit in lits:
        constants.setdefault("", set()).add(lit.text)  # every literal, for a hole-only argument
        if not lit.calls and lit.names:
            constants.setdefault(lit.names[-1], lit.text)
    for m in _ADD_CALL.finditer(src):
        args = call_args(src, m.end() - 1)
        if args is None:
            errs.append(f"{rel}: an add( call at offset {m.start()} cannot be read")
        elif len(args) >= 3:
            regs.append((rel, args))


def _registration(rel: str, args: list, constants: dict, errs: list) -> "Registration | None":
    """The registration an add( call makes, or None (an error when its name is not known)."""
    name = _literal_text(args[0])
    ident = _IDENT.match(args[0])
    if name is None and ident:
        name = constants.get(ident.group(1))
    if name is not None:
        return Registration(rel, name, args[1], args[3] if len(args) > 3 else '"{}"')
    if (rel, args[0]) not in DYNAMIC_NAMES:
        errs.append(
            f"{rel}: a tool is registered under a name that is not a literal ({args[0]}): the twin "
            "cannot hold it; list it in DYNAMIC_NAMES with why, or name it with a literal"
        )
    return None


def scan_sources(root: str) -> "tuple[list, dict, list]":
    """The registrations of an engine-1 image's sources, the constants (identifier -> literal) of every
    source and any errors."""
    e2 = {ln.strip() for ln in (cat._read(root, ENGINE2_ONLY) or "").splitlines() if ln.strip()}
    calls, constants, errs = [], {}, []
    for d in REGISTRATION_DIRS:
        for path in sorted(glob.glob(os.path.join(root, d, "**", "*.*"), recursive=True)):
            if path.endswith((".cpp", ".h")):
                rel = os.path.relpath(path, root)
                found = []
                _scan_file(rel, cat._read(root, rel) or "", found, constants, errs)
                if rel not in e2:
                    calls += found
    regs = [r for r in (_registration(rel, args, constants, errs) for rel, args in calls) if r]
    return regs, constants, errs


def _matches(expr: str, text: str, constants: dict) -> bool:
    """The text fits the argument's pattern; an argument with no fixed text at all (a field of a
    struct, a variable) must at least be a literal the sources hold, word for word."""
    pat, fixed = pattern_of(expr, constants)
    return bool(pat.match(text)) and (fixed or text in constants.get("", set()))


def registered_names(regs: list) -> "set[str]":
    """Every name a registry add( gives a tool, dotted or not (calc has no dot)."""
    return {r.name for r in regs}


def fixture_problems(root: str, fixture: dict, scanned: tuple) -> "list[str]":
    regs, constants, errs = scanned
    errs = list(errs)
    held = set()
    by_name: dict = {}
    for r in regs:
        by_name.setdefault(r.name, []).append(r)
    for t in fixture.get("tools", []):
        name = t.get("name", "")
        held.add(name)
        if name not in by_name:
            errs.append(f"{FIXTURE}: no device source registers {name}: drop it, then {BLESS}")
            continue
        for field, attr in (("description", "desc"), ("schema", "schema")):
            text = t.get(field, "")
            if not any(_matches(getattr(r, attr), text, constants) for r in by_name[name]):
                errs.append(
                    f"{FIXTURE}: {name}'s {field} is not what the source registers: update it to the "
                    f"registered text, then {BLESS}"
                )
    for name in sorted(registered_names(regs) - held - set(NOT_LISTED)):
        errs.append(f"{FIXTURE}: the device registers {name}, which the fixture does not hold: add it, then {BLESS}")
    return errs


def run(root: str) -> "list[str]":
    text = cat._read(root, GOLDEN)
    if text is None:
        return [f"{GOLDEN} is missing: {BLESS}"]
    figures, tools, errs = parse_golden(text)
    raw = cat._read(root, FIXTURE)
    if raw is None:
        return errs + [f"{FIXTURE} is missing"]
    fixture = json.loads(raw)
    names = {t.get("name") for t in fixture.get("tools", [])} | {"orch_turn"}  # the engine adds the final-answer tool
    errs += [f"{GOLDEN}: {n} is not a tool of {FIXTURE}: re-bless with {BLESS}" for n, *_ in tools if n not in names]
    granted, aerrs = load_allowances(root)
    return errs + aerrs + ceiling_problems(figures, granted) + fixture_problems(root, fixture, scan_sources(root))


def main(argv: "list[str] | None" = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--root", default=REPO)
    args = ap.parse_args(argv)
    errs = run(args.root)
    for e in errs:
        print(f"prompt-ceiling: {e}")
    text = cat._read(args.root, GOLDEN) or ""
    figures = parse_golden(text)[0]
    granted = load_allowances(args.root)[0]
    for fig, measured in MEASURED.items():
        if fig in figures:
            ceiling = min(measured + MARGIN_BYTES + granted[fig], CAPS.get(fig, 1 << 30))
            print(f"prompt-ceiling: {fig} {figures[fig]} B, ceiling {ceiling} B, margin {ceiling - figures[fig]} B")
    print("PROMPT CEILING RED" if errs else "PROMPT CEILING GREEN")
    return 1 if errs else 0


if __name__ == "__main__":
    sys.exit(main())
