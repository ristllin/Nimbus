"""Host tests for the lane file-ownership gate (check_lane_files.py) and tools/engine/owners.json.

Run: python3 -m pytest tools/release_gate

The gate takes the files a lane's branch changed since its merge base with the
integration branch and fails on any file outside that lane's entry in owners.json.
These tests use small owners tables and git repos built in a temp dir; the last ones
check the real table: well formed, and covering every lane of the program's PLAN.json
when that file is on this machine. Each counter-test is a change that must turn the gate red.
"""

import json
import os
import subprocess

import pytest

import check_lane_files as gate

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))
PLAN = os.environ.get("RB_PLAN_JSON", "/Users/roydarnell/Projects/nimbus-project/program-2026-10-18/rebuild/PLAN.json")


def owners(**lanes):
    table = {
        "shared": [{"glob": "docs/**", "why": "docs ride the change"}],
        "per_lane": [{"glob": "tools/release_gate/image_budget.d/{lane}.json", "why": "own budget file"}],
        "hot_files": {"src/agent/telegram.cpp": ["RB-A", "RB-B"]},
        "lanes": {},
    }
    for lane, files in lanes.items():
        table["lanes"][lane.replace("_", "-")] = {"stage": "S1", "files": files, "plan_text": ""}
    return table


BASIC = owners(
    RB_A=["lib/core/src/orch_ledger.cpp", "test/test_ledger/**", "src/agent/telegram.cpp"],
    RB_B=["src/agent/telegram.cpp", "lib/harness/src/plain_*.cpp"],
)


def run(table, lane, changed, **kw):
    return gate.check(table, lane, changed, **kw)


# --- the rule: every changed file is in the lane's entry ---------------------------


def test_files_inside_the_entry_pass():
    ok, lines, fails = run(BASIC, "RB-A", ["lib/core/src/orch_ledger.cpp", "test/test_ledger/main.cpp"])
    assert ok, fails
    assert any("2 file(s)" in ln for ln in lines)


def test_counter_a_file_outside_the_entry_fails():
    ok, _, fails = run(BASIC, "RB-A", ["lib/core/src/orch_ledger.cpp", "src/main.cpp"])
    assert not ok
    assert any("src/main.cpp" in f and "outside" in f for f in fails)


def test_counter_a_file_owned_only_by_another_lane_names_the_owner():
    ok, _, fails = run(BASIC, "RB-A", ["lib/harness/src/plain_engine.cpp"])
    assert not ok
    assert any("plain_engine.cpp" in f and "RB-B" in f for f in fails)


def test_counter_unknown_lane_fails():
    ok, _, fails = run(BASIC, "RB-NOPE", ["docs/x.md"])
    assert not ok
    assert any("RB-NOPE" in f and "owners.json" in f for f in fails)


def test_star_does_not_cross_a_directory():
    ok, _, fails = run(BASIC, "RB-B", ["lib/harness/src/plain_x/deep.cpp"])
    assert not ok, "plain_*.cpp must not match a file in a sub-folder"
    ok, _, _ = run(BASIC, "RB-B", ["lib/harness/src/plain_engine.cpp"])
    assert ok


def test_double_star_spans_directories():
    ok, _, fails = run(BASIC, "RB-A", ["test/test_ledger/fixtures/a/b.json"])
    assert ok, fails


def test_shared_docs_and_the_own_budget_file_pass():
    changed = ["docs/reference/ledger.md", "tools/release_gate/image_budget.d/RB-A.json"]
    ok, _, fails = run(BASIC, "RB-A", changed)
    assert ok, fails


def test_counter_another_lanes_budget_file_fails():
    ok, _, fails = run(BASIC, "RB-A", ["tools/release_gate/image_budget.d/RB-B.json"])
    assert not ok
    assert any("RB-B.json" in f for f in fails)


def test_declared_cross_lane_edit_passes_and_is_printed():
    ok, lines, fails = run(BASIC, "RB-A", ["src/main.cpp"], declared={"src/main.cpp"})
    assert ok, fails
    assert any("DECLARED" in ln and "src/main.cpp" in ln for ln in lines)


def test_counter_declaring_one_path_does_not_cover_another():
    ok, _, fails = run(BASIC, "RB-A", ["src/main.cpp", "src/net/webui.cpp"], declared={"src/main.cpp"})
    assert not ok
    assert any("src/net/webui.cpp" in f for f in fails)


def test_counter_lane_of_another_repo_owns_nothing_here():
    table = owners(RB_CU=["apps/reconciler/src/reconcile.ts"])
    table["lanes"]["RB-CU"]["repo"] = "cumulo-nimbus"
    ok, _, fails = run(table, "RB-CU", ["apps/reconciler/src/reconcile.ts"])
    assert not ok
    assert any("cumulo-nimbus" in f for f in fails)


