#!/usr/bin/env python3
"""Model-id lint: compiled-in model ids must exist upstream and have a price.

Model ids compiled into the firmware, the hosted daemon (nimbusd) and the live
lab drift: a provider retires a model, renames it, or ships a new one with no
built-in price, and the code keeps sending the old string. This lint catches
that class at commit time instead of on a device.

Rules (each id is a whole model id found in a C/C++ string literal; comments are
ignored):
  1. Active: the id is in the recorded provider model list
     (test/support/fixtures/models_2026-10/<provider>.json). An undated
     Anthropic alias counts when a dated id extends it (claude-x-y-YYYYMMDD);
     a Mistral alias counts when a listed model names it in `aliases`.
  2. Priced: a chat model has a model row in the built-in price table
     (lib/core/include/nimbus/orch/model_price_defaults.h), matched with the
     firmware's own rule: the longest family the id is, or extends by exactly
     one version suffix. Embedding, speech, image, moderation and OCR models
     need only rule 1.
  3. Not retired by policy: ids on the allowlist file's `retired` list (owner
     rulings, passed retirement floors) are bad even while still listed.
  4. Every price-table family must itself match an active id.
An offending (id, file) pair passes only with an allowlist entry that names the
lane that removes it, or a `keys` entry for a literal that is only ever compared
against (never sent). An allowlist entry that no longer matches an offence is
stale and fails too, so the list only shrinks.

Usage:
  python3 tools/model_id_lint.py                 # lint the tree (exit 1 on findings)
  python3 tools/model_id_lint.py --list          # every id found, with its verdict
  python3 tools/model_id_lint.py --record-fixtures [DIR]
      re-record the four provider model lists (keys by name from the environment
      or NIMBUS_ENV_FILE; nothing is printed but counts)
"""

from __future__ import annotations

import argparse
import json
import os
import re
import sys
import urllib.request
from dataclasses import dataclass, field
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
FIXTURE_DIR = REPO / "test/support/fixtures/models_2026-10"
PRICE_HEADER = REPO / "lib/core/include/nimbus/orch/model_price_defaults.h"
ALLOWLIST = REPO / "tools/model_id_allowlist.json"
PROVIDERS = ("anthropic", "openai", "mistral", "zai")

# Where compiled-in ids live. Generated copies are skipped (the web page header in
# nimbusd duplicates include/web; the docs pack is documentation text).
SCAN_ROOTS = (
    "src",
    "lib/core",
    "lib/harness",
    "include",
    "nimbusd/src",
    "nimbusd/scenarios",
    "tools/harness-lab/src",
)
SKIP_FILES = {
    "nimbusd/src/webui_page.h",
    "lib/core/include/nimbus/docs_pack_data.h",
}
SOURCE_SUFFIXES = (".c", ".cc", ".cpp", ".h", ".hpp")

# Whole-id shapes per provider. Bare family prefixes ("claude-opus", "gpt-5",
# "glm-4", "claude-3") are matching keys in classification code, not ids, and do
# not match these shapes on purpose.
_DATE = r"(?:-\d{8}|-\d{4}-\d{2}-\d{2})"
ID_SHAPES = (
    ("anthropic", re.compile(r"claude-(?:opus|sonnet|haiku|fable|mythos)-\d+(?:-\d+)?" + _DATE + r"?")),
    ("anthropic", re.compile(r"claude-\d(?:-\d)?-(?:opus|sonnet|haiku)(?:-latest|" + _DATE + r")?")),
    ("openai", re.compile(r"gpt-\d+(?:\.\d+)?o?(?:-[a-z0-9]+)+" + _DATE + r"?")),
    ("openai", re.compile(r"gpt-\d+\.\d+o?")),
    ("openai", re.compile(r"gpt-\d+o")),
    ("openai", re.compile(r"o\d(?:-[a-z]+)+" + _DATE + r"?")),
    ("openai", re.compile(r"text-embedding-(?:3-small|3-large|ada-002)")),
    ("openai", re.compile(r"(?:tts-1(?:-hd)?|whisper-1|dall-e-[23]|omni-moderation-latest)")),
    (
        "mistral",
        re.compile(
            r"(?:mistral|magistral|ministral|codestral|devstral|pixtral|voxtral)-"
            r"(?:[a-z]+-)*(?:\d+(?:\.\d+)?[a-z]*|latest)(?:-[a-z0-9]+)*"
        ),
    ),
    ("mistral", re.compile(r"(?:mistral|codestral)-embed")),
    ("zai", re.compile(r"glm-\d+\.\d+(?:-[a-z0-9]+)*|glm-\d+-[a-z0-9]+(?:-[a-z0-9]+)*")),
)
# Token delimiters inside a literal: lists, router selectors, prose.
_SPLIT = re.compile(r"[\s,;|/:\"'()=<>\[\]{}`*]+")
# A token that only looks like an id inside a longer word stays out ("mistral-builtins").
NON_CHAT = re.compile(r"(embed|tts|transcribe|whisper|ocr|moderation|image|dall-e|realtime|voxtral)")


