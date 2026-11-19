"""Host tests for the pending-test roster gate (check_pending_roster.py).

Run: python3 -m pytest tools/release_gate

The gate reads a `pio test -e native` log. These tests feed it logs in the exact
shape PlatformIO prints (one line per test case, `\t[PASSED]` / `\t[SKIPPED]`, then
the `N test cases: ...` summary) and git repos built in a temp dir for the
shrink-only rule. Each counter-test is a change that must turn the gate red.
"""

import os
import subprocess

import pytest

import check_pending_roster as gate

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))


def case(suite, name, status="PASSED", msg=None, line=10):
    text = f"test/{suite}/main.cpp:{line}: {name}"
    if msg:
        text += f": {msg}"
    return f"{text}\t[{status}]"


def native_log(cases, summary=True):
    """A native log: the case lines, then PlatformIO's summary bar."""
    lines = ["Processing test_x in native environment", *cases]
    if summary:
        n = len(cases)
        skipped = sum(1 for c in cases if c.endswith("[SKIPPED]"))
        failed = sum(1 for c in cases if c.endswith("[FAILED]"))
        parts = (f"{failed} failed, " if failed else "") + (f"{skipped} skipped, " if skipped else "")
        lines.append(f"========= {n} test cases: {parts}{n - skipped - failed} succeeded in 00:07:57.599 =========")
    return "\n".join(lines) + "\n"


PENDING_A = case("test_tx_prefix", "test_prefix_is_stable", "SKIPPED", "PENDING RB-T3")
PENDING_B = case("test_tx_log", "test_torn_tail_dropped", "SKIPPED", "PENDING RB-T2")
PASS_1 = case("test_ledger", "test_add_returns_id")
ROSTER_AB = (
    "# test/pending_roster.txt: one ignored native test per line\n"
    "test_tx_prefix::test_prefix_is_stable RB-T3\n"
    "test_tx_log::test_torn_tail_dropped RB-T2\n"
)


def run(log, roster, base=gate.ABSENT, **kw):
    return gate.check(log, roster, base, **kw)


# --- the equality rule: the ignored set is the roster, both ways -------------


def test_ignored_set_equal_to_roster_passes():
    ok, lines, fails = run(native_log([PASS_1, PENDING_A, PENDING_B]), ROSTER_AB)
    assert ok, fails
    assert any("2 ignored" in ln and "2 on the roster" in ln for ln in lines)


def test_clean_run_with_no_roster_file_passes():
    ok, _, fails = run(native_log([PASS_1]), None)
    assert ok, fails


def test_counter_ignored_test_missing_from_roster_fails():
    # A suite that quietly ignores a test the roster does not name: the hidden-skip class.
    ok, _, fails = run(native_log([PASS_1, PENDING_A, PENDING_B]), "test_tx_prefix::test_prefix_is_stable RB-T3\n")
    assert not ok
    assert any("test_tx_log::test_torn_tail_dropped" in f and "not on the roster" in f for f in fails)


def test_counter_ignore_with_no_roster_file_fails():
    ok, _, fails = run(native_log([PASS_1, PENDING_A]), None)
    assert not ok
    assert any("not on the roster" in f for f in fails)


def test_counter_roster_entry_that_now_passes_fails():
    # The implementing lane landed the test but left its roster line: stale, must shrink.
    ok, _, fails = run(native_log([PASS_1, PENDING_A, case("test_tx_log", "test_torn_tail_dropped")]), ROSTER_AB)
    assert not ok
    assert any("test_tx_log::test_torn_tail_dropped" in f and "not ignored" in f for f in fails)


def test_counter_ignore_without_pending_lane_fails_even_when_rostered():
    log = native_log([case("test_x", "test_flaky", "SKIPPED", "fixture needs updating")])
    ok, _, fails = run(log, "test_x::test_flaky RB-T3\n")
    assert not ok
    assert any("PENDING RB-" in f and "test_x::test_flaky" in f for f in fails)


def test_counter_bare_ignore_without_message_fails():
    log = native_log([case("test_x", "test_bare", "SKIPPED")])
    ok, _, fails = run(log, None)
    assert not ok
    assert any("test_x::test_bare" in f for f in fails)


def test_counter_roster_lane_differs_from_the_pending_message_fails():
    ok, _, fails = run(native_log([PENDING_A]), "test_tx_prefix::test_prefix_is_stable RB-T2\n")
    assert not ok
    assert any("RB-T3" in f and "RB-T2" in f for f in fails)


# --- the log must be a whole native run ---------------------------------------


def test_counter_log_without_summary_fails():
    ok, _, fails = run(native_log([PENDING_A], summary=False), "test_tx_prefix::test_prefix_is_stable RB-T3\n")
    assert not ok
    assert any("summary" in f for f in fails)


