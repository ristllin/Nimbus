"""Host tests for the tool-result imperative lint (check_tool_result_imperatives.py).

Run: python3 -m pytest tools/release_gate

Tool results state facts and never give orders (PRD section 4). The counter-tests seed
an order into a tool result and expect red; facts stay green; the engine-1 baseline only
shrinks; engine 2 has none.
"""

import re

import pytest

import check_no_prompt_patch as gate
import check_tool_result_imperatives as lint
import gen_model_text_catalog as cat
from test_no_prompt_patch import REPO, TEXTS_H, copy_template, edit_literal, make_tree, read, write


@pytest.fixture(scope="session")
def template(tmp_path_factory):
    return make_tree(str(tmp_path_factory.mktemp("template") / "t"))


@pytest.fixture
def tree(tmp_path, template):
    return copy_template(template, tmp_path)


@pytest.mark.parametrize(
    "text",
    [
        "Call files.read first, then answer.",
        "the file is missing; call files.read first",
        "Do not retry this call.",
        "not saved - ask the owner which rule to remove",
        "You must quote the receipt id.",
        "saved. Make sure the reply names the id.",
        "stored (remember it for the next turn)",
        "Tell the owner it is waiting on them",
        "the day is full. Never claim it was logged.",
    ],
)
def test_orders_are_flagged(text):
    assert lint.order_in(text), text


@pytest.mark.parametrize(
    "text",
    [
        "saved as open item q3",
        "no open items",
        "read 120 B; next_offset=120",
        "write failed",
        "list not supported on this device",
        "replaced 2; size 1,204 B",
        "wrote notes/a.md size=88 sha8 1a2b3c4d; replaced 40 B",
        "day 2026-11-17: 7 rows, 1,612 kcal, P 118.3 g; target 2,300 -> left 688 kcal",
        "the owner's rule list is full (12 of 12); the rule was stored unpinned",
        "sent message_id=81 bytes=2048 sha:0a1b2c3d",
        "Search results for coffee: 3",
        "List of files under notes/",
        "Report saved as r.pdf",
        "Reply sent to the owner",
        "Note added",
        "Start time: 09:00",
        "Return code 3",
        "Edit applied",
    ],
)
def test_facts_are_not_flagged(text):
    assert lint.order_in(text) is None, lint.order_in(text)


def test_real_tree_is_green():
    base = gate.resolve_base(REPO, None)
    assert lint.run(REPO, base, use_base=base is not None) == []


def test_copied_tree_is_green(tree):
    assert lint.run(tree, "main") == []


def _fact_result(tree):
    """A tool_results literal that states a fact (not on the baseline)."""
    for e in cat.parse(read(tree, cat.CATALOGS["e1"]["golden"]))["entries"]:
        if e.cls != "tool_results" or lint.order_in(e.text) or len(e.text) < 20:
            continue
        if re.fullmatch(r"[A-Za-z0-9 ,.;:()'/_-]+", e.text) and read(tree, e.path).count('"' + e.text + '"') == 1:
            return e.path, e.text
    raise AssertionError("no fact literal")


def test_a_seeded_order_in_a_tool_result_fails(tree):
    path, text = _fact_result(tree)
    edit_literal(tree, path, text, text + "; call files.read first")
    errs = lint.run(tree, "main")
    assert any("gives an order" in e and "call files.read" in e for e in errs), errs


def test_a_seeded_order_in_a_mixed_file_result_fails(tree):
    src = read(tree, "src/main.cpp")
    write(
        tree,
        "src/main.cpp",
        src + '\nToolResult planted() { return ToolResult::ok("done. You must tell the owner now"); }\n',
    )
    errs = lint.run(tree, "main")
    assert any("gives an order" in e and "src/main.cpp" in e for e in errs), errs


def test_a_baseline_row_whose_order_is_gone_must_be_deleted(tree):
    path, sha = lint.parse_baseline(read(tree, gate.CHANGES))[0]
    entry = next(
        e for e in cat.parse(read(tree, cat.CATALOGS["e1"]["golden"]))["entries"] if e.path == path and e.sha8 == sha
    )
    src = read(tree, path)
    lit = '"' + entry.text + '"'
    if lit not in src:
        pytest.skip("the first baseline template is not one plain literal")
    write(tree, path, src.replace(lit, '"the rest is stored"'))
    errs = lint.run(tree, "main")
    assert any("no longer gives an order or is gone" in e for e in errs), errs


def test_the_baseline_only_shrinks(tree):
    path, text = _fact_result(tree)
    edit_literal(tree, path, text, text + "; call files.read first")
    new = cat.Entry("tool_results", path, text + "; call files.read first")
    changes = read(tree, gate.CHANGES)
    row = f"| {path} | {new.sha8} | call files.read | planted |"
    marker = "| Source | sha8 | Order | Template (start) |\n|---|---|---|---|\n"
    assert marker in changes
    write(tree, gate.CHANGES, changes.replace(marker, marker + row + "\n", 1))
    errs = lint.run(tree, "main")
    assert any("was added; the baseline only shrinks" in e for e in errs), errs


def test_e2_results_and_stubs_have_no_baseline(tree):
    write(
        tree,
        cat.E2_TEXTS,
        TEXTS_H.format(core="").replace("wrote {path} size={size}", "wrote {path}; now read it back with read_file"),
    )
    errs = lint.run(tree, "main")
    assert any("gives an order" in e and "texts.h" in e for e in errs), errs


def test_main_markers(tree, capsys):
    assert lint.main(["--root", tree, "--base", "main"]) == 0
    assert capsys.readouterr().out.strip().splitlines()[-1] == "TOOL RESULT IMPERATIVES GREEN"
    path, text = _fact_result(tree)
    edit_literal(tree, path, text, text + ". Do not retry.")
    assert lint.main(["--root", tree, "--base", "main"]) == 1
    assert capsys.readouterr().out.strip().splitlines()[-1] == "TOOL RESULT IMPERATIVES RED"
