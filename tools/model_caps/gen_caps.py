#!/usr/bin/env python3
"""Generate the engine-2 router's table of wire-shape facts, after linting its source.

tools/model_caps/models.toml (hand-curated, every value with its source, the date it was read
and whether it was verified) -> lib/core/include/nimbus/orch/model_caps_data.h (constexpr rows,
committed). Design: program-2026-10-18/analysis-2026-11-24/ROUTER-DESIGN.md sections 3 and 11
(layer L0).

Lints (each a failure):
  1. every value carries v, src, read and ver; ver is one of docs, code, probe, recorded, none,
     default; read is a date not in the future; src is a [sources] id (optionally "#detail") or a
     repo path with lines that exists ("lib/...cpp:12-20");
  2. every field a row needs is present after inheritance, every enum value is known, no
     unknown field, no inheritance cycle;
  3. cap_param matches the provider's dialect (Messages max_tokens, Responses max_output_tokens,
     Chat max_tokens); a Messages row's cap_mode is required;
  4. no tool_choice value the model refuses (refused_choices) is allowed;
  5. each slot has exactly one '*' row and one provider row; no duplicate (slot, kind, match);
  6. every model id compiled into the firmware, the daemon or the lab (tools/model_id_lint.py's
     scan, chat models only, matching keys and not_a_head ids left out) has a row by the
     firmware's own matching rule;
  7. every role default (model_defaults.h fallbacks) has a known row whose values were read at
     most 120 days ago;
  8. at most 128 model rows and 8,192 B of table data;
  9. with --check, the committed header equals the generator's output.
Whether every row resolves to a non-default window and a non-fallback price in the SHARED
functions is checked by test/test_model_caps, which calls them (not a copy of them).

Usage:
  python3 tools/model_caps/gen_caps.py            # lint, then write the header
  python3 tools/model_caps/gen_caps.py --check    # lint, then fail if the header is stale
  python3 tools/model_caps/gen_caps.py --report   # list every value not verified (ver none/default)
"""

from __future__ import annotations

import argparse
import datetime as dt
import re
import sys
import tomllib
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
SRC = REPO / "tools/model_caps/models.toml"
OUT = REPO / "lib/core/include/nimbus/orch/model_caps_data.h"
DEFAULTS_H = REPO / "lib/core/include/nimbus/orch/model_defaults.h"
sys.path.insert(0, str(REPO / "tools"))

MAX_MODEL_ROWS = 128
MAX_DATA_BYTES = 8192
STALE_DAYS = 120
# sizeof() of the C++ rows (model_caps.h); test_model_caps pins the same figures.
MODEL_ROW_BYTES, PROVIDER_ROW_BYTES, MARKER_ROW_BYTES = 26, 20, 8

VERS = ("docs", "code", "probe", "recorded", "none", "default")
SLOTS = ("anthropic", "openai", "mistral", "zai", "custom_chat", "custom_messages")
DIALECTS = ("messages", "responses", "chat")
DIALECT_CAP = {"messages": "max_tokens", "responses": "max_output_tokens", "chat": "max_tokens"}

ENUMS = {
    "cap_mode": ("send_always", "required", "omit", "omit_direct_ceil_routed"),
    "cap_param": ("max_tokens", "max_output_tokens"),
    "id_rule": ("passthrough", "anthropic_pattern", "mistral9"),
    "structured": ("none", "anthropic_output_config", "responses_text_format", "chat_json_schema"),
    "summarizer_structured": ("none", "anthropic_output_config", "responses_text_format", "chat_json_schema"),
    "thinking": ("off", "adaptive", "always_on", "between_tools"),
    "replay": ("none", "signed_blocks", "encrypted_items", "think_chunks", "reasoning_content"),
    "replay_scope": ("open_turn", "all_turns"),
    "cache_style": ("none", "automatic", "explicit4", "openai_breakpoints", "openai_implicit", "key_hint"),
    "usage_mode": ("input_includes_cache", "input_excludes_cache"),
}
BITS = {
    "input": ("text", "image"),
    "image_formats": ("jpeg", "png", "gif", "webp"),
    "tool_choice": ("auto", "none", "any", "required", "named"),
    "refused_choices": ("auto", "none", "any", "required", "named"),
    "effort": ("none", "minimal", "low", "medium", "high", "xhigh", "max"),
    "order_rules": (
        "ends_on_user",
        "results_first_in_user",
        "no_empty_text",
        "merge_users",
        "user_after_tool_ok",
        "not_only_system",
    ),
}
BOOLS = ("image_in_tool_result", "file_ref", "tools", "drop_under_rung", "long_cache_ok")
U16 = ("image_max_kib", "images_max", "cache_min_tok")
FLAG_BITS = ("image_in_tool_result", "file_ref", "tools", "long_cache_ok", "drop_under_rung")
MODEL_FIELDS = tuple(ENUMS) + ("input", "image_formats", "tool_choice", "effort", "order_rules") + BOOLS + U16
LINT_ONLY = ("refused_choices",)
ROW_KEYS = ("slot", "kind", "match", "inherit", "deprecated_on")