def test_counter_summary_skip_count_disagrees_with_the_case_lines_fails():
    # A case line in a shape the parser does not know must not read as "no ignores".
    log = native_log([PASS_1]).replace("1 test cases: 1 succeeded", "2 test cases: 1 skipped, 1 succeeded")
    ok, _, fails = run(log, None)
    assert not ok
    assert any("skipped" in f and "1" in f for f in fails)


def test_unity_raw_ignore_lines_are_read_too():
    raw = "test/test_tx_prefix/main.cpp:44:test_prefix_is_stable:IGNORE: PENDING RB-T3"
    log = native_log([PASS_1]).replace(PASS_1, PASS_1 + "\n" + raw)
    log = log.replace("1 test cases: 1 succeeded", "2 test cases: 1 skipped, 1 succeeded")
    ok, _, fails = run(log, "test_tx_prefix::test_prefix_is_stable RB-T3\n")
    assert ok, fails


def test_ansi_colours_and_duplicate_lines_are_tolerated():
    coloured = PENDING_A.replace("[SKIPPED]", "[\x1b[33mSKIPPED\x1b[0m]")
    log = native_log([PENDING_A]).replace(PENDING_A, coloured + "\n" + coloured)
    ok, _, fails = run(log, "test_tx_prefix::test_prefix_is_stable RB-T3\n")
    assert ok, fails


# --- roster file shape ----------------------------------------------------------


@pytest.mark.parametrize(
    "bad",
    [
        "test_tx_prefix::test_prefix_is_stable\n",  # no lane
        "test_tx_prefix test_prefix_is_stable RB-T3\n",  # no ::
        "test_tx_prefix::test_prefix_is_stable T3\n",  # lane without RB-
        "test_tx_prefix::test_prefix_is_stable RB-T3 extra\n",  # trailing words
    ],
)
def test_counter_malformed_roster_line_fails(bad):
    ok, _, fails = run(native_log([PASS_1]), bad)
    assert not ok
    assert any("line 1" in f for f in fails)


def test_counter_duplicate_roster_line_fails():
    roster = "test_tx_prefix::test_prefix_is_stable RB-T3\n" * 2
    ok, _, fails = run(native_log([PENDING_A]), roster)
    assert not ok
    assert any("twice" in f for f in fails)


# --- shrink-only against the base ----------------------------------------------


def test_roster_that_shrank_against_the_base_passes():
    roster = "test_tx_prefix::test_prefix_is_stable RB-T3\n"
    ok, lines, fails = run(native_log([PENDING_A]), roster, base=ROSTER_AB)
    assert ok, fails
    assert any("shrank by 1" in ln for ln in lines)


def test_counter_roster_that_grew_against_the_base_fails():
    ok, _, fails = run(
        native_log([PENDING_A, PENDING_B]), ROSTER_AB, base="test_tx_prefix::test_prefix_is_stable RB-T3\n"
    )
    assert not ok
    assert any("test_tx_log::test_torn_tail_dropped" in f and "can only shrink" in f for f in fails)


def test_a_new_line_that_names_a_ruling_may_grow_the_roster():
    base = "test_tx_prefix::test_prefix_is_stable RB-T3\n"
    roster = base + "test_tx_log::test_torn_tail_dropped RB-T2 ruling:56\n"
    ok, lines, fails = run(native_log([PENDING_A, PENDING_B]), roster, base=base)
    assert ok, fails
    assert any("added under a ruling" in ln and "test_tx_log::test_torn_tail_dropped" in ln for ln in lines)


def test_counter_a_new_line_without_a_ruling_still_fails_beside_one_with_it():
    base = "test_tx_prefix::test_prefix_is_stable RB-T3\n"
    roster = "test_tx_prefix::test_prefix_is_stable RB-T3 ruling:56\ntest_tx_log::test_torn_tail_dropped RB-T2\n"
    ok, _, fails = run(native_log([PENDING_A, PENDING_B]), roster, base=base)
    assert not ok
    assert any("test_tx_log::test_torn_tail_dropped" in f and "can only shrink" in f for f in fails)
    assert not any("test_tx_prefix::test_prefix_is_stable" in f for f in fails)


@pytest.mark.parametrize("bad", [" ruling:", " ruling:0", " ruling:x", " ruling 56", " ruling:56 extra"])
def test_counter_a_malformed_ruling_tag_fails(bad):
    ok, _, fails = run(native_log([PENDING_A]), "test_tx_prefix::test_prefix_is_stable RB-T3" + bad + "\n")
    assert not ok
    assert any("line 1" in f for f in fails)


