#!/usr/bin/env python3
"""Tests for check_fold_health.py: a clean tree passes, and every planted violation fails it.

Each counter-test starts from the same clean synthetic tree (no pause, fold control on the
device and the hosted instance, a v5.1.1 and a candidate blob that round-trip) and plants
one fault the gate exists for. The real tree at v5.1.1 fails it (the 2 Nov 2026 breaker and
thrash pauses, the pause copies, no fold control, no candidate blob): see the last test.
"""

from __future__ import annotations

import os
import sys

import pytest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import check_fold_health as G  # noqa: E402

US, RS = "\x1f", "\x1e"


def record(chat="c1", paused="0", summary="1. Decisions & preferences\n- keep it short.", nums=None):
    nums = nums or ["4000", "12", "1795000000;m00000100", "3;m00000120", "0"]
    return US.join([chat, *nums, paused, summary]) + RS


def write(root, rel, text, binary=False):
    path = os.path.join(root, rel)
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "wb" if binary else "w") as f:
        f.write(text.encode() if binary else text)


@pytest.fixture
def tree(tmp_path):
    root = str(tmp_path)
    write(root, "lib/core/src/orch_compact.cpp", "void noteFoldFailed() { f.failedCycles++; f.nextTry = now + 900; }\n")
    write(
        root,
        "lib/core/include/nimbus/orch/fold_ctl.h",
        "struct FoldCtl { uint32_t paidDay, paidCount, lastAlertDay; std::string manualPendingChat, fwAtLastFail; };\n",
    )
    write(root, "src/agent/orch_persist.cpp", 'static const char* kFoldCtlPath = "/fold_ctl.txt";\n')
    write(
        root,
        "nimbusd/src/hosted_fold.h",
        'inline std::string foldCtlPath(const std::string& d) { return d + "/fold_ctl.txt"; }\n',
    )
    write(root, "nimbusd/src/rig.h", 'record(chat, "Summary updated.");\n')
    write(
        root,
        "test/support/fixtures/fold_blob_v5_1_1_x.txt",
        record(paused="1", nums=["61234", "187", "1793642050;m1", "158;m2", "3"]),
        True,
    )
    write(
        root,
        "test/support/fixtures/fold_blob_v5_1_2_x.txt",
        record(nums=["10", "1", "1795000000;m3;900", "0;m4", "0;1;2"]),
        True,
    )
    return root


def test_clean_tree_passes(tree):
    assert G.problems(tree) == []
    assert G.main(["--root", tree]) == 0


@pytest.mark.parametrize(
    "rel,line,why",
    [
        ("lib/core/src/orch_compact.cpp", "void f() { s->f.breakerPaused = true; }\n", "sets a pause flag"),
        ("lib/harness/src/fold_driver.cpp", "if (refill) { s.thrashPaused = true; }\n", "sets a pause flag"),
        ("lib/core/src/orch_compact.cpp", "FoldDue g() { return FoldDue::ThrashPaused; }\n", "returns ThrashPaused"),
        ("lib/core/include/nimbus/orch/compact.h", "if (++fails >= 3) { paused = true; return true; }\n", "pauses"),
        ("src/agent/orchestrator_e1.cpp", 'send("Automatic summaries of this chat are paused.");\n', "paused"),
        ("nimbusd/src/rig.h", 'record(chat, "Summaries keep failing, so they are paused.");\n', "paused"),
    ],
)
def test_a_planted_pause_fails(tree, rel, line, why):
    path = os.path.join(tree, rel)
    old = open(path).read() if os.path.exists(path) else ""
    write(tree, rel, old + line)
    errs = G.problems(tree)
    assert any(why in e and rel in e for e in errs), errs


def test_a_pause_named_only_in_a_comment_is_not_a_finding(tree):
    write(tree, "lib/core/src/orch_compact.cpp", "// v5.1.1 set breakerPaused = true here; never again\nint x;\n")
    assert G.pause_problems(tree) == []