@dataclass(frozen=True)
class Hit:
    model: str
    provider: str
    file: str
    line: int


@dataclass
class Finding:
    model: str
    file: str
    line: int
    reasons: list = field(default_factory=list)


# ---- C/C++ string literal extraction -----------------------------------------


def _skip_comment(text: str, i: int) -> int:
    if text.startswith("//", i):
        j = text.find("\n", i)
        return len(text) if j < 0 else j
    j = text.find("*/", i + 2)
    return len(text) if j < 0 else j + 2


def _read_raw(text: str, i: int) -> tuple[str, int]:
    """i points at the opening quote of R"delim( ... )delim"."""
    paren = text.find("(", i)
    delim = text[i + 1 : paren]
    end = text.find(")" + delim + '"', paren)
    end = len(text) if end < 0 else end
    return text[paren + 1 : end], end + len(delim) + 2


def _read_quoted(text: str, i: int, quote: str) -> tuple[str, int]:
    out = []
    j = i + 1
    while j < len(text) and text[j] != quote:
        if text[j] == "\\" and j + 1 < len(text):
            out.append(text[j : j + 2])
            j += 2
            continue
        if text[j] == "\n":  # unterminated: stop at the line end
            break
        out.append(text[j])
        j += 1
    return "".join(out), j + 1


def string_literals(text: str) -> list[tuple[int, str]]:
    """(line, content) of every string literal outside comments."""
    lits = []
    i = 0
    n = len(text)
    while i < n:
        c = text[i]
        if c == "/" and i + 1 < n and text[i + 1] in "/*":
            i = _skip_comment(text, i)
            continue
        if c == "'":
            _, i = _read_quoted(text, i, "'")
            continue
        if c == '"':
            raw = i > 0 and text[i - 1] == "R" and (i < 2 or not (text[i - 2].isalnum() or text[i - 2] == "_"))
            line = text.count("\n", 0, i) + 1
            content, i = _read_raw(text, i) if raw else _read_quoted(text, i, '"')
            lits.append((line, content))
            continue
        i += 1
    return lits


def ids_in_literal(s: str) -> list[tuple[str, str, int]]:
    """(model, provider, newlines before it in the literal) of every id token."""
    found = []
    low = s.lower()
    pos = 0
    for tok in _SPLIT.split(low):
        at = low.find(tok, pos) if tok else pos
        pos = at + len(tok)
        tok = tok.strip(".-")
        for provider, rx in ID_SHAPES:
            if rx.fullmatch(tok):
                found.append((tok, provider, low.count("\n", 0, max(at, 0))))
                break
    return found


def scan_tree(repo: Path = REPO, roots=SCAN_ROOTS) -> list[Hit]:
    hits = []
    for root in roots:
        base = repo / root
        if not base.exists():
            continue
        for p in sorted(base.rglob("*")):
            rel = p.relative_to(repo).as_posix()
            if not p.is_file() or p.suffix not in SOURCE_SUFFIXES or rel in SKIP_FILES:
                continue
            text = p.read_text(encoding="utf-8", errors="replace")
            for line, lit in string_literals(text):
                for model, provider, dl in ids_in_literal(lit):
                    hits.append(Hit(model, provider, rel, line + dl))
    return hits


# ---- the reference data --------------------------------------------------------


def load_active(fixture_dir: Path = FIXTURE_DIR) -> dict:
    """provider -> {"ids": set, "aliases": set, "deprecated": set}.

    `aliases` also takes the probed aliases (aliases_probed.json): ids a provider
    serves but does not list, each with the recorded request that proved it (the
    model the response echoed)."""
    out = {}
    for prov in PROVIDERS:
        data = json.loads((fixture_dir / f"{prov}.json").read_text())
        ids, aliases, deprecated = set(), set(), set()
        for m in data.get("data", []):
            mid = m.get("id")
            if not mid:
                continue
            ids.add(mid)
            aliases.update(a for a in (m.get("aliases") or []) if isinstance(a, str))
            if m.get("deprecated_at") or m.get("retires_at") or m.get("deprecation"):
                deprecated.add(mid)
        out[prov] = {"ids": ids, "aliases": aliases, "deprecated": deprecated}
    probed = fixture_dir / "aliases_probed.json"
    if probed.exists():
        for a in json.loads(probed.read_text()).get("probed", []):
            if a.get("provider") in out and a.get("served"):
                out[a["provider"]]["aliases"].add(a["id"])
    return out