PROVIDER_FIELDS = {
    "dialect": DIALECTS,
    "hosts": "hosts",
    "base_path": "str",
    "auth": ("x_api_key", "bearer", "none"),
    "version_header": "str",
    "cache_key": ("none", "prompt_cache_key"),
    "empty_text": "str",
    "tool_frame_bytes": "u16",
    "routed_images_max": "u16",
}
MARKER_KINDS = ("code", "type", "text")
ERR_CLASSES = (
    "transport",
    "rate_limited",
    "server",
    "key_auth",
    "key_forbidden",
    "billing",
    "context",
    "refused_field",
    "unknown_model",
    "bad_request",
    "refusal",
)
MARKER_KEYS = ("slot", "kind", "text", "status", "class", "src", "read", "ver")


class Lint(Exception):
    pass


# ---- values and sources ---------------------------------------------------------------------

_PATH_SRC = re.compile(r"^([A-Za-z0-9_./-]+\.[A-Za-z0-9]+):(\d+)(?:-(\d+))?$")


def check_src(src, sources: dict, where: str, errs: list) -> None:
    if not isinstance(src, str) or not src:
        errs.append(f"{where}: src missing")
        return
    if src.split("#", 1)[0] in sources:
        return
    m = _PATH_SRC.match(src)
    if not m:
        errs.append(f"{where}: src '{src}' is neither a [sources] id nor a repo path with lines")
        return
    path = REPO / m.group(1)
    if not path.is_file():
        errs.append(f"{where}: src file {m.group(1)} does not exist")
        return
    last = int(m.group(3) or m.group(2))
    n = len(path.read_text(encoding="utf-8", errors="replace").splitlines())
    if int(m.group(2)) < 1 or last > n or last < int(m.group(2)):
        errs.append(f"{where}: src lines {m.group(2)}-{last} outside {m.group(1)} ({n} lines)")


def check_value(val, sources: dict, where: str, today: dt.date, errs: list) -> None:
    if not isinstance(val, dict) or "v" not in val:
        errs.append(f"{where}: not a value table {{v, src, read, ver}}")
        return
    extra = set(val) - {"v", "src", "read", "ver", "note"}
    if extra:
        errs.append(f"{where}: unknown keys {sorted(extra)}")
    check_src(val.get("src"), sources, where, errs)
    check_read(val.get("read"), where, today, errs)
    if val.get("ver") not in VERS:
        errs.append(f"{where}: ver '{val.get('ver')}' is not one of {', '.join(VERS)}")


def check_read(read, where: str, today: dt.date, errs: list) -> None:
    try:
        d = dt.date.fromisoformat(read if isinstance(read, str) else "")
    except ValueError:
        errs.append(f"{where}: read '{read}' is not a date (YYYY-MM-DD)")
        return
    if d > today:
        errs.append(f"{where}: read {d} is in the future")


# ---- rows -----------------------------------------------------------------------------------


def resolve(row: dict, templates: dict, where: str, errs: list, seen=()) -> dict:
    """The row's fields after inheritance (the row's own win)."""
    name = row.get("inherit")
    base = {}
    if name is not None:
        if name in seen:
            errs.append(f"{where}: inheritance cycle through '{name}'")
            return dict(row)
        if name not in templates:
            errs.append(f"{where}: unknown template '{name}'")
        else:
            base = resolve(templates[name], templates, f"template {name}", errs, seen + (name,))
    out = dict(base)
    out.update({k: v for k, v in row.items() if k != "inherit"})
    return out


