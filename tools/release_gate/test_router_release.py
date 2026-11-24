"""Host tests for tools/release_gate/check_router_release.py, the engine-2 router's release gate.

The gate passes on the tree (Anthropic direct releasable with its class waivers, every routed route
and every other provider not marked) and with the error fixture folders lane E2-R5 adds; each way a
route could ship on synthetic fixtures or unrecorded request shapes is planted in a scratch tree
and must fail.
"""

import copy
import json
import shutil
import sys
from pathlib import Path

import pytest

HERE = Path(__file__).resolve().parent
REPO = HERE.parents[1]
sys.path.insert(0, str(HERE))

import check_router_release as rr  # noqa: E402

DATA = rr.gen_caps.load()
RECORD = json.loads(rr.RECORD.read_text(encoding="utf-8"))
FIX = "test/support/fixtures"


def test_the_tree_passes():
    assert rr.main() == 0


def test_features_and_routes_follow_the_rows():
    allowed = rr.allowed_features(DATA)
    routes = rr.routes_of(DATA)
    assert allowed["anthropic"] == set(rr.FEATURES)
    assert "tool_choice_none" not in allowed["zai"]  # Z.ai rows never allow none
    assert routes["custom_chat"] == ("direct",)  # a custom endpoint is never routed
    assert "routed_path" not in rr.route_features(allowed["anthropic"], "direct")


@pytest.fixture()
def tree(tmp_path):
    """A scratch copy of every wire fixture folder of the tree."""
    for d in (REPO / FIX).iterdir():
        if d.is_dir() and "_wire" in d.name:
            shutil.copytree(d, tmp_path / FIX / d.name)
    return tmp_path


def fails(tree, record):
    return rr.run(tree, record, DATA)[0]


def report(tree, record):
    return rr.run(tree, record, DATA)[1]


def test_the_real_record_passes_on_the_scratch_tree(tree):
    assert fails(tree, RECORD) == []


def test_error_fixture_folders_of_every_provider_pass_and_are_listed(tree):
    # The shape lane E2-R5 adds: <prefix>_wire_synthetic/errors/cases.json for each prefix.
    for prefix in ("anthropic", "openai", "mistral", "zai", "router"):
        d = tree / FIX / f"{prefix}_wire_synthetic" / "errors"
        d.mkdir(parents=True, exist_ok=True)
        (d / "cases.json").write_text('{"provider": "%s"}' % prefix)
    assert fails(tree, RECORD) == []
    new = f"{FIX}/anthropic_wire_synthetic/errors/cases.json"
    assert any(
        new in line and "class test/support/fixtures/anthropic_wire_synthetic/" in line for line in report(tree, RECORD)
    )


def test_router_fixtures_count_only_for_routed_routes(tree):
    synthetic = rr.synthetic_fixtures(tree, rr.routes_of(DATA))
    router = [f for f in synthetic[("anthropic", "routed")] if "router_wire" in f]
    assert router and not any("router_wire" in f for f in synthetic[("anthropic", "direct")])
    rec = copy.deepcopy(RECORD)
    rec["providers"]["anthropic"]["routed"]["releasable"] = True
    f = fails(tree, rec)
    assert any("router_wire_synthetic" in x for x in f) and all(x.startswith("anthropic routed") for x in f)


def test_a_synthetic_fixture_outside_every_waived_class_fails(tree):
    (tree / FIX / "anthropic_wire_2026-11" / "odd_shape.json").write_text("synthetic: https://x, read 2026-11-24\n{}")
    f = fails(tree, RECORD)
    assert len(f) == 1 and "odd_shape.json" in f[0] and f[0].startswith("anthropic direct")


def test_a_new_file_in_a_waived_class_is_listed_not_failed(tree):
    (tree / FIX / "anthropic_wire_2026-11" / "synthetic_new.json").write_text("{}")
    assert fails(tree, RECORD) == []
    assert any("synthetic_new.json" in line for line in report(tree, RECORD))


def test_marking_a_provider_releasable_without_recordings_fails(tree):
    rec = copy.deepcopy(RECORD)
    rec["providers"]["openai"]["direct"]["releasable"] = True
    f = fails(tree, rec)
    assert f and all(x.startswith("openai direct: releasable") for x in f)
    assert any("openai_wire_synthetic" in x for x in f) and any("id_rule" in x for x in f)


def test_a_dropped_recording_fails(tree):
    rec = copy.deepcopy(RECORD)
    del rec["providers"]["anthropic"]["direct"]["recorded"]["id_rule"]
    f = fails(tree, rec)
    assert len(f) == 1 and "id_rule" in f[0]


def test_bad_waivers_and_citations_fail(tree):
    rec = copy.deepcopy(RECORD)
    w = rec["providers"]["anthropic"]["direct"]["waived"]
    w[f"{FIX}/anthropic_wire_2026-11/gone.json"] = {"why": "was here", "until": "R10"}
    w["tool_choice_none"] = {"why": "no end named"}
    w["structured_summary"] = "a bare string"
    rec["providers"]["anthropic"]["direct"]["recorded"]["image_in_tool_result"] = f"{FIX}/nope.json"
    f = fails(tree, rec)
    assert any("stale waiver" in x for x in f)
    assert sum("needs a 'why' and an 'until'" in x for x in f) == 2
    assert any("does not exist" in x for x in f)


def test_a_slot_or_route_missing_from_the_record_fails(tree):
    rec = copy.deepcopy(RECORD)
    del rec["providers"]["zai"]
    assert fails(tree, rec) == ["slot zai: missing from the release record"]
    rec = copy.deepcopy(RECORD)
    del rec["providers"]["anthropic"]["routed"]
    assert len(fails(tree, rec)) == 1


def test_the_scope_gate_reads_releasable_routes():
    assert rr.releasable_routes(RECORD) == {"anthropic-direct"}
