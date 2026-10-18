"""Tests for tools/model_id_lint.py (the compiled-in model id lint).

Run: python3 -m pytest tools/release_gate
The last test lints the real tree, so a new compiled-in id with no recorded
upstream entry or no price row fails CI here.
"""

import json
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))

import model_id_lint as lint  # noqa: E402

PRICE_REL = "lib/core/include/nimbus/orch/model_price_defaults.h"


# --- literal extraction -------------------------------------------------------
def test_literals_skip_comments_and_keep_raw_strings():
    src = (
        '// "claude-opus-5" in a comment\n'
        "/* \"gpt-5.6-luna\" in a block */\n"
        'const char* a = "claude-haiku-5-5";\n'
        "const char* b = R\"J({\"model\":\"glm-5.3\"})J\";\n"
        "char c = '\"';\n"
        'const char* d = "esc \\" mistral-small-latest";\n'
    )
    lits = [s for _, s in lint.string_literals(src)]
    joined = " | ".join(lits)
    assert "claude-opus-5" not in joined
    assert "gpt-5.6-luna" not in joined
    assert "claude-haiku-5-5" in joined
    assert "glm-5.3" in joined
    assert "mistral-small-latest" in joined


def test_literal_line_numbers_follow_multiline_raw_strings():
    src = 'x;\nR"(first\nsecond claude-opus-5\n)";\n'
    hits = []
    for line, lit in lint.string_literals(src):
        for model, _, dl in lint.ids_in_literal(lit):
            hits.append((model, line + dl))
    assert hits == [("claude-opus-5", 3)]


def test_ids_in_lists_and_router_selectors():
    found = {m for m, _, _ in lint.ids_in_literal("anthropic/claude-opus-5,gpt-5.6-luna; zai/glm-5.3-flash")}
    assert found == {"claude-opus-5", "gpt-5.6-luna", "glm-5.3-flash"}


def test_family_prefixes_and_words_are_not_ids():
    for s in (
        "claude-opus",
        "claude-3",
        "gpt-5",
        "gpt-6",
        "glm-4",
        "mistral-builtins",
        "mistral-loop",
        "mistral-large",
        "orchTurn(mistral-loop)",
    ):
        assert lint.ids_in_literal(s) == [], s


def test_provider_of_each_shape():
    cases = {
        "claude-haiku-4-5-20251001": "anthropic",
        "claude-3-5-haiku-latest": "anthropic",
        "gpt-4o-mini": "openai",
        "gpt-5.6": "openai",
        "o4-mini-deep-research": "openai",
        "text-embedding-3-small": "openai",
        "pixtral-12b-latest": "mistral",
        "mistral-embed": "mistral",
        "voxtral-mini-tts-latest": "mistral",
        "glm-5.3-flash": "zai",
    }
    for model, prov in cases.items():
        assert lint.ids_in_literal(model) == [(model, prov, 0)], model


# --- the firmware's version-suffix rule (parity with orch_model_key.cpp) -------
def test_version_suffix_parity():
    good = ["-latest", "-2411", "-2026-10", "-20251001", "-2025-10-01", "-v2"]
    bad = ["", "latest", "-luna", "-12b", "-202510", "-v", "-2026-1", "-mini"]
    for s in good:
        assert lint.is_version_suffix(s), s
    for s in bad:
        assert not lint.is_version_suffix(s), s
    assert lint.family_matches("gpt-5.6", "gpt-5.6")
    assert not lint.family_matches("gpt-5.6-luna", "gpt-5.6")  # the money-safe rule
    assert lint.family_matches("mistral-small-latest", "mistral-small")


# --- the verdict, on a synthetic tree ------------------------------------------
def _tree(tmp_path, source, allow=None, prices=None, probed=None):
    (tmp_path / "src").mkdir()
    (tmp_path / "src/a.cpp").write_text(source)
    hdr = tmp_path / PRICE_REL
    hdr.parent.mkdir(parents=True)
    rows = prices if prices is not None else [("anthropic", "claude-opus-5"), ("openai", "gpt-5.6-luna")]
    body = "".join(f'    {{"{s}", "{f}", 1, 1, 0}},\n' for s, f in rows)
    hdr.write_text(
        "inline constexpr BuiltinPrice kBuiltinPrices[] = {\n" + body + '    {"openai", "*", 1, 1, 0},\n};\n'
    )
    fx = tmp_path / "fx"
    fx.mkdir()
    lists = {
        "anthropic": [{"id": "claude-opus-5"}, {"id": "claude-haiku-4-5-20251001"}],
        "openai": [{"id": "gpt-5.6-luna"}, {"id": "text-embedding-3-small"}],
        "mistral": [{"id": "mistral-small-latest", "aliases": ["mistral-small-2603"]}],
        "zai": [{"id": "glm-5.3"}],
    }
    for prov, data in lists.items():
        (fx / f"{prov}.json").write_text(json.dumps({"data": data}))
    if probed:
        (fx / "aliases_probed.json").write_text(json.dumps({"probed": probed}))
    al = tmp_path / "allow.json"
    al.write_text(json.dumps(allow or {"allow": [], "keys": [], "retired": []}))
    return lint.lint(repo=tmp_path, fixture_dir=fx, allow_path=al, roots=("src",))


