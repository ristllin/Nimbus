"""Host tests for tools/release_gate/check_router_release.py, the engine-2 router's release gate.

The gate passes on the tree (Anthropic releasable with its waivers named, the others not marked);
each way a provider could ship on synthetic fixtures or unrecorded request shapes is planted in a
scratch tree and must fail.
"""

import copy
import json
import sys
from pathlib import Path

import pytest

HERE = Path(__file__).resolve().parent
REPO = HERE.parents[1]
sys.path.insert(0, str(HERE))

import check_router_release as rr  # noqa: E402

DATA = rr.gen_caps.load()
RECORD = json.loads(rr.RECORD.read_text(encoding="utf-8"))


def test_the_tree_passes():
    assert rr.main() == 0


def test_features_follow_the_rows():
    allowed = rr.allowed_features(DATA)
    assert allowed["anthropic"] == set(rr.FEATURES)
    assert "tool_choice_none" not in allowed["zai"]  # Z.ai rows never allow none
    assert "routed_path" not in allowed["custom_chat"]  # a custom endpoint is never routed


@pytest.fixture()
def tree(tmp_path):
    """A scratch tree with the real Anthropic fixtures (three synthetic among them)."""
    src = REPO / "test/support/fixtures/anthropic_wire_2026-11"
    dst = tmp_path / "test/support/fixtures/anthropic_wire_2026-11"
    dst.mkdir(parents=True)
    for f in src.iterdir():
        (dst / f.name).write_bytes(f.read_bytes())
    return tmp_path


def fails(tree, record):
    return rr.run(tree, record, DATA)[0]


def test_the_real_record_passes_on_the_scratch_tree(tree):
    assert fails(tree, RECORD) == []


def test_a_new_synthetic_fixture_blocks_a_releasable_slot(tree):
    (tree / "test/support/fixtures/anthropic_wire_2026-11/synthetic_new.json").write_text("{}")
    f = fails(tree, RECORD)
    assert len(f) == 1 and "synthetic_new.json" in f[0]


def test_a_header_line_or_a_synthetic_folder_marks_a_fixture(tree):
    (tree / "test/support/fixtures/anthropic_wire_2026-11/odd_shape.json").write_text(
        "synthetic: https://x, read 2026-11-24\n{}"
    )
    d = tree / "test/support/fixtures/anthropic_wire_synthetic"
    d.mkdir()
    (d / "answer.json").write_text("{}")
    assert len(fails(tree, RECORD)) == 2


def test_router_fixtures_count_for_every_routed_slot(tree):
    d = tree / "test/support/fixtures/router_wire_synthetic"
    d.mkdir()
    (d / "refusal.json").write_text("{}")
    f = fails(tree, RECORD)
    assert len(f) == 1 and f[0].startswith("anthropic:")
    synthetic = rr.synthetic_fixtures(tree, rr.allowed_features(DATA))
    assert synthetic["openai"] and not synthetic["custom_chat"]


def test_marking_a_slot_releasable_without_recordings_fails(tree):
    rec = copy.deepcopy(RECORD)
    rec["providers"]["openai"]["releasable"] = True
    f = fails(tree, rec)
    assert f and all(x.startswith("openai: releasable") for x in f)
    d = tree / "test/support/fixtures/openai_wire_synthetic"
    d.mkdir()
    (d / "function_call.json").write_text("{}")
    assert any("function_call.json" in x for x in fails(tree, rec))


def test_a_dropped_recording_fails(tree):
    rec = copy.deepcopy(RECORD)
    del rec["providers"]["anthropic"]["recorded"]["id_rule"]
    f = fails(tree, rec)
    assert len(f) == 1 and "id_rule" in f[0]


def test_stale_and_unreasoned_waivers_and_bad_citations_fail(tree):
    rec = copy.deepcopy(RECORD)
    rec["providers"]["anthropic"]["waived"]["test/support/fixtures/anthropic_wire_2026-11/gone.json"] = "was here"
    rec["providers"]["anthropic"]["waived"]["routed_path"] = " "
    rec["providers"]["anthropic"]["recorded"]["structured_summary"] = "test/support/fixtures/nope.json"
    f = fails(tree, rec)
    assert any("stale waiver" in x for x in f)
    assert any("no reason" in x for x in f)
    assert any("does not exist" in x for x in f)


def test_a_slot_missing_from_the_record_fails(tree):
    rec = copy.deepcopy(RECORD)
    del rec["providers"]["zai"]
    assert fails(tree, rec) == ["slot zai: missing from the release record"]
