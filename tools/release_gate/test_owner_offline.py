#!/usr/bin/env python3
"""Tests for the history fix's owner-regression graders (lane CTXE): the offline fold replay's
assertions (tests/evals/owner_regressions/run_offline.py) and request_contains (assertions.py).

Each grader is shown passing the candidate's shape and failing the v5.1.1 shape recorded on
16 Nov 2026 (lanes/CTXE/evidence/offline-v511, live-v511-run2-request0), so none can pass
vacuously. Needs no board, no network and no build.
"""

from __future__ import annotations

import json
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
EVALS = os.path.join(HERE, "..", "..", "tests", "evals", "owner_regressions")
sys.path.insert(0, EVALS)
import assertions as A  # noqa: E402
import run_offline as O  # noqa: E402

V511_STORM = {
    "fold_requests": [
        {"pass": p, "host": "app.cumulo-nimbus.ai", "path": "/router/openai/v1/responses", "model": "gpt-5.6"}
        for p in (1, 2, 3)
    ],
    "messages": ["Automatic summaries of this chat keep failing, so they are paused. Send /compact to try again."],
    "summary_after": "",
}
CANDIDATE_STORM = {
    "fold_requests": [
        {"pass": 1, "host": "app.cumulo-nimbus.ai", "path": "/router/openai/v1/responses", "model": "gpt-5.6"},
        {"pass": 2, "host": "app.cumulo-nimbus.ai", "path": "/router/openai/v1/responses", "model": "gpt-5.6-luna"},
    ],
    "messages": [],
    "summary_after": "",
}


def scenario(name):
    with open(os.path.join(EVALS, "scenarios", name + ".json"), encoding="utf-8") as f:
        return json.load(f)


def test_the_v511_storm_fails_and_a_ladder_passes():
    sc = scenario("lc-fold-ladder-storm")
    assert not all(a["pass"] for a in O.grade(sc, V511_STORM))
    assert all(a["pass"] for a in O.grade(sc, CANDIDATE_STORM))


def test_a_storm_with_no_fold_at_all_fails():
    # A cycle that never starts (a gate that waits forever) is no pass: the row needs a try.
    sc = scenario("lc-fold-ladder-storm")
    assert not all(a["pass"] for a in O.grade(sc, {"fold_requests": [], "messages": []}))


def test_self_heal_needs_the_new_summary_and_no_pause_copy():
    sc = scenario("lc-fold-self-heal")
    v511 = {"fold_requests": [], "messages": [], "summary_after": "1. Decisions & preferences\n- old"}
    healed = {
        "fold_requests": [{"pass": 1, "host": "h", "model": "m"}],
        "messages": [],
        "summary_after": "4. Owner intent & active threads\n- SUMMARY-HEALED: diary.",
    }
    assert not all(a["pass"] for a in O.grade(sc, v511))
    assert all(a["pass"] for a in O.grade(sc, healed))
    healed_but_told = dict(healed, messages=["Summaries were paused; send /compact."])
    assert not all(a["pass"] for a in O.grade(sc, healed_but_told))


def test_every_offline_scenario_validates_and_the_live_suite_skips_it():
    import run_eval as E

    offline = O.load_offline()
    assert {s["id"] for s in offline} >= {"lc-fold-self-heal", "lc-fold-ladder-storm"}
    assert all(O.check_scenario(s) == [] for s in offline)
    assert not {s["id"] for s in E.load_scenarios()} & {s["id"] for s in offline}


def test_an_unknown_offline_assertion_or_runner_is_invalid():
    bad = {"id": "lc-x", "rows": ["OIR-0"], "title": "t", "runner": "nope", "assertions": ["nope"]}
    errs = O.check_scenario(bad)
    assert any("unknown runner" in e for e in errs) and any("unknown assertion" in e for e in errs)


def test_request_contains_reads_the_first_request():
    params = {"text": "blueberries about 190 g, 108 kcal"}
    with_row = {
        "turns": [
            {
                "reply": "x",
                "request0": '{"messages":[{"content":"Estimate, not logged: blueberries about 190 g, 108 kcal"}]}',
            }
        ]
    }
    cut = {
        "turns": [{"reply": "I estimated 190 g.", "request0": '{"messages":[{"content":"With the labels: 125 g"}]}'}]
    }
    assert A.ASSERTIONS["request_contains"]({}, with_row, params)[0]
    assert not A.ASSERTIONS["request_contains"]({}, cut, params)[0]  # the reply alone cannot pass it
    assert not A.ASSERTIONS["request_contains"]({}, {"turns": [{"reply": "x"}]}, params)[0]


def test_the_history_rows_grade_the_request_not_only_the_reply():
    for name in ("lc-context-own-estimate", "lc-day-boundary"):
        ids = [a if isinstance(a, str) else a["id"] for a in scenario(name)["assertions"]]
        assert "request_contains" in ids, name


if __name__ == "__main__":
    import pytest

    sys.exit(pytest.main([__file__, "-q"]))
