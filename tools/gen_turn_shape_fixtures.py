#!/usr/bin/env python3
"""Generate shared turn-shape fixtures from the Lumi episodic exports.

The metering, claim-check and background-turn lanes (T, S, M, P) all need to
replay real conversation SHAPES: the sequence of roles and kinds in a turn, which
rounds carried a tool call, where a blob rode along, how long each piece was.
That shape is in the Lumi episodic exports, but those also hold the owner's
personal data, which must never be committed, pasted into Linear, or turned into a
fixture unscrubbed (charter: "Lumi data is personal").

This is the ONE script that turns them into fixtures. It keeps only the shape -
role, kind, tag structure, channel, whether a blob was present, and a coarse
length bucket - and replaces every natural-language text with synthetic filler
derived only from (kind, bucket, index), never from the real bytes. System
identifiers that are not personal (tool names, round/turn tag structure) are kept
because the shape is the point; chat/session ids and blob paths are replaced with
synthetic ones. The output is deterministic, so re-running it produces an
identical fixture (a clean diff proves nothing personal crept in).

  gen_turn_shape_fixtures.py --source <dir-with-epi_*.json> --out <fixture-dir>
  gen_turn_shape_fixtures.py --verify <fixture-dir>    # re-scrub check: no personal text

The committed fixtures live in test/support/fixtures/turn_shapes_2026-10/. The
orchestrator regenerates them by pointing --source at
program-2026-10-10/investigation-2026-10-18/ (its lumi-data/ and earlier/lumi15/).
"""

from __future__ import annotations

import argparse
import hashlib
import json
import re
import sys
from pathlib import Path

# Episodic row fields we keep (shape only). `text`, `blob` and `session` are
# transformed, never copied.
KEEP_KINDS = {"message", "llm_response", "tool_output", "image", "log"}

# Coarse length buckets, so a replay reproduces "short reply" vs "fold-sized body"
# without carrying the bytes.
BUCKETS = [
    (0, "empty"),
    (1, "tiny"),
    (64, "short"),
    (512, "medium"),
    (4096, "long"),
    (16384, "huge"),
    (10**9, "oversize"),
]


def bucket(n: int) -> str:
    for lo, name in BUCKETS:
        if n <= lo:
            return name
    return "oversize"


# The exact length is carried in `text_len`; the literal is a short placeholder so
# the fixture stays small. A replay that needs the full length repeats the lorem
# body to text_len itself. The literal is lowercase letters and spaces only, so a
# single personal byte (a capital, a digit, punctuation, a newline) would fail the
# char-class guard in verify_clean.
_LEAD = {
    "message": "synthetic owner message",
    "llm_response": "synthetic model reasoning",
    "tool_output": "synthetic tool result",
    "image": "synthetic image caption",
    "log": "synthetic log line",
}


def synth_text(kind: str, buck: str, idx: int) -> str:
    """Deterministic synthetic placeholder for a (kind, bucket, index). Depends on
    NO real bytes. Lowercase letters and spaces only (plus the bracketed header)."""
    if buck == "empty":
        return ""
    lead = _LEAD.get(kind, "synthetic text")
    body = f"[{lead} number {idx}] lorem ipsum dolor sit amet"
    return body[:120]


# A tool call's shape is "name({json}) -> result": keep the tool NAME (a system
# identifier, not personal) and the fact it produced a stored-memory id, drop the
# arguments and the result text.
_TOOL = re.compile(r"^([a-z_][a-z0-9_.]*)\(")
_STORED = re.compile(r"stored memory (m[0-9a-f]+)")


def tool_shape(text: str, idx: int) -> str:
    m = _TOOL.match(text.strip())
    name = m.group(1) if m else "tool"
    stored = "\n-> stored memory m" + hashlib.sha1(f"mem{idx}".encode()).hexdigest()[:8] if _STORED.search(text) else ""
    return f'{name}({{"arg":"synthetic"}}){stored}'


# Tag values are an ALLOWLIST, never a pass-through: a key keeps its value only
# when the value has a known non-personal shape (a system identifier, an event
# word, a round number). `from:` names the sender and is always replaced; an id is
# re-derived; anything unrecognised becomes "x". verify_clean re-checks the result.
_TAG_SHAPES = {
    "tool": re.compile(r"[a-z_][a-z0-9_.]*"),
    "ev": re.compile(r"[a-z_]+"),
    "round": re.compile(r"\d+"),
    "via": re.compile(r"[a-z]+"),
    "src": re.compile(r"[a-z]+"),
}
_TAG_OK = re.compile(r"(turn|tool|ev|round|via|src):[a-z0-9_.]+|from:owner|err|[a-z_]+:x|[a-z_]+:m[0-9a-f]{8}")


def scrub_tag(part: str) -> str:
    key, sep, val = part.partition(":")
    if not re.fullmatch(r"[a-z_]+", key):
        return "x"
    if not sep:
        return key if key == "err" else "x"
    if key == "from":
        return "from:owner"
    if re.fullmatch(r"m[0-9a-f]+", val):
        return f"{key}:m" + hashlib.sha1(f"{key}{val}".encode()).hexdigest()[:8]
    shape = _TAG_SHAPES.get(key)
    return f"{key}:{val}" if shape and shape.fullmatch(val) else f"{key}:x"


