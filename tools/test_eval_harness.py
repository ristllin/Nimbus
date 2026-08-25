#!/usr/bin/env python3
"""Unit tests (T1) for the suite-agnostic eval runner (tools/eval_harness.py).

Everything here runs offline against a MOCK suite: no provider, no network, no
spend. It pins the parts a real nightly relies on - the persistence format,
change detection (content hash), the budget caps, and the trend report - so a
regression in any of them fails here before it can waste the paid-API allowance.

Run: python3 -m pytest tools/test_eval_harness.py
"""

import importlib.util
import json
import os

import pytest

_HERE = os.path.dirname(os.path.abspath(__file__))


def _load():
    spec = importlib.util.spec_from_file_location("eval_harness", os.path.join(_HERE, "eval_harness.py"))
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


H = _load()


@pytest.fixture
def evals_dir(tmp_path, monkeypatch):
    """Point the harness at a throwaway ~/nimbus-evals/ for the test."""
    monkeypatch.setattr(H, "EVALS_DIR", str(tmp_path))
    return str(tmp_path)


def _mock_suite(suite_id="mock_suite", provider="mock", scores=None, usage=None, models=("m1",)):
    """A deterministic suite: score/usage come from a table keyed by case name."""
    scores = scores or {"a": 1.0, "b": 0.0, "c": 1.0}
    usage = usage or {"in": 10, "out": 5}
    cases = list(scores.keys())

    def run(model, case):
        s = scores[case]
        return H.CaseResult(ok=s >= 0.5, score=s, note=f"scored {s}", usage=dict(usage))

    return H.Suite(
        id=suite_id,
        description="mock",
        provider=provider,
        models=list(models),
        cases=cases,
        inputs={"cases": cases, "models": list(models), "v": 1},
        run=run,
        budget=H.Budget(max_calls=100, max_usd=100.0),
    )


# ---- hashing ----------------------------------------------------------------
def test_content_hash_stable_and_sensitive():
    a = H.content_hash({"x": 1, "y": [1, 2]})
    b = H.content_hash({"y": [1, 2], "x": 1})  # key order must not matter
    assert a == b
    assert H.content_hash({"x": 1, "y": [1, 2]}) != H.content_hash({"x": 1, "y": [1, 3]})
    assert len(a) == 64  # sha256 hex


# ---- run + persistence ------------------------------------------------------
def test_run_writes_corpus_summary_ledger(evals_dir):
    suite = _mock_suite()
    summary = H.run_suite(suite, reps=1, now="2026-08-25T00:00:00+00:00", log=lambda *_: None)
    assert summary["n"] == 3
    assert summary["pass"] == 2  # a,c pass; b fails
    assert summary["pass_rate"] == pytest.approx(2 / 3, abs=1e-4)
    assert summary["mean_score"] == pytest.approx(2 / 3, abs=1e-4)
    assert summary["cost_usd"] == 0.0  # mock provider is free
    assert summary["tokens_in"] == 30 and summary["tokens_out"] == 15

    # corpus: one row per call
    with open(H.corpus_path("mock_suite")) as f:
        rows = [json.loads(x) for x in f if x.strip()]
    assert len(rows) == 3
    assert {r["case"] for r in rows} == {"a", "b", "c"}

    # summary.json + ledger both written
    assert os.path.exists(H.summary_path("mock_suite"))
    ledger = H.read_ledger("mock_suite")
    assert len(ledger) == 1
    assert ledger[0]["input_hash"] == summary["input_hash"]
    assert ledger[0]["generated_at"] == "2026-08-25T00:00:00+00:00"


