"""Host tests for the plain engine's request ceilings gate (check_plain_ceilings.py, lane RB-E2).

Run: python3 -m pytest tools/release_gate/test_plain_ceilings.py

The first test runs the gate on the real tree. Each counter-test copies what the gate reads
into a temp folder, plants a fault and asserts the gate turns red: a 300 B addition to each
engine-2 class that composition reads, a figure over each ceiling, a missing role or
host, a malformed line, a class over its catalog ceiling and a stale catalog.
"""

import os
import re
import shutil

import pytest

import check_no_prompt_patch as npp
import check_plain_ceilings as gate
import gen_model_text_catalog as cat

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))
PAD = " and this sentence was planted by a test to grow the class by a few hundred bytes" * 4


def make_tree(dst):
    rels = {cat.E2_TEXTS, cat.CATALOGS["e2"]["golden"], gate.GOLDEN, npp.CHANGES}
    rels |= {p for p, _ in cat.E2_EXTRA_SOURCES}
    for rel in sorted(rels):
        os.makedirs(os.path.dirname(os.path.join(dst, rel)), exist_ok=True)
        shutil.copy(os.path.join(REPO, rel), os.path.join(dst, rel))
    return str(dst)


def edit(root, rel, fn):
    path = os.path.join(root, rel)
    with open(path, encoding="utf-8") as f:
        text = f.read()
    new = fn(text)
    assert new != text
    with open(path, "w", encoding="utf-8") as f:
        f.write(new)


def regenerate(root):
    entries = cat.build_e2(root)
    with open(os.path.join(root, cat.CATALOGS["e2"]["golden"]), "w", encoding="utf-8") as f:
        f.write(cat.render("e2", entries))


def test_real_tree_is_green():
    assert gate.run(REPO) == []
    assert gate.main(["--root", REPO]) == 0


def test_copied_tree_is_green(tmp_path):
    assert gate.run(make_tree(tmp_path)) == []


@pytest.mark.parametrize("cls,anchor", [("core", "kReplyForm[] = \""), ("tool_descriptions", "Search the web.")])
def test_a_seeded_300_byte_addition_is_red(tmp_path, cls, anchor):
    root = make_tree(tmp_path)
    edit(root, cat.E2_TEXTS, lambda t: t.replace(anchor, anchor + PAD[:300], 1))
    regenerate(root)
    errs = gate.run(root)
    assert any(e.startswith(f"e2 {cls}: model-facing text changed") for e in errs), errs
    # Not regenerated: the stale catalog is red as well.
    root2 = make_tree(tmp_path / "stale")
    edit(root2, cat.E2_TEXTS, lambda t: t.replace(anchor, anchor + PAD[:300], 1))
    assert any("is stale" in e for e in gate.run(root2))


def test_a_logged_growth_over_the_class_ceiling_is_red(tmp_path):
    root = make_tree(tmp_path)
    # Past the class ceiling from wherever the class stands now (4,095 B since RB-REC3).
    size = cat.class_stats("e2", cat.build_e2(root))["tool_descriptions"]["bytes"]
    big = "x " * ((cat.E2_CEILINGS["tool_descriptions"] - size) // 2 + 50)
    edit(root, cat.E2_TEXTS, lambda t: t.replace('"Search the web.', '"' + big + "Search the web.", 1))
    regenerate(root)
    errs = gate.run(root)
    assert any("tool_descriptions" in e and "over its ceiling" in e for e in errs), errs


@pytest.mark.parametrize(
    "figure,value",
    [
        ("core", 3073),
        ("index", 513),
        ("system", 8193),
        ("tools", 15),
        ("tools_bytes", 10241),
        ("longest_description", 241),
        ("digests", 6775),
    ],
)
def test_a_figure_over_its_ceiling_is_red(tmp_path, figure, value):
    root = make_tree(tmp_path)
    edit(
        root,
        gate.GOLDEN,
        lambda t: re.sub(rf"(measure role=member host=60s.*? {figure}=)\d+", rf"\g<1>{value}", t, count=1),
    )
    errs = gate.run(root)
    assert any(f"{figure} {value} is over its ceiling" in e for e in errs), errs


def test_a_missing_or_malformed_line_is_red(tmp_path):
    root = make_tree(tmp_path)
    edit(root, gate.GOLDEN, lambda t: re.sub(r"measure role=guest host=60s[^\n]*\n", "", t))
    assert any("role=guest host=60s" in e for e in gate.run(root))
    # One fixed pack (ruling 41): a line that names a pack again is malformed, not a new key.
    root_p = make_tree(tmp_path / "p")
    edit(
        root_p,
        gate.GOLDEN,
        lambda t: t.replace("measure role=owner host=120s", "measure role=owner pack=extra host=120s", 1),
    )
    assert any("malformed" in e for e in gate.run(root_p))
    root2 = make_tree(tmp_path / "m")
    edit(root2, gate.GOLDEN, lambda t: t.replace(" tools=14", " tools=fourteen", 1))
    assert any("malformed" in e for e in gate.run(root2))
    root3 = make_tree(tmp_path / "n")
    edit(root3, gate.GOLDEN, lambda t: t.replace(" index=", " idx=", 1))
    assert any("no index figure" in e for e in gate.run(root3))
    root4 = make_tree(tmp_path / "g")
    os.remove(os.path.join(root4, gate.GOLDEN))
    assert any("is missing" in e for e in gate.run(root4))
