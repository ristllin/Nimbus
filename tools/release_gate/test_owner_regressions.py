"""Host tests for the owner-regression release gate (check_owner_regressions.py).

Run: python3 -m pytest tools/release_gate
"""

import json
import os

import pytest

import check_owner_regressions as cor

HERE = os.path.dirname(os.path.abspath(__file__))

CLEAN_REGISTER = """# register

| ID | Date(s) | Failure | Class | Root cause | Fix | Deterministic guard | Live case |
|---|---|---|---|---|---|---|---|
| OIR-01 | 28 Oct | a failure | class | cause | abc1234 | oir_01, test_orch_loops | lc-one |
| OIR-02 | 29 Oct | open failure | class | cause | open | none yet; live case only | lc-two (xfail) |
| OIR-03 | 30 Oct | process-gated | class | cause | abc1234 | tools/release_gate/check_critical_sections.py, test_critical_sections | n/a |
| OIR-04 | 31 Oct | in-flight fix | class | cause | lane X in flight (xfail) | oir_04 (EXPECTED-FAIL) | lc-four |
"""

CLEAN_SUITE = """
void test_oir_01_thing() {
  TEST_ASSERT_TRUE(true);
}
void test_oir_04_xfail() {
  if (up.replaced == 1) {
    TEST_IGNORE_MESSAGE("EXPECTED-FAIL OIR-04");
  }
  TEST_ASSERT_EQUAL_INT(0, up.replaced);
}
int main() {
  RUN_TEST(test_oir_01_thing);
  RUN_TEST(test_oir_04_xfail);
  return UNITY_END();
}
"""

CLEAN_RUN_GATE = "import check_owner_regressions\n# ...\n('owner regressions', check_owner_regressions.main)\n"

THRESHOLDS = {
    "runs": 5,
    "default_min_passes": 4,
    "gate_models": ["mistral-large-4-0", "claude-haiku-5-5"],
    "models": {"mistral-large-4-0": {"host": "mistral", "min_passes": 4}},
    "scenarios": {
        "lc-two": {"xfail": True, "reason": "open on main"},
        "lc-four": {"xfail": True, "reason": "lane X in flight"},
    },
}


def summary(sha, scenarios):
    """A summary.json body: one model, the given {scenario: (runs, passes, xfail, meets)}."""
    return {
        "suite": "owner_regressions",
        "sha": sha,
        "runs": 5,
        "generated_utc": "2026-11-05T00:00:00+00:00",
        "models": {
            "mistral-large-4-0": {
                "host": "mistral",
                "scenarios": {
                    sid: {"runs": r, "passes": p, "min_passes": 4, "xfail": x, "meets": m}
                    for sid, (r, p, x, m) in scenarios.items()
                },
            }
        },
    }


@pytest.fixture
def tree(tmp_path, monkeypatch):
    """A minimal repo tree with a clean register, suite, run_gate and gate files."""
    (tmp_path / "coverage").mkdir()
    (tmp_path / "test" / "test_owner_regressions").mkdir(parents=True)
    (tmp_path / "test" / "test_orch_loops").mkdir()
    (tmp_path / "tools" / "release_gate").mkdir(parents=True)
    (tmp_path / "tests" / "evals" / "owner_regressions" / "runs").mkdir(parents=True)
    (tmp_path / "coverage" / "OWNER-ISSUES-REGISTER.md").write_text(CLEAN_REGISTER)
    (tmp_path / "test" / "test_owner_regressions" / "main.cpp").write_text(CLEAN_SUITE)
    (tmp_path / "tools" / "release_gate" / "run_gate.py").write_text(CLEAN_RUN_GATE)
    (tmp_path / "tools" / "release_gate" / "check_owner_regressions.py").write_text("")
    (tmp_path / "tools" / "release_gate" / "test_owner_regressions.py").write_text("")
    (tmp_path / "tools" / "release_gate" / "test_critical_sections.py").write_text("")
    (tmp_path / "tools" / "release_gate" / "check_critical_sections.py").write_text("")
    (tmp_path / "tests" / "evals" / "owner_regressions" / "thresholds.json").write_text(json.dumps(THRESHOLDS))
    monkeypatch.setattr(cor, "REPO", str(tmp_path))
    monkeypatch.setattr(cor, "REGISTER", str(tmp_path / "coverage" / "OWNER-ISSUES-REGISTER.md"))
    monkeypatch.setattr(cor, "SUITE", str(tmp_path / "test" / "test_owner_regressions" / "main.cpp"))
    monkeypatch.setattr(cor, "RUN_GATE", str(tmp_path / "tools" / "release_gate" / "run_gate.py"))
    monkeypatch.setattr(cor, "RUNS", str(tmp_path / "tests" / "evals" / "owner_regressions" / "runs"))
    monkeypatch.setattr(cor, "THRESHOLDS", str(tmp_path / "tests" / "evals" / "owner_regressions" / "thresholds.json"))
    return tmp_path


def write_register(tree, text):
    (tree / "coverage" / "OWNER-ISSUES-REGISTER.md").write_text(text)


def write_suite(tree, text):
    (tree / "test" / "test_owner_regressions" / "main.cpp").write_text(text)


def test_clean_tree_passes_static(tree):
    assert cor.static_problems() == []
    assert cor.main([]) == 0


