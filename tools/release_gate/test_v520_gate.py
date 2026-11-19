"""Host tests of check_v520_gate.py (lane RB-REC7): the real data is green, and each way the rows can rot is red."""

import copy
import json
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import check_v520_gate as G  # noqa: E402

DATA = json.load(open(G.DATA, encoding="utf-8"))


def test_the_real_rows_are_green():
    assert G.problems(DATA) == []
    assert G.main([]) == 0


def test_a_missing_row_is_red():
    for row in ("history-never-reduced", "cut-during-edit", "families-parity"):
        d = copy.deepcopy(DATA)
        d["rows"] = [r for r in d["rows"] if r["id"] != row]
        assert any(row in p for p in G.problems(d)), row


def test_a_row_naming_a_path_the_tree_lacks_is_red():
    d = copy.deepcopy(DATA)
    d["rows"][0]["paths"] = ["tests/evals/no_such_suite"]
    assert any("not in the tree" in p for p in G.problems(d))


def test_a_removed_gate_coming_back_is_red(tmp_path):
    (tmp_path / "tests" / "evals" / "food_replay").mkdir(parents=True)
    for row in DATA["rows"]:
        for p in row["paths"]:
            target = tmp_path / p
            target.parent.mkdir(parents=True, exist_ok=True)
            target.touch() if "." in os.path.basename(p) else target.mkdir(exist_ok=True)
    assert any("food replay" in p for p in G.problems(DATA, str(tmp_path)))


def test_a_gate_script_running_the_food_coach_eval_is_red(tmp_path):
    gate = tmp_path / "tools" / "release_gate"
    gate.mkdir(parents=True)
    (gate / "run_gate.py").write_text("subprocess.run(['python3', 'tests/evals/food_coach/run_eval.py'])\n")
    assert any("food coach" in p for p in G.problems({"rows": [], "removed": []}, str(tmp_path)))