def test_files_from_reads_a_listed_file_at_the_root(tmp_path):
    (tmp_path / "tools" / "engine").mkdir(parents=True)
    (tmp_path / "tools" / "engine" / "guard_files.txt").write_text(
        "# listed\nsrc/agent/store.cpp\n\n", encoding="utf-8"
    )
    table = owners(RB_S=["tools/engine/guard_files.txt"])
    table["lanes"]["RB-S"]["files_from"] = ["tools/engine/guard_files.txt"]
    ok, _, fails = run(table, "RB-S", ["src/agent/store.cpp"], root=str(tmp_path))
    assert ok, fails
    ok, _, fails = run(table, "RB-S", ["src/agent/other.cpp"], root=str(tmp_path))
    assert not ok


def test_declared_paths_are_read_from_the_cross_lane_section_only():
    body = (
        "# PR\n\nChanged `lib/x.cpp`.\n\n## Cross-lane edits\n\n- `src/main.cpp`: one include\n"
        "- `test/test_a/main.cpp` and `docs/a.md`\n\n## Risks\n\n- `src/net/webui.cpp` is not declared\n"
    )
    assert gate.declared_paths(body) == {"src/main.cpp", "test/test_a/main.cpp", "docs/a.md"}


def test_none_declared_when_the_section_says_none():
    assert gate.declared_paths("## Cross-lane edits\n\nNone.\n") == set()


# --- git: what the branch changed since its merge base --------------------------------


def git(root, *args):
    return subprocess.run(["git", "-C", root, *args], check=True, capture_output=True, text=True).stdout


def make_repo(tmp_path):
    root = str(tmp_path / "g")
    os.makedirs(root)
    git(root, "init", "-q", "-b", "rb-main")
    git(root, "config", "user.email", "t@example.invalid")
    git(root, "config", "user.name", "t")
    for rel in ("lib/core/src/orch_ledger.cpp", "src/main.cpp", "README.md"):
        p = os.path.join(root, rel)
        os.makedirs(os.path.dirname(p), exist_ok=True)
        with open(p, "w", encoding="utf-8") as f:
            f.write("x\n")
    git(root, "add", "-A")
    git(root, "commit", "-q", "-m", "base")
    git(root, "checkout", "-q", "-b", "lane/RB-A")
    return root


def commit(root, rel, text="y\n"):
    p = os.path.join(root, rel)
    os.makedirs(os.path.dirname(p), exist_ok=True)
    with open(p, "w", encoding="utf-8") as f:
        f.write(text)
    git(root, "add", "-A")
    git(root, "commit", "-q", "-m", rel)


def write_owners(tmp_path, table):
    p = tmp_path / "owners.json"
    p.write_text(json.dumps(table), encoding="utf-8")
    return str(p)


def test_main_reads_the_branch_diff_and_the_lane_from_the_branch(tmp_path, capsys):
    root = make_repo(tmp_path)
    commit(root, "lib/core/src/orch_ledger.cpp")
    path = write_owners(tmp_path, BASIC)
    assert gate.main(["--root", root, "--owners", path]) == 0
    assert "LANE FILES GREEN RB-A" in capsys.readouterr().out
    commit(root, "src/main.cpp")
    assert gate.main(["--root", root, "--owners", path]) == 1
    assert "LANE FILES RED RB-A" in capsys.readouterr().out


def test_counter_main_rename_out_of_the_entry_fails(tmp_path, capsys):
    # A move is a delete plus an add: both paths must be the lane's.
    root = make_repo(tmp_path)
    git(root, "mv", "lib/core/src/orch_ledger.cpp", "lib/core/src/orch_ledger_old.cpp")
    git(root, "commit", "-q", "-m", "move")
    assert gate.main(["--root", root, "--owners", write_owners(tmp_path, BASIC)]) == 1
    assert "orch_ledger_old.cpp" in capsys.readouterr().out


def test_counter_main_deleting_another_lanes_file_fails(tmp_path, capsys):
    root = make_repo(tmp_path)
    git(root, "rm", "-q", "src/main.cpp")
    git(root, "commit", "-q", "-m", "rm")
    assert gate.main(["--root", root, "--owners", write_owners(tmp_path, BASIC)]) == 1
    assert "src/main.cpp" in capsys.readouterr().out


def test_main_ignores_what_the_integration_branch_did_after_the_cut(tmp_path):
    root = make_repo(tmp_path)
    commit(root, "lib/core/src/orch_ledger.cpp")
    git(root, "checkout", "-q", "rb-main")
    commit(root, "src/main.cpp")  # another lane merged; the lane did not touch it
    git(root, "checkout", "-q", "lane/RB-A")
    assert gate.main(["--root", root, "--owners", write_owners(tmp_path, BASIC)]) == 0


def test_main_declared_file_option(tmp_path):
    root = make_repo(tmp_path)
    commit(root, "src/main.cpp")
    body = tmp_path / "PR_BODY.md"
    body.write_text("## Cross-lane edits\n\n- `src/main.cpp`\n", encoding="utf-8")
    path = write_owners(tmp_path, BASIC)
    assert gate.main(["--root", root, "--owners", path, "--declared", str(body)]) == 0