def test_register_naming_a_nonexistent_oir_id_fails(tree):
    write_register(tree, CLEAN_REGISTER.replace("oir_01, test_orch_loops", "oir_01, oir_99, test_orch_loops"))
    assert any("oir_99" in e for e in cor.static_problems())


def test_register_row_with_no_guard_at_all_fails(tree):
    write_register(tree, CLEAN_REGISTER.replace("oir_01, test_orch_loops | lc-one", " | n/a"))
    assert any("OIR-01 names no guard" in e for e in cor.static_problems())


def test_open_row_guarded_only_by_its_live_case_passes(tree):
    # OIR-02 in the clean fixture: fix open, live case named - that is a guard.
    assert not any("OIR-02" in e for e in cor.static_problems())


def test_register_naming_a_missing_suite_or_tool_fails(tree):
    write_register(tree, CLEAN_REGISTER.replace("test_orch_loops", "test_no_such_suite"))
    assert any("test_no_such_suite" in e for e in cor.static_problems())
    write_register(tree, CLEAN_REGISTER.replace("check_critical_sections.py", "check_no_such_gate.py"))
    assert any("check_no_such_gate.py" in e for e in cor.static_problems())


def test_xfail_roster_mismatch_fails_both_ways(tree):
    write_register(tree, CLEAN_REGISTER.replace("oir_01, test_orch_loops", "oir_01 (EXPECTED-FAIL), test_orch_loops"))
    assert any("roster mismatch" in e for e in cor.static_problems())
    write_suite(tree, CLEAN_SUITE.replace('TEST_IGNORE_MESSAGE("EXPECTED-FAIL OIR-04");', ""))
    assert any("roster mismatch" in e for e in cor.static_problems())


def test_xfail_test_asserting_nonempty_was_fails(tree):
    write_suite(
        tree,
        CLEAN_SUITE.replace("TEST_ASSERT_EQUAL_INT(0, up.replaced);", "TEST_ASSERT_FALSE(up.changes[0].was.empty());"),
    )
    assert any("non-empty RowChange::was" in e for e in cor.static_problems())


def test_unwired_run_gate_fails(tree):
    (tree / "tools" / "release_gate" / "run_gate.py").write_text("# nothing\n")
    assert any("does not wire" in e for e in cor.static_problems())


def test_live_half_requires_a_run_on_the_candidate_sha(tree):
    sha = "a" * 40
    assert cor.main(["--candidate", sha]) == 1  # no run on disk yet
    runs = tree / "tests" / "evals" / "owner_regressions" / "runs" / sha
    runs.mkdir(parents=True)
    (runs / "summary.json").write_text(
        json.dumps(
            summary(sha, {"lc-one": (5, 5, False, True), "lc-two": (5, 0, True, False), "lc-four": (5, 0, True, False)})
        )
    )
    assert cor.main(["--candidate", sha]) == 0


def test_live_half_fails_on_unmet_threshold_on_a_gate_model(tree):
    sha = "b" * 40
    runs = tree / "tests" / "evals" / "owner_regressions" / "runs" / sha
    runs.mkdir(parents=True)
    (runs / "summary.json").write_text(
        json.dumps(
            summary(
                sha, {"lc-one": (5, 2, False, False), "lc-two": (5, 0, True, False), "lc-four": (5, 0, True, False)}
            )
        )
    )
    assert cor.main(["--candidate", sha]) == 1


def test_live_half_fails_on_a_stale_xfail_that_met_its_threshold(tree):
    sha = "c" * 40
    runs = tree / "tests" / "evals" / "owner_regressions" / "runs" / sha
    runs.mkdir(parents=True)
    (runs / "summary.json").write_text(
        json.dumps(
            summary(sha, {"lc-one": (5, 5, False, True), "lc-two": (5, 0, True, False), "lc-four": (5, 5, True, True)})
        )
    )
    errs = cor.live_problems(
        summary(sha, {"lc-one": (5, 5, False, True), "lc-two": (5, 0, True, False), "lc-four": (5, 5, True, True)}),
        THRESHOLDS,
        None,
    )
    assert any("stale EXPECTED-FAIL" in e for e in errs)


def test_live_half_fails_on_regression_against_the_previous_candidate(tree):
    sha, prev = "d" * 40, "e" * 40
    now = summary(sha, {"lc-one": (5, 1, False, False)})
    old = summary(prev, {"lc-one": (5, 5, False, True)})
    errs = cor.live_problems(now, THRESHOLDS, old)
    assert any("regressed" in e for e in errs)


def test_native_log_with_an_expected_fail_skip_blocks(tree, tmp_path):
    log = tmp_path / "native.log"
    log.write_text("38 Tests 0 Failures 3 Ignored\nEXPECTED-FAIL OIR-43: lane FSCAP in flight\n")
    assert cor.main(["--native-log", str(log)]) == 1
    log.write_text("38 Tests 0 Failures 0 Ignored\n")
    assert cor.main(["--native-log", str(log)]) == 0


def test_the_real_tree_is_consistent():
    """The committed register and suite must agree; this fails on any stale claim."""
    assert cor.static_problems() == []


def test_the_gate_runs_it():
    with open(os.path.join(HERE, "run_gate.py"), encoding="utf-8") as f:
        assert "check_owner_regressions.main" in f.read()