def test_static_leg_reads_a_ruled_line_like_any_other(tmp_path):
    suite = tmp_path / "test" / "test_tx_prefix"
    suite.mkdir(parents=True)
    (suite / "main.cpp").write_text('void test_prefix_is_stable() { TXB_WAITS_FOR("PENDING RB-T3"); }\n')
    roster, _ = gate.parse_roster("test_tx_prefix::test_prefix_is_stable RB-T3 ruling:56\n")
    assert gate.static_problems(str(tmp_path), roster) == []
    roster, _ = gate.parse_roster("test_tx_prefix::test_missing RB-T3 ruling:56\n")
    assert gate.static_problems(str(tmp_path), roster)


def test_counter_moving_an_entry_to_another_lane_is_growth():
    base = "test_tx_prefix::test_prefix_is_stable RB-T2\n"
    ok, _, fails = run(native_log([PENDING_A]), "test_tx_prefix::test_prefix_is_stable RB-T3\n", base=base)
    assert not ok
    assert any("can only shrink" in f for f in fails)


def test_first_holder_creates_the_roster_when_the_base_has_none():
    ok, lines, fails = run(native_log([PENDING_A, PENDING_B]), ROSTER_AB, base=gate.ABSENT)
    assert ok, fails
    assert any("base has no roster" in ln for ln in lines)


def test_require_empty_fails_while_entries_remain():
    ok, _, fails = run(native_log([PENDING_A]), "test_tx_prefix::test_prefix_is_stable RB-T3\n", require_empty=True)
    assert not ok
    assert any("must be empty" in f for f in fails)


# --- the roster's tests exist in the tree (static leg) ---------------------------


def make_tree(tmp_path, sources):
    root = tmp_path / "repo"
    for rel, text in sources.items():
        p = root / rel
        p.parent.mkdir(parents=True, exist_ok=True)
        p.write_text(text, encoding="utf-8")
    return str(root)


def test_static_leg_finds_the_pending_test_in_its_suite(tmp_path):
    src = 'void test_prefix_is_stable() { TEST_IGNORE_MESSAGE("PENDING RB-T3"); }\n'
    root = make_tree(tmp_path, {"test/test_tx_prefix/main.cpp": src})
    fails = gate.static_problems(root, {"test_tx_prefix::test_prefix_is_stable": "RB-T3"})
    assert fails == []


def test_counter_static_leg_roster_names_a_missing_suite_or_test(tmp_path):
    root = make_tree(tmp_path, {"test/test_tx_prefix/main.cpp": "void test_other() {}\n"})
    fails = gate.static_problems(root, {"test_tx_prefix::test_prefix_is_stable": "RB-T3", "test_gone::test_x": "RB-T2"})
    assert any("test_gone" in f and "no suite" in f for f in fails)
    assert any("test_prefix_is_stable" in f and "PENDING RB-T3" in f for f in fails)


# --- main(): exit codes, git base, real tree ---------------------------------------


def git(root, *args):
    subprocess.run(["git", "-C", root, *args], check=True, capture_output=True)


def make_git_tree(tmp_path, base_roster, head_roster):
    root = str(tmp_path / "g")
    os.makedirs(os.path.join(root, "test"))
    git(root, "init", "-q", "-b", "rb-main")
    git(root, "config", "user.email", "t@example.invalid")
    git(root, "config", "user.name", "t")
    path = os.path.join(root, gate.ROSTER_REL)
    if base_roster is not None:
        with open(path, "w", encoding="utf-8") as f:
            f.write(base_roster)
    with open(os.path.join(root, "README"), "w", encoding="utf-8") as f:
        f.write("x\n")
    git(root, "add", "-A")
    git(root, "commit", "-q", "-m", "base")
    git(root, "checkout", "-q", "-b", "lane/RB-X")
    with open(path, "w", encoding="utf-8") as f:
        f.write(head_roster)
    git(root, "add", "-A")
    git(root, "commit", "-q", "-m", "lane")
    return root


def write_log(tmp_path, text):
    p = tmp_path / "native.log"
    p.write_text(text, encoding="utf-8")
    return str(p)


def test_main_exit_codes_and_marker_lines(tmp_path, capsys):
    root = make_git_tree(tmp_path, ROSTER_AB, "test_tx_prefix::test_prefix_is_stable RB-T3\n")
    log = write_log(tmp_path, native_log([PENDING_A, case("test_tx_log", "test_torn_tail_dropped")]))
    assert gate.main(["--root", root, "--native-log", log, "--no-static"]) == 0
    assert "PENDING ROSTER GREEN" in capsys.readouterr().out
    log = write_log(tmp_path, native_log([PENDING_A, PENDING_B]))
    assert gate.main(["--root", root, "--native-log", log, "--no-static"]) == 1
    assert "PENDING ROSTER RED" in capsys.readouterr().out