def check_field(field: str, v, where: str, errs: list) -> None:
    if field in ENUMS:
        ok = v in ENUMS[field]
    elif field in BITS:
        ok = isinstance(v, list) and all(x in BITS[field] for x in v) and len(set(v)) == len(v)
    elif field in BOOLS:
        ok = isinstance(v, bool)
    elif field in U16:
        ok = isinstance(v, int) and not isinstance(v, bool) and 0 <= v <= 65535
    else:
        ok = False
    if not ok:
        errs.append(f"{where}: {field} = {v!r} is not a valid value")


def model_where(row: dict) -> str:
    return f"model {row.get('slot')}/{row.get('match') or '*'}"


def check_model(row: dict, sources: dict, providers: dict, today: dt.date, errs: list) -> None:
    where = model_where(row)
    if row.get("slot") not in SLOTS:
        errs.append(f"{where}: unknown slot")
        return
    if row.get("kind") not in ("exact", "prefix", "any"):
        errs.append(f"{where}: kind must be exact, prefix or any")
    if (row.get("kind") == "any") != (row.get("match") == ""):
        errs.append(f"{where}: an 'any' row has match \"\" and only it does")
    unknown = set(row) - set(MODEL_FIELDS) - set(LINT_ONLY) - set(ROW_KEYS)
    if unknown:
        errs.append(f"{where}: unknown fields {sorted(unknown)}")
    for f in MODEL_FIELDS + LINT_ONLY:
        if f not in row:
            if f not in LINT_ONLY:
                errs.append(f"{where}: {f} missing")
            continue
        check_value(row[f], sources, f"{where} {f}", today, errs)
        if isinstance(row[f], dict) and "v" in row[f]:
            check_field(f, row[f]["v"], f"{where}", errs)
    check_model_rules(row, providers, where, errs)


def v(row: dict, f: str, default=None):
    x = row.get(f)
    return x.get("v", default) if isinstance(x, dict) else default


def check_model_rules(row: dict, providers: dict, where: str, errs: list) -> None:
    prov = providers.get(row.get("slot"))
    if prov is None:
        return
    dialect = v(prov, "dialect")
    want = DIALECT_CAP.get(dialect)
    if want and v(row, "cap_param") != want:
        errs.append(f"{where}: cap_param {v(row, 'cap_param')} does not match the {dialect} dialect ({want})")
    if dialect == "messages" and v(row, "cap_mode") != "required":
        errs.append(f"{where}: the messages dialect requires max_tokens (cap_mode required)")
    banned = set(v(row, "refused_choices", []) or []) & set(v(row, "tool_choice", []) or [])
    if banned:
        errs.append(f"{where}: tool_choice allows {sorted(banned)}, which the model refuses")
    if v(row, "tools") and "auto" not in (v(row, "tool_choice", []) or []):
        errs.append(f"{where}: a row with tools must allow tool_choice auto")


def provider_value_ok(kind, x) -> bool:
    if kind == "str":
        return isinstance(x, str)
    if kind == "u16":
        return isinstance(x, int) and not isinstance(x, bool) and 0 <= x <= 65535
    if kind == "hosts":
        return isinstance(x, list) and len(x) <= 2 and all(isinstance(h, str) and h for h in x)
    return isinstance(x, str) and x in kind


def check_provider(row: dict, sources: dict, today: dt.date, errs: list) -> None:
    where = f"provider {row.get('slot')}"
    if row.get("slot") not in SLOTS:
        errs.append(f"{where}: unknown slot")
    unknown = set(row) - set(PROVIDER_FIELDS) - {"slot"}
    if unknown:
        errs.append(f"{where}: unknown fields {sorted(unknown)}")
    for f, kind in PROVIDER_FIELDS.items():
        if f not in row:
            errs.append(f"{where}: {f} missing")
            continue
        check_value(row[f], sources, f"{where} {f}", today, errs)
        if not provider_value_ok(kind, v(row, f)):
            x = v(row, f)
            errs.append(f"{where}: {f} = {x!r} is not a valid value")


