"""The domain-words lint (tools/check_no_domain_words.py): what it matches, what it never matches,
its scope, and that a hit fails it (owner ruling 41, ruling 42 item 4)."""

import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))
import check_no_domain_words as lint  # noqa: E402

REPO = os.path.normpath(os.path.join(os.path.dirname(__file__), "..", ".."))


def _tree(tmp_path, files):
    for rel, text in files.items():
        p = tmp_path / rel
        p.parent.mkdir(parents=True, exist_ok=True)
        p.write_text(text)
    return str(tmp_path)


def test_whole_tokens_and_identifier_parts_match():
    for line in (
        "// the food log",
        "int foodLedger = 0;",
        "FOOD_LOG",
        "registerFoodTools(reg);",
        '"a meal row"',
        "kcal_total",
        "Calories",
        "protein_g",
        "Nutrition facts",
        "520 kJ",
        "meals.csv",
    ):
        assert lint.line_hits(line), line


def test_words_that_only_contain_a_domain_word_never_match():
    for line in (
        "themeAlertHue",
        "centsPerKCalls",
        "TickCallback",
        "seafoodish_wordless",  # one token, no domain part
        "the usage ledger",
        "#define A_MACRO 1 // macros",
        "the expenses log and the training log",
    ):
        assert not lint.line_hits(line), line


def test_a_hit_in_scope_fails_and_report_mode_passes(tmp_path, capsys):
    root = _tree(tmp_path, {"lib/core/src/a.cpp": "// a food rule\n", "src/b.cpp": "int x;\n"})
    assert lint.main(["--root", root]) == 1
    assert "lib/core/src/a.cpp:1: [food]" in capsys.readouterr().out
    assert lint.main(["--root", root, "--report"]) == 0


def test_every_scoped_directory_is_scanned(tmp_path):
    for top in ("lib/core", "lib/harness", "src", "nimbusd/src"):
        root = _tree(tmp_path / top.replace("/", "_"), {top + "/x.h": "// kcal\n"})
        assert lint.main(["--root", root]) == 1, top


def test_the_generated_docs_pack_is_in_scope(tmp_path):
    root = _tree(tmp_path, {"lib/core/include/nimbus/docs_pack_data.h": 'R"(a meal)"\n'})
    assert lint.main(["--root", root]) == 1


def test_tests_and_out_of_scope_trees_are_not_scanned(tmp_path):
    root = _tree(
        tmp_path,
        {
            "nimbusd/tests/test_x.cpp": "// a food CSV, an expenses CSV and a training CSV\n",
            "test/test_x/main.cpp": "// food\n",
            "tests/evals/x.py": "# food\n",
            "docs/x.md": "food\n",
        },
    )
    assert lint.main(["--root", root]) == 0


def test_the_allowlist_is_the_usage_ledger_only():
    assert set(lint.ALLOWLIST) == {
        "lib/core/src/orch_usage_ledger.cpp",
        "lib/core/include/nimbus/orch/usage_ledger.h",
    }
