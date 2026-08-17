"""Unit tests for the eval harness (summarize.py + report.py).

Runs against the committed redacted sample corpus under evals/samples/ - no
hardware, network, or provider keys required.
"""

from __future__ import annotations

import importlib.util
import json
import os

HERE = os.path.dirname(os.path.abspath(__file__))
EVALS = os.path.dirname(HERE)
SAMPLES = os.path.join(EVALS, "samples")
CORPUS = os.path.join(SAMPLES, "example-run.jsonl")


def _load(name: str):
    spec = importlib.util.spec_from_file_location(name, os.path.join(EVALS, f"{name}.py"))
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


summarize = _load("summarize")
report = _load("report")


def _rows():
    with open(CORPUS) as f:
        return [json.loads(line) for line in f if line.strip()]


# --------------------------------------------------------------------------- #
# summarize.py
# --------------------------------------------------------------------------- #
def test_summary_matches_committed_sample():
    rows = _rows()
    got = summarize.build_summary(
        rows,
        triage={},
        meta={
            "run": "example-run",
            "date": "2026-08-17",
            "fw": "v4.1.8-example",
            "board": "sample",
            "categories": ["multi-subsession", "research-artifacts", "resilience"],
            "note": "Redacted synthetic corpus for tests and docs.",
        },
    )
    with open(os.path.join(SAMPLES, "example-summary.json")) as f:
        want = json.load(f)
    assert got == want


def test_pass_rule():
    scen = summarize.summarize_scenarios(_rows())
    # both reps pass -> scenario passes
    assert scen["sample-fanout-basic"]["pass"] is True
    assert scen["sample-fanout-basic"]["reps"] == 2
    # oracle failure -> fail
    assert scen["sample-timeout-case"]["pass"] is False
    # oracle passes but judge marks dishonest -> fail (judge is a soft signal that fails a scenario)
    soft = scen["sample-judge-soft-fail"]
    assert soft["oracle_fails"] == 0 and soft["judge_fails"] == 1 and soft["pass"] is False


def test_pass_rate_math():
    summary = summarize.build_summary(_rows(), {}, {})
    t = summary["totals"]
    assert t["ran"] == 7
    assert t["pass"] == 2
    assert t["fail"] == 5
    assert summary["schema_version"] == 1


def test_triage_annotates_only_failures():
    triage = {"sample-timeout-case": {"class": "transient", "note": "provider 429"}}
    summary = summarize.build_summary(_rows(), triage, {})
    assert summary["scenarios"]["sample-timeout-case"]["triage"] == "transient"
    assert summary["scenarios"]["sample-timeout-case"]["note"] == "provider 429"
    # a passing scenario carries no triage key
    assert "triage" not in summary["scenarios"]["sample-fanout-basic"]
    assert summary["totals"]["fail_by_class"]["transient"] == 1


# --------------------------------------------------------------------------- #
# report.py - the one wedge rule
# --------------------------------------------------------------------------- #
def test_wedge_rule_empty_reply():
    assert report.is_wedge({"reply": "", "oracle": {"passed": False, "detail": "x"}}) is True


def test_wedge_rule_markers():
    for marker in ("timed out", "wedge", "runner error"):
        row = {"reply": "something", "oracle": {"passed": False, "detail": f"the turn {marker} here"}}
        assert report.is_wedge(row) is True


def test_wedge_rule_negative():
    row = {"reply": "a real answer", "oracle": {"passed": False, "detail": "FABRICATED: no tool fired"}}
    assert report.is_wedge(row) is False


def test_aggregate_totals():
    agg = report.aggregate(_rows(), catmap={})
    t = agg["total"]
    assert t["n"] == 8
    assert t["oracle_pass"] == 4
    assert t["wedge"] == 3
    assert t["honest_viol"] == 2
    assert abs(report.mean(t["scores"]) - 0.61) < 0.01


def test_aggregate_per_scenario_pass():
    agg = report.aggregate(_rows(), catmap={})
    ps = agg["per_scenario"]
    assert ps["sample-fanout-basic"]["pass"] is True
    assert ps["sample-email-honesty"]["pass"] is False


# --------------------------------------------------------------------------- #
# report.py - all three formats produce output
# --------------------------------------------------------------------------- #
def test_report_text():
    out = report.build_report([CORPUS], "text")
    assert "Nimbus eval report" in out
    assert "scenario grid" in out
    assert "✅" not in out and "❌" not in out  # no emoji


def test_report_md():
    out = report.build_report([CORPUS], "md")
    assert out.startswith("# Nimbus eval report")
    assert "| scenario |" in out


def test_report_html():
    out = report.build_report([CORPUS], "html")
    assert "<style>" in out and "<table>" in out
    assert "border=1" not in out  # no HTML 3.2 slop


def test_report_comparison_deltas():
    out = report.build_report([CORPUS, CORPUS], "text")
    assert "deltas" in out
    assert "+0" in out  # identical files -> zero delta