def check_marker(m: dict, sources: dict, today: dt.date, errs: list) -> None:
    where = f"marker {m.get('slot')}/{m.get('text')}"
    if set(m) != set(MARKER_KEYS):
        errs.append(f"{where}: keys must be exactly {', '.join(MARKER_KEYS)}")
    if m.get("slot") not in SLOTS or m.get("kind") not in MARKER_KINDS or m.get("class") not in ERR_CLASSES:
        errs.append(f"{where}: unknown slot, kind or class")
    if not isinstance(m.get("text"), str) or not m.get("text"):
        errs.append(f"{where}: text missing")
    if m.get("kind") == "text" and m.get("text") != str(m.get("text")).lower():
        errs.append(f"{where}: a text marker is written in lower case (it matches case-insensitively)")
    if not isinstance(m.get("status"), int) or not 0 <= m["status"] <= 999:
        errs.append(f"{where}: status must be 0 (any) or an HTTP status")
    check_src(m.get("src"), sources, where, errs)
    check_read(m.get("read"), where, today, errs)
    if m.get("ver") not in VERS:
        errs.append(f"{where}: ver '{m.get('ver')}' is not one of {', '.join(VERS)}")


# ---- the tree's model ids ----------------------------------------------------------------------


def is_version_suffix(s: str) -> bool:
    import model_id_lint

    return model_id_lint.is_version_suffix(s)


def row_for(rows: list, slot: str, model: str):
    """The firmware's lookup (orch_model_caps.cpp): exact (also with one version suffix), then
    the longest prefix, then the slot's '*' row."""
    m = model.lower()
    best, best_len, anyrow = None, -1, None
    for r in rows:
        if r["slot"] != slot:
            continue
        mid = r["match"].lower()
        if r["kind"] == "any":
            anyrow = r
        elif r["kind"] == "exact" and m.startswith(mid) and (m == mid or is_version_suffix(m[len(mid) :])):
            return r
        elif r["kind"] == "prefix" and m.startswith(mid) and len(mid) > best_len:
            best, best_len = r, len(mid)
    return best or anyrow


def compiled_ids(not_a_head: set) -> "list[tuple[str, str, str]]":
    """(provider, id, first file) of every chat model id the tree sends."""
    import model_id_lint as lint

    keys = {(e["id"], e["file"]) for e in lint.load_allowlist().get("keys", [])}
    out = {}
    for h in lint.scan_tree():
        if lint.NON_CHAT.search(h.model) or (h.model, h.file) in keys or h.model in not_a_head:
            continue
        out.setdefault((h.provider, h.model), f"{h.file}:{h.line}")
    return [(p, m, f) for (p, m), f in sorted(out.items())]


_ROLE_ROW = re.compile(r'\{"([a-z]+)",\s*DefaultRole::\w+,\s*"[^"]*",\s*"[^"]*",\s*"([^"]+)"\}')


def role_defaults(text: str) -> "list[tuple[str, str]]":
    """(slot, fallback model) of every kRoleDefaults row; a cumulo row is read on its upstream
    (a bare router id is an openai model, model_key.h inferUpstream)."""
    out = []
    for slot, model in _ROLE_ROW.findall(text):
        if slot == "cumulo":
            slot, model = model.split("/", 1) if "/" in model else ("openai", model)
        out.append((slot, model))
    return sorted(set(out))


def check_tree(rows: list, data: dict, today: dt.date, errs: list, ids=None, defaults_text=None) -> None:
    not_a_head = {e["id"] for e in data.get("not_a_head", [])}
    for prov, model, where in compiled_ids(not_a_head) if ids is None else ids:
        r = row_for(rows, prov, model)
        if r is None or r["kind"] == "any":
            errs.append(f"compiled model id {prov}/{model} ({where}) has no row in models.toml")
    text = DEFAULTS_H.read_text() if defaults_text is None else defaults_text
    for slot, model in role_defaults(text):
        r = row_for(rows, slot, model)
        if r is None or r["kind"] == "any":
            errs.append(f"role default {slot}/{model} has no row in models.toml")
            continue
        for f in MODEL_FIELDS:
            read = r.get(f, {}).get("read") if isinstance(r.get(f), dict) else None
            try:
                age = (today - dt.date.fromisoformat(read)).days
            except (TypeError, ValueError):
                continue
            if age > STALE_DAYS:
                errs.append(f"role default {slot}/{model}: {f} was read {age} days ago (over {STALE_DAYS}); re-read it")