def test_unknown_id_fails(tmp_path):
    unallowed, stale, _ = _tree(tmp_path, 'const char* m = "claude-opus-9";\n')
    assert [f.model for f in unallowed] == ["claude-opus-9"]
    assert "not in the recorded anthropic model list" in unallowed[0].reasons[0]
    assert not stale


def test_active_and_priced_id_passes(tmp_path):
    unallowed, stale, hits = _tree(tmp_path, 'const char* m = "claude-opus-5";\n')
    assert not unallowed and not stale and len(hits) == 1


def test_unpriced_chat_id_fails_but_unpriced_embedding_passes(tmp_path):
    unallowed, _, _ = _tree(tmp_path, 'a = "glm-5.3"; b = "text-embedding-3-small";\n')
    assert [(f.model, f.reasons) for f in unallowed] == [("glm-5.3", ["no built-in price row"])]


def test_dated_anthropic_alias_and_mistral_alias_are_active(tmp_path):
    unallowed, _, _ = _tree(
        tmp_path,
        'a = "claude-haiku-4-5"; b = "mistral-small-2603";\n',
        prices=[("anthropic", "claude-haiku-4-5"), ("mistral", "mistral-small")],
    )
    assert not unallowed


def test_probed_alias_is_active(tmp_path):
    src = 'a = "gpt-5.6";\n'
    plain, probed = tmp_path / "plain", tmp_path / "probed"
    plain.mkdir()
    probed.mkdir()
    unallowed, _, _ = _tree(plain, src, prices=[("openai", "gpt-5.6")])
    # unproved: both the source literal and the price family it relies on fail
    assert sorted(f.file for f in unallowed) == [PRICE_REL, "src/a.cpp"]
    unallowed, _, _ = _tree(
        probed,
        src,
        prices=[("openai", "gpt-5.6")],
        probed=[{"provider": "openai", "id": "gpt-5.6", "served": "gpt-5.6-sol"}],
    )
    assert not unallowed


def test_retired_id_fails_even_when_listed(tmp_path):
    allow = {"allow": [], "keys": [], "retired": [{"id": "claude-opus-5", "why": "test policy"}]}
    unallowed, _, _ = _tree(tmp_path, 'a = "claude-opus-5";\n', allow=allow)
    assert any("retired by policy" in r for r in unallowed[0].reasons)


def test_allowlisted_offence_passes_and_names_a_lane(tmp_path):
    allow = {
        "allow": [{"id": "claude-opus-9", "file": "src/a.cpp", "lane": "E", "why": "t"}],
        "keys": [],
        "retired": [],
    }
    unallowed, stale, _ = _tree(tmp_path, 'a = "claude-opus-9";\n', allow=allow)
    assert not unallowed and not stale


def test_allowlist_is_per_file(tmp_path):
    allow = {
        "allow": [{"id": "claude-opus-9", "file": "src/other.cpp", "lane": "E", "why": "t"}],
        "keys": [],
        "retired": [],
    }
    unallowed, stale, _ = _tree(tmp_path, 'a = "claude-opus-9";\n', allow=allow)
    assert [f.model for f in unallowed] == ["claude-opus-9"]
    assert len(stale) == 1  # and the entry for the other file is stale


def test_stale_allowlist_entry_fails(tmp_path):
    allow = {
        "allow": [{"id": "claude-opus-9", "file": "src/a.cpp", "lane": "E", "why": "t"}],
        "keys": [],
        "retired": [],
    }
    unallowed, stale, _ = _tree(tmp_path, 'a = "claude-opus-5";\n', allow=allow)
    assert not unallowed and [e["id"] for e in stale] == ["claude-opus-9"]


def test_price_family_must_match_an_active_id(tmp_path):
    unallowed, _, _ = _tree(tmp_path, "int x;\n", prices=[("openai", "gpt-0.1"), ("anthropic", "claude-opus-5")])
    assert [(f.model, f.file) for f in unallowed] == [("gpt-0.1", PRICE_REL)]


def test_allowlist_entries_need_a_lane(tmp_path):
    bad = tmp_path / "a.json"
    bad.write_text(json.dumps({"allow": [{"id": "x", "file": "y", "why": "z"}]}))
    try:
        lint.load_allowlist(bad)
    except ValueError as e:
        assert "lane" in str(e)
    else:
        raise AssertionError("an allowlist entry without a lane must be refused")


# --- the real tree --------------------------------------------------------------
def test_repo_tree_is_lint_clean():
    unallowed, stale, hits = lint.lint()
    assert hits, "the scan found no ids at all - the scanner is broken"
    assert not unallowed, "\n".join(f"{f.file}:{f.line} {f.model}: {f.reasons}" for f in unallowed)
    assert not stale, stale