def test_counter_main_roster_grown_in_git_fails(tmp_path, capsys):
    root = make_git_tree(tmp_path, "test_tx_prefix::test_prefix_is_stable RB-T3\n", ROSTER_AB)
    log = write_log(tmp_path, native_log([PENDING_A, PENDING_B]))
    assert gate.main(["--root", root, "--native-log", log, "--no-static"]) == 1
    assert "can only shrink" in capsys.readouterr().out


def test_main_needs_a_native_log_unless_static_only(tmp_path, capsys):
    root = make_git_tree(tmp_path, None, "")
    assert gate.main(["--root", root]) == 1
    assert "--native-log" in capsys.readouterr().out
    assert gate.main(["--root", root, "--static-only"]) == 0


def test_counter_main_unknown_base_ref_fails(tmp_path, capsys):
    root = make_git_tree(tmp_path, None, "")
    assert gate.main(["--root", root, "--static-only", "--base-ref", "no-such-ref"]) == 1
    assert "no-such-ref" in capsys.readouterr().out


def test_real_tree_static_leg():
    """The roster in this tree names only tests that exist and say PENDING for their lane."""
    rc = gate.main(["--root", REPO, "--static-only", "--base-ref", "HEAD"])
    assert rc == 0


# --- review findings: a red or crashed run, other summary shapes, lane boundaries ---------


def test_counter_a_failed_case_makes_the_ignored_set_unknown():
    log = native_log([PASS_1, case("test_ledger", "test_bad", "FAILED", "Expected 1 Was 2")])
    ok, _, fails = run(log, None)
    assert not ok
    assert any("not green" in f for f in fails)


def test_counter_an_errored_suite_fails_even_when_the_counts_agree():
    log = native_log([PASS_1]) + "------ native:test_tx_log [ERRORED] Took 2.21 seconds ------\n"
    ok, _, fails = run(log, None)
    assert not ok
    assert any("ERRORED" in f for f in fails)


def test_counter_summary_with_errored_and_skipped_counts_is_read_token_by_token():
    log = "========= 2 test cases: 1 errored, 1 skipped, 0 succeeded in 00:00:01.000 =========\n"
    ok, _, fails = run(log, None)
    assert not ok
    assert any("1 errored" in f for f in fails)
    assert any("1 skipped" in f for f in fails)


def test_summary_with_only_skipped_cases_is_read():
    log = PENDING_A + "\n========= 1 test cases: 1 skipped in 00:00:01.000 =========\n"
    ok, _, fails = run(log, "test_tx_prefix::test_prefix_is_stable RB-T3\n")
    assert ok, fails


def test_counter_pending_lane_prefix_is_not_another_lane():
    log = native_log([case("test_tx_prefix", "test_prefix_is_stable", "SKIPPED", "PENDING RB-T30")])
    ok, _, fails = run(log, "test_tx_prefix::test_prefix_is_stable RB-T3\n")
    assert not ok
    log = native_log([case("test_tx_prefix", "test_prefix_is_stable", "SKIPPED", "PENDING RB-T3-x")])
    ok, _, fails = run(log, "test_tx_prefix::test_prefix_is_stable RB-T3\n")
    assert not ok


def test_pending_message_with_a_reason_after_the_lane_passes():
    log = native_log(
        [case("test_tx_prefix", "test_prefix_is_stable", "SKIPPED", "PENDING RB-T3: waits for the stub rule")]
    )
    ok, _, fails = run(log, "test_tx_prefix::test_prefix_is_stable RB-T3\n")
    assert ok, fails


def test_counter_static_leg_lane_prefix_is_not_another_lane(tmp_path):
    src = 'void test_prefix_is_stable() { TEST_IGNORE_MESSAGE("PENDING RB-T30"); }\n'
    root = make_tree(tmp_path, {"test/test_tx_prefix/main.cpp": src})
    assert gate.static_problems(root, {"test_tx_prefix::test_prefix_is_stable": "RB-T3"}) != []


def test_counter_main_without_rb_main_has_no_fallback(tmp_path, capsys):
    # A clone without rb-main must not compare against main (which has no roster) and pass.
    root = make_git_tree(tmp_path, None, ROSTER_AB)
    git(root, "branch", "-m", "rb-main", "main")
    assert gate.main(["--root", root, "--static-only", "--no-static"]) == 1
    assert "rb-main" in capsys.readouterr().out


def test_main_native_log_that_is_a_directory_is_red_not_a_traceback(tmp_path, capsys):
    root = make_git_tree(tmp_path, None, "")
    assert gate.main(["--root", root, "--native-log", str(tmp_path)]) == 1
    assert "PENDING ROSTER RED" in capsys.readouterr().out


def test_marker_names_the_legs_that_did_not_run(tmp_path, capsys):
    root = make_git_tree(tmp_path, None, "")
    assert gate.main(["--root", root, "--static-only", "--no-static"]) == 0
    out = capsys.readouterr().out
    assert "equality leg not run" in out and "static leg not run" in out