# ---- the whole file ------------------------------------------------------------------------------


def load(path: Path = SRC) -> dict:
    with open(path, "rb") as fh:
        return tomllib.load(fh)


def check_sources(sources: dict, errs: list) -> None:
    for k, s in sources.items():
        if not isinstance(s, str) or not s.startswith(("https://", "cumulo-nimbus/", "program-2026-10-18")):
            errs.append(f"source {k}: must be an https URL or a named project document")


def check_providers(provs: list, sources: dict, today: dt.date, errs: list) -> dict:
    by_slot = {}
    for p in provs:
        check_provider(p, sources, today, errs)
        if p.get("slot") in by_slot:
            errs.append(f"provider {p.get('slot')}: listed twice")
        by_slot[p.get("slot")] = p
    errs += [f"provider {s}: missing" for s in SLOTS if s not in by_slot]
    return by_slot


def check_models(rows: list, sources: dict, by_slot: dict, today: dt.date, errs: list) -> None:
    seen = set()
    for r in rows:
        check_model(r, sources, by_slot, today, errs)
        key = (r.get("slot"), r.get("kind"), str(r.get("match", "")).lower())
        if key in seen:
            errs.append(f"{model_where(r)}: listed twice")
        seen.add(key)
    for s in SLOTS:
        n = sum(1 for r in rows if r.get("slot") == s and r.get("kind") == "any")
        if n != 1:
            errs.append(f"slot {s}: needs exactly one '*' (kind any) row, has {n}")
    if len(rows) > MAX_MODEL_ROWS:
        errs.append(f"{len(rows)} model rows (limit {MAX_MODEL_ROWS})")


def lint(data: dict, today: dt.date, tree=True, ids=None, defaults_text=None) -> "tuple[list, list, list]":
    """(errors, resolved model rows, provider rows in slot order)."""
    errs: list = []
    if data.get("schema") != 1:
        errs.append("schema must be 1")
    sources = data.get("sources", {})
    check_sources(sources, errs)
    by_slot = check_providers(data.get("provider", []), sources, today, errs)
    templates = data.get("template", {})
    rows = [resolve(r, templates, model_where(r), errs) for r in data.get("model", [])]
    check_models(rows, sources, by_slot, today, errs)
    for m in data.get("marker", []):
        check_marker(m, sources, today, errs)
    if tree and not errs:
        check_tree(rows, data, today, errs, ids, defaults_text)
    return errs, rows, [by_slot[s] for s in SLOTS if s in by_slot]


# ---- the header --------------------------------------------------------------------------------


class Pool:
    def __init__(self):
        self.data = bytearray(b"\0")
        self.at = {"": 0}

    def add(self, s: str) -> int:
        if s not in self.at:
            raw = s.encode("utf-8")
            if b"\0" in raw or not raw.isascii():
                raise Lint(f"pool string {s!r} must be printable ASCII")
            self.at[s] = len(self.data)
            self.data += raw + b"\0"
        if self.at[s] > 65535:
            raise Lint("string pool over 64 KiB")
        return self.at[s]


def camel(s: str) -> str:
    return "".join(p[:1].upper() + p[1:] for p in s.split("_"))


ENUM_CXX = {
    "cap_mode": ("CapMode", {"omit_direct_ceil_routed": "OmitDirectCeilRouted"}),
    "cap_param": ("CapParam", {}),
    "id_rule": ("IdRule", {"mistral9": "Mistral9"}),
    "structured": ("Structured", {}),
    "summarizer_structured": ("Structured", {}),
    "thinking": ("Thinking", {}),
    "replay": ("Replay", {}),
    "replay_scope": ("ReplayScope", {}),
    "cache_style": ("CacheStyle", {"explicit4": "Explicit4"}),
    "usage_mode": ("UsageMode", {}),
}
SLOT_CXX = {
    "anthropic": "Anthropic",
    "openai": "OpenAI",
    "mistral": "Mistral",
    "zai": "Zai",
    "custom_chat": "CustomChat",
    "custom_messages": "CustomMessages",
}