_PRICE_ROW = re.compile(r'\{\s*"([a-z_]+)"\s*,\s*"([^"]+)"\s*,')


def load_prices(header: Path = PRICE_HEADER) -> list[tuple[str, str, int]]:
    """(slot, family, line) of every model row (the '*' provider defaults excluded)."""
    rows = []
    in_table = False
    for n, line in enumerate(header.read_text().splitlines(), 1):
        if "kBuiltinPrices[]" in line:
            in_table = True
        elif in_table and line.strip().startswith("};"):
            break
        elif in_table:
            m = _PRICE_ROW.search(line)
            if m and m.group(2) != "*":
                rows.append((m.group(1), m.group(2), n))
    return rows


def is_version_suffix(s: str) -> bool:
    """Port of nimbus::orch::isVersionSuffix (lib/core/src/orch_model_key.cpp)."""
    if not s.startswith("-"):
        return False
    body = s[1:]
    n = len(s)
    if n == 5 and body.isdigit():
        return True  # -NNNN build
    if s == "-latest":
        return True
    if n == 8 and re.fullmatch(r"20\d{2}-\d{2}", body):
        return True  # -20YY-MM
    if n == 9 and body.isdigit():
        return True  # -YYYYMMDD
    if n == 11 and re.fullmatch(r"\d{4}-\d{2}-\d{2}", body):
        return True  # -YYYY-MM-DD
    return n >= 3 and body[0] == "v" and body[1:].isdigit()  # -vN


def family_matches(model: str, family: str) -> bool:
    return model == family or (model.startswith(family) and is_version_suffix(model[len(family) :]))


def price_row(provider: str, model: str, prices) -> tuple | None:
    best = None
    for slot, fam, line in prices:
        if slot == provider and family_matches(model, fam) and (best is None or len(fam) > len(best[1])):
            best = (slot, fam, line)
    return best


def is_active(provider: str, model: str, active: dict) -> bool:
    ref = active[provider]
    if model in ref["ids"] or model in ref["aliases"]:
        return True
    if provider == "anthropic":
        return any(re.fullmatch(re.escape(model) + r"-\d{8}", i) for i in ref["ids"])
    return False


def is_deprecated(provider: str, model: str, active: dict) -> bool:
    ref = active[provider]
    return any(family_matches(d, model) or d == model for d in ref["deprecated"])


def retired_reason(model: str, retired: list) -> str | None:
    for r in retired:
        if family_matches(model, r["id"]):
            return r["why"]
    return None


# ---- the verdict ----------------------------------------------------------------


def judge_hit(hit: Hit, active: dict, prices, retired: list) -> list[str]:
    reasons = []
    if not is_active(hit.provider, hit.model, active):
        reasons.append(f"not in the recorded {hit.provider} model list")
    elif is_deprecated(hit.provider, hit.model, active):
        reasons.append(f"deprecated in the recorded {hit.provider} model list")
    why = retired_reason(hit.model, retired)
    if why:
        reasons.append(f"retired by policy: {why}")
    if not NON_CHAT.search(hit.model) and price_row(hit.provider, hit.model, prices) is None:
        reasons.append("no built-in price row")
    return reasons


def judge_price_rows(prices, active: dict, header_rel: str) -> list[Finding]:
    """Rule 4. A retired model keeps its price row: a device still configured with
    it must not be mis-priced, so the retired list never applies here."""
    out = []
    for slot, fam, line in prices:
        if slot not in active:
            continue
        reasons = []
        refs = active[slot]["ids"] | active[slot]["aliases"]
        if not any(family_matches(i, fam) for i in refs):
            reasons.append(f"price family matches no id in the recorded {slot} model list")
        if reasons:
            out.append(Finding(fam, header_rel, line, reasons))
    return out


def load_allowlist(path: Path = ALLOWLIST) -> dict:
    """`allow`: known offences, each naming the lane that removes it.
    `keys`: literals that name a model only to match against it (a size table, a
    context-window family) and are never sent; each says why.
    `retired`: ids that are bad by policy even while a provider still lists them."""
    data = json.loads(path.read_text())
    need = {"allow": {"id", "file", "lane", "why"}, "keys": {"id", "file", "why"}, "retired": {"id", "why"}}
    for section, fields in need.items():
        for e in data.get(section, []):
            missing = fields - set(e)
            if missing:
                raise ValueError(f"{section} entry {e} lacks {sorted(missing)}")
    return data