@pytest.mark.parametrize("field", G.FOLD_CTL_FIELDS)
def test_a_missing_fold_control_field_fails(tree, field):
    path = os.path.join(tree, "lib/core/include/nimbus/orch/fold_ctl.h")
    write(tree, "lib/core/include/nimbus/orch/fold_ctl.h", open(path).read().replace(field, "other"))
    assert any(field in e for e in G.problems(tree))


def test_no_device_or_hosted_fold_control_file_fails(tree):
    write(tree, "src/agent/orch_persist.cpp", "int x;\n")
    write(tree, "nimbusd/src/hosted_fold.h", "int y;\n")
    errs = G.problems(tree)
    assert any("/fold_ctl.txt" in e for e in errs) and any("hosted instance" in e for e in errs)


def test_a_missing_candidate_blob_fails(tree):
    os.remove(os.path.join(tree, "test/support/fixtures/fold_blob_v5_1_2_x.txt"))
    assert any("candidate fold blob" in e for e in G.problems(tree))


@pytest.mark.parametrize(
    "blob,why",
    [
        (record(paused="1"), "rollback would start paused"),
        (
            record(nums=["4000", "12", "1795000000;m1", "3;m2", "2;2;1795000000;1795003600"]),
            "2 failure(s) from its pause",
        ),
        (record(nums=["n/a", "12", "1795000000", "3", "0"]), "reads as no number"),
        (US.join(["c1", "4000", "12", "1795000000"]) + RS, "fields"),
        (record(summary="x" * 7000), "over 6144"),
        ("", "no record"),
    ],
)
def test_a_candidate_blob_that_does_not_round_trip_fails(tree, blob, why):
    write(tree, "test/support/fixtures/fold_blob_v5_1_2_x.txt", blob, True)
    errs = G.problems(tree)
    assert any(why in e for e in errs), errs


def test_an_oversize_blob_fails(tree):
    big = "".join(record(chat=f"c{i}", summary="s" * 4000) for i in range(20))
    write(tree, "test/support/fixtures/fold_blob_v5_1_2_x.txt", big, True)
    assert any("over the 65536 B blob cap" in e for e in G.problems(tree))


def test_suffixes_read_the_way_v511_reads_them():
    # toU32 stops at the first non-digit: a ';' suffix on a number field is skipped, not an error.
    assert G.to_u32("1793642050;m000017d9") == 1793642050
    assert G.to_u32("0;1;2") == 0
    assert G.to_u32("x12") is None


def test_the_committed_lumi_shaped_v511_blob_reads():
    p = os.path.join(G.REPO, "test", "support", "fixtures", "fold_blob_v5_1_1_lumi.txt")
    assert G.blob_problems(p, candidate=False) == []
    recs = G.legacy_records(open(p, encoding="utf-8").read())
    assert recs[0][5:7] == ["3", "1"]  # breaker fails=3, paused=1: the 2 Nov 2026 state


def test_this_tree_passes_the_gate():
    # At v5.1.1 (591245f4) the gate reported 16 problems here: the breaker and thrash pauses, the
    # pause copies, no fold control, no candidate blob (lane E phase 1). Lane F removed them.
    assert G.problems(G.REPO) == []


def test_the_candidate_blobs_the_replays_left_read_under_v511():
    # The fold-store files a hosted instance wrote on the candidate (run_offline.py blob): a chat
    # healed from the planted pause, and a chat whose cycle just failed (failures 0;1;first;next).
    fx = os.path.join(G.REPO, "test", "support", "fixtures")
    for name, schedule in (
        ("fold_blob_v5_1_2_replay_healed.txt", False),
        ("fold_blob_v5_1_2_replay_failing.txt", True),
    ):
        path = os.path.join(fx, name)
        assert G.blob_problems(path, candidate=True) == []
        rec = G.legacy_records(open(path, encoding="utf-8").read())[0]
        assert rec[0] == "owner" and rec[6] == "0" and G.to_u32(rec[5]) == 0
        assert (rec[5].count(";") == 3 and rec[5].split(";")[1] == "1") == schedule


if __name__ == "__main__":
    sys.exit(pytest.main([__file__, "-q"]))