def enum_cxx(field: str, value: str) -> str:
    typ, special = ENUM_CXX[field]
    return f"{typ}::{special.get(value, camel(value))}"


def bits(field: str, values: list) -> int:
    return sum(1 << BITS[field].index(x) for x in values)


def flags(row: dict) -> int:
    return sum(1 << i for i, f in enumerate(FLAG_BITS) if v(row, f))


def model_line(r: dict, pool: Pool) -> str:
    kind = {"exact": "Exact", "prefix": "Prefix", "any": "Any"}[r["kind"]]
    parts = [
        str(pool.add(r["match"])),
        f"Slot::{SLOT_CXX[r['slot']]}",
        f"MatchKind::{kind}",
        enum_cxx("cap_mode", v(r, "cap_mode")),
        enum_cxx("cap_param", v(r, "cap_param")),
        str(bits("input", v(r, "input"))),
        str(bits("image_formats", v(r, "image_formats"))),
        str(v(r, "image_max_kib")),
        str(v(r, "images_max")),
        str(flags(r)),
        str(bits("tool_choice", v(r, "tool_choice"))),
        enum_cxx("id_rule", v(r, "id_rule")),
        enum_cxx("structured", v(r, "structured")),
        enum_cxx("summarizer_structured", v(r, "summarizer_structured")),
        enum_cxx("thinking", v(r, "thinking")),
        str(bits("effort", v(r, "effort"))),
        enum_cxx("replay", v(r, "replay")),
        enum_cxx("replay_scope", v(r, "replay_scope")),
        enum_cxx("cache_style", v(r, "cache_style")),
        str(v(r, "cache_min_tok")),
        enum_cxx("usage_mode", v(r, "usage_mode")),
        str(bits("order_rules", v(r, "order_rules"))),
    ]
    return f"    {{{', '.join(parts)}}},  // {r['slot']}/{r['match'] or '*'}"


def provider_line(p: dict, set_index: int, pool: Pool) -> str:
    hosts = v(p, "hosts") + ["", ""]
    dialect = camel(v(p, "dialect"))
    auth = {"x_api_key": "XApiKey", "bearer": "Bearer", "none": "None"}[v(p, "auth")]
    parts = [
        f"Slot::{SLOT_CXX[p['slot']]}",
        f"Dialect::{dialect}",
        f"Auth::{auth}",
        f"CacheKey::{camel(v(p, 'cache_key'))}",
        str(pool.add(hosts[0])),
        str(pool.add(hosts[1])),
        str(pool.add(v(p, "base_path"))),
        str(pool.add(v(p, "version_header"))),
        str(pool.add(v(p, "empty_text"))),
        str(v(p, "tool_frame_bytes")),
        str(v(p, "routed_images_max")),
        str(set_index),
        "0",
    ]
    return f"    {{{', '.join(parts)}}},  // {p['slot']}"


def marker_line(m: dict, set_index: int, pool: Pool) -> str:
    parts = [
        str(set_index),
        f"MarkerKind::{camel(m['kind'])}",
        f"ErrClass::{camel(m['class'])}",
        "0",
        str(m["status"]),
        str(pool.add(m["text"])),
    ]
    return f"    {{{', '.join(parts)}}},  // {m['slot']} {m['kind']} {m['text']}"


def cxx_string(data: bytes) -> "list[str]":
    """The pool as adjacent string literals, one per entry (the trailing NUL is the literal's)."""
    out = []
    for piece in bytes(data[1:]).split(b"\0")[:-1]:
        out.append('    "' + piece.decode().replace("\\", "\\\\").replace('"', '\\"') + '\\0"')
    return out