# ---- change detection -------------------------------------------------------
def test_unchanged_inputs_skip_but_force_reruns(evals_dir):
    suite = _mock_suite()
    H.run_suite(suite, now="2026-08-25T00:00:00+00:00", log=lambda *_: None)
    assert not H.suite_changed(suite)  # same inputs -> not changed

    # a second plain run is skipped: no new ledger row, no calls
    calls = {"n": 0}
    orig_run = suite.run
    suite.run = lambda m, c: (calls.__setitem__("n", calls["n"] + 1), orig_run(m, c))[1]
    s2 = H.run_suite(suite, now="2026-08-25T01:00:00+00:00", log=lambda *_: None)
    assert s2.get("skipped") is True
    assert calls["n"] == 0
    assert len(H.read_ledger("mock_suite")) == 1

    # --force runs it again -> second ledger row
    H.run_suite(suite, force=True, now="2026-08-25T02:00:00+00:00", log=lambda *_: None)
    assert calls["n"] == 3
    assert len(H.read_ledger("mock_suite")) == 2


def test_changed_inputs_run_again(evals_dir):
    suite = _mock_suite()
    H.run_suite(suite, now="2026-08-25T00:00:00+00:00", log=lambda *_: None)
    suite.inputs = dict(suite.inputs, v=2)  # an input changed
    assert H.suite_changed(suite)
    s2 = H.run_suite(suite, now="2026-08-25T03:00:00+00:00", log=lambda *_: None)
    assert not s2.get("skipped")
    assert len(H.read_ledger("mock_suite")) == 2


# ---- budget -----------------------------------------------------------------
def test_max_calls_caps_the_run(evals_dir):
    suite = _mock_suite(models=("m1", "m2", "m3"))  # 3 cases x 3 models = 9 planned
    suite.budget = H.Budget(max_calls=4, max_usd=100.0)
    summary = H.run_suite(suite, now="2026-08-25T00:00:00+00:00", log=lambda *_: None)
    assert summary["n"] == 4
    assert summary["budget_hit"] == "max_calls"


def test_max_usd_caps_the_run(evals_dir):
    # priced provider + heavy usage so the dollar cap binds before the call cap.
    suite = _mock_suite(provider="anthropic", usage={"in": 100000, "out": 100000})
    # each call costs (1e5*3 + 1e5*15)/1e6 = $1.80; cap at $2 stops after 2 calls.
    suite.budget = H.Budget(max_calls=100, max_usd=2.0)
    summary = H.run_suite(suite, now="2026-08-25T00:00:00+00:00", log=lambda *_: None)
    assert summary["budget_hit"] == "max_usd"
    assert summary["n"] == 2
    assert summary["cost_usd"] == pytest.approx(3.6, abs=1e-6)


# ---- resilience -------------------------------------------------------------
def test_run_error_is_recorded_not_raised(evals_dir):
    def boom(model, case):
        raise RuntimeError("provider exploded")

    suite = _mock_suite()
    suite.run = boom
    summary = H.run_suite(suite, now="2026-08-25T00:00:00+00:00", log=lambda *_: None)
    assert summary["n"] == 3
    assert summary["pass"] == 0
    with open(H.corpus_path("mock_suite")) as f:
        rows = [json.loads(x) for x in f if x.strip()]
    assert all("error: RuntimeError" in r["note"] for r in rows)


# ---- trend report -----------------------------------------------------------
def test_trend_markdown_reports_history(evals_dir):
    suite = _mock_suite()
    H.run_suite(suite, now="2026-08-25T00:00:00+00:00", log=lambda *_: None)
    suite.inputs = dict(suite.inputs, v=2)
    H.run_suite(suite, now="2026-08-26T00:00:00+00:00", log=lambda *_: None)
    md = H.trend_markdown()
    assert "## mock_suite" in md
    assert "2026-08-25T00:00:00+00:00" in md
    assert "2026-08-26T00:00:00+00:00" in md
    assert "Trend over 2 run(s)" in md
    assert "(new)" in md and "(changed)" in md  # hash-change annotation


def test_trend_markdown_empty(evals_dir):
    md = H.trend_markdown()
    assert "No suites have run yet" in md


# ---- registry ---------------------------------------------------------------
def test_registry_roundtrip():
    suite = _mock_suite(suite_id="reg_test")
    H.register(suite)
    assert H.get("reg_test") is suite
    assert suite in H.all_suites()