def test_counter_main_off_a_lane_branch_without_lane_fails(tmp_path, capsys):
    root = make_repo(tmp_path)
    git(root, "checkout", "-q", "rb-main")
    assert gate.main(["--root", root, "--owners", write_owners(tmp_path, BASIC)]) == 1
    assert "--lane" in capsys.readouterr().out


# --- overlaps: paths two lanes own, against the hot-file order ------------------------


def test_overlap_in_the_hot_file_order_is_sequenced():
    rows = gate.overlaps(BASIC)
    tg = [r for r in rows if r["path"] == "src/agent/telegram.cpp"]
    assert tg and tg[0]["sequenced"] and tg[0]["lanes"] == ["RB-A", "RB-B"]


def test_counter_overlap_outside_the_hot_file_order_is_flagged():
    table = owners(RB_A=["src/agent/jobs.cpp"], RB_B=["src/agent/jobs.cpp"])
    rows = gate.overlaps(table)
    assert [(r["path"], r["sequenced"]) for r in rows] == [("src/agent/jobs.cpp", False)]


def test_counter_hot_file_writer_missing_from_its_order_is_flagged():
    table = owners(RB_A=["src/agent/telegram.cpp"], RB_B=["src/agent/telegram.cpp"], RB_C=["src/agent/telegram.cpp"])
    rows = [r for r in gate.overlaps(table) if r["path"] == "src/agent/telegram.cpp"]
    assert rows and not rows[0]["sequenced"]
    assert rows[0]["outside_order"] == ["RB-C"]


def test_glob_overlaps_a_literal_path():
    table = owners(RB_A=["lib/harness/src/plain_*.cpp"], RB_B=["lib/harness/src/plain_engine.cpp"])
    rows = gate.overlaps(table)
    assert rows and rows[0]["path"] == "lib/harness/src/plain_engine.cpp"
    assert rows[0]["lanes"] == ["RB-A", "RB-B"]


# --- coverage of the plan --------------------------------------------------------------


def test_counter_a_plan_lane_without_an_entry_fails():
    plan = {"lanes": [{"id": "RB-A", "files_owned": ""}, {"id": "RB-NEW", "files_owned": "x.cpp"}]}
    errs = gate.plan_problems(BASIC, plan)
    assert any("RB-NEW" in e and "no entry" in e for e in errs)


def test_counter_files_owned_text_changed_in_the_plan_fails():
    table = owners(RB_A=["a.cpp"])
    table["lanes"]["RB-A"]["plan_text"] = "a.cpp"
    errs = gate.plan_problems(table, {"lanes": [{"id": "RB-A", "files_owned": "a.cpp, b.cpp"}]})
    assert any("RB-A" in e and "changed" in e for e in errs)


def test_counter_entry_for_a_lane_the_plan_dropped_fails():
    errs = gate.plan_problems(BASIC, {"lanes": [{"id": "RB-A", "files_owned": ""}]})
    assert any("RB-B" in e and "not in the plan" in e for e in errs)


# --- the real table ----------------------------------------------------------------------


def real_owners():
    return gate.load_owners(gate.OWNERS)


def test_real_owners_table_is_well_formed():
    table = real_owners()
    assert gate.table_problems(table) == []
    assert "RB-GATE0" in table["lanes"]


@pytest.mark.parametrize(
    "bad",
    [
        {"lanes": {"RB-A": {"files": ["/abs/path.cpp"], "plan_text": ""}}},
        {"lanes": {"RB-A": {"files": ["../up.cpp"], "plan_text": ""}}},
        {"lanes": {"RB-A": {"files": [], "plan_text": ""}}},
        {"lanes": {"RB-A": {"files": ["a.cpp"], "plan_text": ""}}, "hot_files": {"a.cpp": ["RB-GONE"]}},
    ],
)
def test_counter_malformed_owners_table_is_reported(bad):
    table = {"shared": [], "per_lane": [], "hot_files": {}, **bad}
    assert gate.table_problems(table) != []


def test_real_owners_table_covers_every_plan_lane():
    if not os.path.exists(PLAN):
        pytest.skip(f"the program's PLAN.json is not on this machine ({PLAN}); set RB_PLAN_JSON to check it")
    with open(PLAN, encoding="utf-8") as f:
        plan = json.load(f)
    assert gate.plan_problems(real_owners(), plan) == []


def test_real_owners_hot_files_match_the_prd_order():
    """PRD section 10's hot files, one writer at a time, in this order."""
    hot = real_owners()["hot_files"]
    assert hot["lib/harness/src/engine.cpp"] == [
        "RB-Q1a",
        "RB-Q1b",
        "RB-Q1c",
        "RB-G0",
        "RB-L1d",
        "RB-Q2a",
        "RB-Q2b",
        "RB-Q3a",
        "RB-Q4",
    ]
    assert hot["src/agent/telegram.cpp"] == ["RB-Q1c", "RB-TS1", "RB-TG1", "RB-TG2", "RB-P2"]
    assert hot["lib/harness/src/plain_engine.cpp"] == ["RB-E1a", "RB-E1b"]
    assert len(hot) >= 17