def lint(
    repo: Path = REPO, fixture_dir: Path = FIXTURE_DIR, allow_path: Path = ALLOWLIST, roots=SCAN_ROOTS
) -> tuple[list[Finding], list[dict], list[Hit]]:
    """Returns (unallowed findings, stale allowlist entries, every hit)."""
    active = load_active(fixture_dir)
    prices = load_prices(repo / PRICE_HEADER.relative_to(REPO))
    allow = load_allowlist(allow_path)
    retired = allow.get("retired", [])
    hits = scan_tree(repo, roots)

    findings: dict[tuple[str, str], Finding] = {}
    header_rel = PRICE_HEADER.relative_to(REPO).as_posix()
    for h in hits:
        if h.file == header_rel:  # price families: rule 4 below, not rules 1-3
            continue
        reasons = judge_hit(h, active, prices, retired)
        if reasons:
            key = (h.model, h.file)
            findings.setdefault(key, Finding(h.model, h.file, h.line, reasons))
    for f in judge_price_rows(prices, active, header_rel):
        findings.setdefault((f.model, f.file), f)

    entries = allow.get("allow", []) + allow.get("keys", [])
    allowed = {(e["id"], e["file"]) for e in entries}
    unallowed = [f for k, f in sorted(findings.items()) if k not in allowed]
    stale = [e for e in entries if (e["id"], e["file"]) not in findings]
    return unallowed, stale, hits


# ---- fixture recording ------------------------------------------------------------


def _env_key(name: str) -> str:
    if os.environ.get(name):
        return os.environ[name]
    path = os.environ.get("NIMBUS_ENV_FILE", str(REPO / ".env"))
    try:
        for line in Path(path).read_text().splitlines():
            line = line.strip()
            if line.startswith("export "):
                line = line[7:]
            if line.startswith(name + "="):
                return line.split("=", 1)[1].strip().strip("\"'")
    except OSError:
        pass
    return ""


def record_fixtures(out_dir: Path) -> int:
    reqs = {
        "anthropic": (
            "https://api.anthropic.com/v1/models?limit=1000",
            {"x-api-key": "ANTHROPIC_API_KEY", "anthropic-version": "2023-06-01"},
        ),
        "openai": ("https://api.openai.com/v1/models", {"Authorization": "OPENAI_API_KEY"}),
        "mistral": ("https://api.mistral.ai/v1/models", {"Authorization": "MISTRAL_API_KEY"}),
        "zai": ("https://api.z.ai/api/paas/v4/models", {"Authorization": "Z_AI_TOKEN"}),
    }
    out_dir.mkdir(parents=True, exist_ok=True)
    rc = 0
    for prov, (url, hdr) in reqs.items():
        headers = {}
        for k, v in hdr.items():
            if v.isupper() and "_" in v:
                key = _env_key(v)
                headers[k] = ("Bearer " + key) if k == "Authorization" else key
            else:
                headers[k] = v
        try:
            body = urllib.request.urlopen(urllib.request.Request(url, headers=headers), timeout=30).read()
            n = len(json.loads(body).get("data", []))
            (out_dir / f"{prov}.json").write_bytes(body)
            print(f"{prov}: {n} models")
        except Exception as e:  # noqa: BLE001 - report the class, never the request
            print(f"{prov}: FAILED ({type(e).__name__} {getattr(e, 'code', '')})")
            rc = 1
    return rc


# ---- CLI ---------------------------------------------------------------------------


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description="Compiled-in model id lint")
    ap.add_argument("--list", action="store_true", help="print every id found with its verdict")
    ap.add_argument("--record-fixtures", nargs="?", const=str(FIXTURE_DIR), metavar="DIR")
    args = ap.parse_args(argv)
    if args.record_fixtures:
        return record_fixtures(Path(args.record_fixtures))

    unallowed, stale, hits = lint()
    if args.list:
        allow = load_allowlist()
        active = load_active()
        prices = load_prices()
        for h in sorted(set(hits), key=lambda x: (x.file, x.line)):
            r = judge_hit(h, active, prices, allow.get("retired", []))
            print(f"{h.file}:{h.line}  {h.model:32s} {'ok' if not r else '; '.join(r)}")
    for f in unallowed:
        print(f"FAIL {f.file}:{f.line} {f.model}: {'; '.join(f.reasons)}")
    for e in stale:
        print(
            f"FAIL stale allowlist entry {e['id']} in {e['file']} "
            f"({'lane ' + e['lane'] if 'lane' in e else 'key'}): "
            "it no longer matches an offence - remove it"
        )
    n_ids = len({h.model for h in hits})
    if unallowed or stale:
        print(
            f"model-id lint: {len(unallowed)} unallowed finding(s), {len(stale)} stale allowlist "
            f"entr{'y' if len(stale) == 1 else 'ies'} ({n_ids} distinct ids scanned)"
        )
        return 1
    print(
        f"model-id lint: clean ({n_ids} distinct ids scanned, allowlist "
        f"{len(load_allowlist().get('allow', []))} entries)"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