def render(data: dict, rows: list, provs: list) -> "tuple[str, int]":
    """(header text, table data bytes)."""
    pool = Pool()
    set_of = {p["slot"]: i for i, p in enumerate(provs)}
    model_lines = [model_line(r, pool) for r in rows]
    prov_lines = [provider_line(p, set_of[p["slot"]], pool) for p in provs]
    markers = sorted(data.get("marker", []), key=lambda m: set_of[m["slot"]])
    marker_lines = [marker_line(m, set_of[m["slot"]], pool) for m in markers]
    size = (
        len(rows) * MODEL_ROW_BYTES
        + len(provs) * PROVIDER_ROW_BYTES
        + len(markers) * MARKER_ROW_BYTES
        + len(pool.data)
        + 1  # the literal's own terminating NUL
    )
    lines = [
        "#pragma once",
        "// GENERATED by tools/model_caps/gen_caps.py from tools/model_caps/models.toml.",
        "// Do not edit by hand: edit the TOML and run the generator (AGENTS.md section 4).",
        "// constexpr rows in flash; read through nimbus/orch/model_caps.h.",
        '#include "nimbus/orch/model_caps.h"',
        "",
        "namespace nimbus {",
        "namespace orch {",
        "namespace caps {",
        "",
        f"inline constexpr size_t kCapsDataBytes = {size};   // rows and pool, as gen_caps.py counts them",
        "",
        "// The string pool (offset 0 is the empty string).",
        "inline constexpr char kCapsPool[] =",
        '    "\\0"',
        *cxx_string(pool.data),
        "    ;",
        "",
        "inline constexpr ModelRow kCapsModels[] = {",
        *model_lines,
        "};",
        "",
        "inline constexpr ProviderRow kCapsProviders[] = {",
        *prov_lines,
        "};",
        "",
        "inline constexpr MarkerRow kCapsMarkers[] = {",
        *marker_lines,
        "};",
        "",
        "}  // namespace caps",
        "}  // namespace orch",
        "}  // namespace nimbus",
        "",
    ]
    return "\n".join(lines), size


# ---- report ------------------------------------------------------------------------------------


def unverified(data: dict, rows: list) -> "list[str]":
    out = []
    for p in data.get("provider", []):
        for f in PROVIDER_FIELDS:
            x = p.get(f, {})
            if x.get("ver") in ("none", "default"):
                out.append(f"provider {p['slot']} {f} = {x.get('v')!r} ({x.get('ver')}; {x.get('note', '')})")
    for r in rows:
        for f in MODEL_FIELDS + LINT_ONLY:
            x = r.get(f)
            if isinstance(x, dict) and x.get("ver") in ("none", "default"):
                out.append(f"{model_where(r)} {f} = {x.get('v')!r} ({x.get('ver')}; {x.get('note', '')})")
    for m in data.get("marker", []):
        if m.get("ver") in ("none", "default"):
            out.append(f"marker {m['slot']} {m['kind']} {m['text']} -> {m['class']} ({m['ver']})")
    return out


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n", 1)[0])
    ap.add_argument("--check", action="store_true", help="fail when the committed header is stale")
    ap.add_argument("--report", action="store_true", help="list every value not verified")
    ap.add_argument("--today", help="the date staleness is judged on (default: today)")
    args = ap.parse_args(argv)
    today = dt.date.fromisoformat(args.today) if args.today else dt.date.today()
    data = load()
    errs, rows, provs = lint(data, today)
    if not errs:
        try:
            text, size = render(data, rows, provs)
        except Lint as e:
            errs.append(str(e))
        else:
            if size > MAX_DATA_BYTES:
                errs.append(f"table data {size} B (limit {MAX_DATA_BYTES} B)")
    if errs:
        for e in errs:
            print(f"FAIL {e}")
        print(f"model caps: {len(errs)} lint failure(s)")
        return 1
    if args.report:
        for line in unverified(data, rows):
            print(line)
    if args.check:
        if not OUT.exists() or OUT.read_text() != text:
            print(f"FAIL {OUT} is stale: run python3 tools/model_caps/gen_caps.py")
            return 1
    else:
        OUT.write_text(text)
    nk = sum(1 for r in rows if r["kind"] != "any")
    print(
        f"model caps: clean ({len(provs)} providers, {len(rows)} model rows of which {nk} named, "
        f"{len(data.get('marker', []))} markers, {size} B of table data)"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
