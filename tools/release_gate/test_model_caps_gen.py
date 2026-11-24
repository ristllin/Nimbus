"""Host tests for tools/model_caps/gen_caps.py: the L0 lints of the engine-2 router's table.

The real models.toml lints clean and the committed header is the generator's output; each lint
is counter-tested by a planted fault (ROUTER-DESIGN.md section 11, layer L0). Whether every row
resolves in the shared window and price functions is test/test_model_caps (C++), which calls them.
"""

import copy
import datetime as dt
import os
import sys

import pytest

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, os.path.join(REPO, "tools", "model_caps"))

import gen_caps as gen  # noqa: E402

TODAY = dt.date(2026, 11, 24)


@pytest.fixture(scope="module")
def data():
    return gen.load()


def errors(d, **kw):
    kw.setdefault("tree", False)
    return gen.lint(d, TODAY, **kw)[0]


def model(d, slot, match):
    return next(m for m in d["model"] if m["slot"] == slot and m["match"] == match)


def test_the_real_table_lints_clean_and_the_header_is_current(data):
    errs, rows, provs = gen.lint(data, dt.date.today())
    assert errs == []
    text, size = gen.render(data, rows, provs)
    assert size <= gen.MAX_DATA_BYTES
    with open(gen.OUT, encoding="utf-8") as fh:
        assert fh.read() == text, "run python3 tools/model_caps/gen_caps.py"
    assert gen.main(["--check"]) == 0


def test_every_compiled_model_id_has_a_row(data):
    _, rows, _ = gen.lint(data, TODAY, tree=False)
    ids = gen.compiled_ids({e["id"] for e in data.get("not_a_head", [])})
    assert len(ids) >= 20
    for prov, mid, where in ids:
        r = gen.row_for(rows, prov, mid)
        assert r is not None and r["kind"] != "any", (prov, mid, where)


# ---- counter-tests: each lint fails on a planted fault ------------------------------------------


def test_counter_a_compiled_id_without_a_row_fails(data):
    errs = errors(data, tree=True, ids=[("openai", "gpt-9-nope", "src/x.cpp:1")])
    assert any("gpt-9-nope" in e and "no row" in e for e in errs)


def test_counter_a_role_default_without_a_row_or_read_long_ago_fails(data):
    defaults = '{"zai", DefaultRole::Head, "standard", "glm", "glm-9"},'
    assert any("role default zai/glm-9" in e for e in errors(data, tree=True, ids=[], defaults_text=defaults))
    old = copy.deepcopy(data)
    old["template"]["glm"]["effort"] = dict(old["template"]["glm"]["effort"], read="2026-06-01")
    defaults = '{"zai", DefaultRole::Head, "standard", "glm", "glm-5.3"},'
    errs = errors(old, tree=True, ids=[], defaults_text=defaults)
    assert any("glm-5.3: effort was read" in e for e in errs)


def test_role_defaults_are_read_from_the_defaults_table():
    rows = gen.role_defaults(open(gen.DEFAULTS_H, encoding="utf-8").read())
    assert ("anthropic", "claude-sonnet-5") in rows
    assert ("openai", "gpt-5.6") in rows  # cumulo's head, read on its upstream
    assert len(rows) >= 8