def scrub_tags(tags: str, chat_map: dict) -> str:
    """Keep the tag STRUCTURE (turn:/round:/tool:/ev: keys) with allowlisted
    values only (see scrub_tag); a sender name or any unknown value never ships."""
    return ",".join(scrub_tag(p.strip()) for p in tags.split(",") if p.strip())


def synth_chat(session: str, chat_map: dict) -> str:
    if session not in chat_map:
        chat_map[session] = f"chat{len(chat_map) + 1}"
    return chat_map[session]


def scrub_rows(rows: list, chat_map: dict) -> list:
    out = []
    for i, r in enumerate(rows):
        kind = r.get("kind", "message")
        if kind not in KEEP_KINDS:
            continue
        text = r.get("text") or ""
        buck = bucket(len(text))
        if kind == "tool_output":
            new_text = tool_shape(text, i)
        else:
            new_text = synth_text(kind, buck, i)
        out.append(
            {
                "id": "m" + hashlib.sha1(f"row{i}".encode()).hexdigest()[:8],
                "chat": synth_chat(r.get("session") or r.get("sid") or "unknown", chat_map),
                "ts": int(r.get("ts", 0)),
                "role": r.get("role", "user"),
                "kind": kind,
                "text_bucket": buck,
                "text_len": len(text),
                "text": new_text,
                "has_blob": bool(r.get("blob")),
                "tags": scrub_tags(r.get("tags", ""), chat_map),
                "channel": r.get("channel", ""),
            }
        )
    return out


def load_episodic(path: Path) -> list:
    data = json.loads(path.read_text())
    if isinstance(data, dict):
        data = data.get("rows") or data.get("data") or []
    return data if isinstance(data, list) else []


# A scrubbed row is clean when its `text` is a synthetic placeholder: a non-tool
# row is the bracketed header plus lowercase lorem (letters and spaces only); a
# tool row is "<tool-name>({...}) [-> stored memory <id>]". Personal bytes cannot
# pass either pattern, so this guard fails loudly if a future edit ever copies
# real text through.
_NONTOOL_TEXT = re.compile(r"^(\[[a-z ]+number \d+\] [a-z ]*)?$")
_TOOL_TEXT = re.compile(r'^[a-z_][a-z0-9_.]*\(\{"arg":"synthetic"\}\)(\n-> stored memory m[0-9a-f]+)?$')


# The free-text fields a row may carry, and the only shapes each may take.
_CHAT = re.compile(r"chat\d+")
_ID = re.compile(r"m[0-9a-f]{8}")


def row_problems(row: dict) -> list:
    """Every field that could carry personal bytes, checked against its synthetic shape."""
    probs = []
    text = row.get("text", "")
    ok = _TOOL_TEXT.match(text) if row.get("kind") == "tool_output" else _NONTOOL_TEXT.match(text)
    if not ok:
        probs.append(f"text is not a synthetic placeholder: {text[:60]!r}")
    bad_tags = [t for t in row.get("tags", "").split(",") if t and not _TAG_OK.fullmatch(t)]
    if bad_tags:
        probs.append(f"tags carry unscrubbed values: {bad_tags[:3]}")
    if not _CHAT.fullmatch(str(row.get("chat", ""))):
        probs.append("chat is not a synthetic chat id")
    if not _ID.fullmatch(str(row.get("id", ""))):
        probs.append("id is not a synthetic row id")
    return probs


def verify_clean(fixture_dir: Path) -> int:
    bad = 0
    for f in sorted(fixture_dir.glob("*.jsonl")):
        for n, line in enumerate(f.read_text().splitlines(), 1):
            if not line.strip():
                continue
            for prob in row_problems(json.loads(line)):
                print(f"NOT CLEAN {f.name}:{n}: {prob}", file=sys.stderr)
                bad += 1
    if bad:
        return 1
    print("turn-shape fixtures: clean (synthetic text, tags, chat and row ids only)")
    return 0


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--source", help="directory holding the Lumi epi_*.json exports")
    ap.add_argument("--out", help="fixture output directory")
    ap.add_argument("--verify", help="check an existing fixture dir carries only synthetic text")
    args = ap.parse_args(argv)

    if args.verify:
        return verify_clean(Path(args.verify))
    if not args.source or not args.out:
        ap.error("--source and --out are required (or use --verify)")

    src = Path(args.source)
    exports = sorted(src.rglob("epi_*.json"))
    if not exports:
        print(f"no epi_*.json under {src}", file=sys.stderr)
        return 2
    out = Path(args.out)
    out.mkdir(parents=True, exist_ok=True)
    manifest = {"generated_by": "tools/gen_turn_shape_fixtures.py", "sources": [], "files": {}}
    for exp in exports:
        chat_map: dict = {}
        rows = scrub_rows(load_episodic(exp), chat_map)
        name = exp.parent.name + "_" + exp.stem + ".jsonl"
        (out / name).write_text("".join(json.dumps(r) + "\n" for r in rows))
        manifest["sources"].append(exp.parent.name + "/" + exp.name)
        manifest["files"][name] = {"rows": len(rows), "chats": len(chat_map)}
        print(f"{name}: {len(rows)} rows, {len(chat_map)} chats")
    (out / "MANIFEST.json").write_text(json.dumps(manifest, indent=1) + "\n")
    return verify_clean(out)


if __name__ == "__main__":
    sys.exit(main())