@pytest.mark.parametrize(
    "plant, words",
    [
        (lambda d: d["template"]["claude5"]["thinking"].pop("src"), "src missing"),
        (lambda d: d["template"]["claude5"]["thinking"].update(src="no-such-source"), "neither a [sources] id"),
        (lambda d: d["template"]["claude5"]["thinking"].update(src="lib/nope.cpp:1"), "does not exist"),
        (lambda d: d["template"]["claude5"]["thinking"].update(src="AGENTS.md:99999"), "outside"),
        (lambda d: d["template"]["claude5"]["thinking"].update(read="2026-13-01"), "not a date"),
        (lambda d: d["template"]["claude5"]["thinking"].update(read="2027-01-01"), "in the future"),
        (lambda d: d["template"]["claude5"]["thinking"].update(ver="maybe"), "ver 'maybe'"),
        (lambda d: d["template"]["claude5"]["thinking"].update(v="sometimes"), "not a valid value"),
        (lambda d: d["template"]["claude5"]["effort"].update(v=["low", "turbo"]), "not a valid value"),
        (lambda d: d["template"]["claude5"]["effort"].update(v=["low", "low"]), "not a valid value"),
        (lambda d: d["template"]["claude5"]["images_max"].update(v=70000), "not a valid value"),
        (lambda d: d["template"]["claude5"].update(window={"v": 1}), "unknown fields"),
        (lambda d: d["template"]["failsafe"].pop("replay"), "replay missing"),
        (lambda d: d["template"]["claude5"].update(inherit="nope"), "unknown template"),
        (lambda d: d["template"]["failsafe"].update(inherit="claude5"), "inheritance cycle"),
        (lambda d: d["template"]["gpt"]["cap_param"].update(v="max_tokens"), "does not match the responses dialect"),
        (lambda d: d["template"]["claude5"]["cap_mode"].update(v="send_always"), "requires max_tokens"),
        (
            lambda d: model(d, "anthropic", "claude-opus-5-5").update(
                tool_choice={"v": ["auto", "any"], "src": "ant-errors", "read": "2026-11-24", "ver": "docs"}
            ),
            "which the model refuses",
        ),
        (lambda d: d["template"]["glm"]["tool_choice"].update(v=["required"]), "must allow tool_choice auto"),
        (lambda d: d["model"].append(copy.deepcopy(model(d, "zai", "glm-5.3"))), "listed twice"),
        (lambda d: d["model"].remove(model(d, "zai", "")), "needs exactly one '*'"),
        (lambda d: d["provider"].pop(), "provider custom_messages: missing"),
        (lambda d: d["provider"][0]["hosts"].update(v=["a", "b", "c"]), "not a valid value"),
        (lambda d: d["provider"][0].pop("version_header"), "version_header missing"),
        (lambda d: d["marker"][0].update(kind="regex"), "unknown slot, kind or class"),
        (lambda d: d["marker"][3].update(text="Credit balance"), "lower case"),
        (lambda d: d["marker"][0].pop("ver"), "keys must be exactly"),
        (lambda d: d["sources"].update(bad="http://insecure.example"), "https URL"),
        (lambda d: d.update(schema=2), "schema must be 1"),
    ],
)
def test_counter_each_lint_catches_its_fault(data, plant, words):
    d = copy.deepcopy(data)
    plant(d)
    errs = errors(d)
    assert any(words in e for e in errs), errs


def test_counter_too_many_rows_or_bytes_fail(data, monkeypatch, capsys):
    monkeypatch.setattr(gen, "MAX_MODEL_ROWS", 3)
    assert any("model rows (limit 3)" in e for e in errors(data))
    monkeypatch.setattr(gen, "MAX_DATA_BYTES", 100)
    monkeypatch.setattr(gen, "load", lambda path=gen.SRC: data)
    monkeypatch.setattr(gen, "MAX_MODEL_ROWS", 128)
    assert gen.main(["--check"]) == 1
    assert "(limit 100 B)" in capsys.readouterr().out


def test_counter_a_stale_header_fails_the_check(data, monkeypatch, tmp_path, capsys):
    stale = tmp_path / "model_caps_data.h"
    stale.write_text("// stale\n")
    monkeypatch.setattr(gen, "OUT", stale)
    assert gen.main(["--check"]) == 1
    assert "is stale" in capsys.readouterr().out


# ---- the lookup the lint uses is the firmware's ---------------------------------------------------


def test_row_for_follows_the_firmware_rule(data):
    _, rows, _ = gen.lint(data, TODAY, tree=False)
    assert gen.row_for(rows, "openai", "gpt-5.6-luna")["match"] == "gpt-5.6-luna"
    assert gen.row_for(rows, "openai", "GPT-5.6")["match"] == "gpt-5.6"
    assert gen.row_for(rows, "anthropic", "claude-haiku-5-5-20261001")["match"] == "claude-haiku-5-5"
    assert gen.row_for(rows, "openai", "gpt-5.6-mini")["kind"] == "prefix"  # the family row (E2-REP5), not gpt-5.6's
    assert gen.row_for(rows, "openai", "o9-unknown")["kind"] == "any"
    assert gen.row_for(rows, "mistral", "claude-haiku-5-5")["kind"] == "any"


def test_the_report_lists_unverified_values(data):
    _, rows, _ = gen.lint(data, TODAY, tree=False)
    lines = gen.unverified(data, rows)
    assert any("context_length_exceeded" in ln for ln in lines)
    # E2-R10 recorded replay_scope on OpenAI (S3); drop_under_rung is still unverified there
    assert not any("replay_scope" in ln and "gpt-5.6-luna" in ln for ln in lines)
    assert any("drop_under_rung" in ln and "gpt-5.6-luna" in ln for ln in lines)
    assert not any("claude-sonnet-5-5 thinking" in ln for ln in lines)
