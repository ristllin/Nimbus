#!/usr/bin/env python3
"""Self-test for context_report.py (runnable directly or under pytest; needs no board).

Three inputs:
- OLD: an excerpt of the 15 Nov 2026 Lumi logs (v5.1.0 and v5.1.1 lines, chat id replaced),
  every figure below counted by hand from these lines;
- NEW: synthetic lines in the v5.1.2 shapes (lane H context and budget lines, lane F rung,
  cycle and blob lines, lane W wms/pms and 'transport: up');
- the full saved Lumi logs, when the evidence folder is on this machine
  (NIMBUS_LUMI_EVIDENCE_DIR, or program-2026-10-18/evidence/lumi-history-2026-11-15 next to
  the checkout): the 15 Nov baseline must reproduce exactly. Skipped elsewhere.
"""

from __future__ import annotations

import importlib.util
import json
import os
import pathlib

import pytest

_HERE = pathlib.Path(__file__).resolve()
_SPEC = importlib.util.spec_from_file_location("context_report", _HERE.with_name("context_report.py"))
cr = importlib.util.module_from_spec(_SPEC)
_SPEC.loader.exec_module(cr)

OLD = """\
0 160 boot boot: n=20 fw=v5.1.0 build=v5.1.0 reason=software-restart(0x0c) wake=none mode=orch t=1794658683 ota=installing_v5.1.0 prev=n19/188370s/idle/h16064
408 20497297 - budget: ctx=128K sys=20971 res=5242 tot=41943 (auto)
409 20497479 - history: 18 message(s) left out to keep the request under 85000 B
411 20497532 - orchestrator: turn host=mistral (conv=0 heap=37404)
413 20505102 - transport: heaptrace api.mistral.ai/v1/chat/completions body=72402 pre=36612/8180 conn=37232/8180 wrote=37232/8180 done=37364/8180 fail=none af=0 min=30636 at=20498351 ms=6750
415 20505247 - fabric-loop: rounds=0 cap= ok=1 host=mistral switches=0 heap=37404
416 20505256 - fabric-spend: mistral/mistral-large-4-0 in=19170 out=321 cR=0 cW=0 n=1 id=mistral-large-4-0
418 20507906 - turn spend: ok owner | mistral/mistral-large-4-0 in=19170 out=321 cR=0 cW=0 n=1 id=mistral-large-4-0
419 20507922 - turn end: ok host=mistral tokens in=19170 out=321 tools=0 reply=696B src=owner
424 20581284 - budget: ctx=128K sys=20971 res=5242 tot=41943 (auto)
425 20581533 - history: 20 message(s) left out to keep the request under 85000 B
427 20581607 - orchestrator: turn host=mistral (conv=0 heap=37228)
429 20591325 - transport: heaptrace api.mistral.ai/v1/chat/completions body=82567 pre=37228/8180 conn=37056/8180 wrote=34808/8180 done=37188/8180 fail=none af=0 min=30028 at=20583578 ms=7746
432 20602602 - turn fit: left out 10 recalled memory item(s) (asked for 115 B, gave 3952 B)
433 20602664 - headloop: round 1 body 87339 B over the 85000 B budget - folded 6688 B of the replayed tool rounds then the prompt and input, running the round again
434 20610260 - transport: heaptrace api.mistral.ai/v1/chat/completions body=84743 pre=37228/8180 conn=37056/8180 wrote=36028/8180 done=37188/8180 fail=none af=0 min=30460 at=20602931 ms=7328
438 20622339 - turn fit: left out 10 recalled memory item(s); the attached food playbook (asked for 3078 B, gave 12754 B)
439 20622392 - headloop: round 2 body 88694 B over the 85000 B budget - folded 17466 B of the replayed tool rounds then the prompt and input, running the round again
454 20711343 - transport: heaptrace api.mistral.ai/v1/chat/completions body=81420 pre=37228/8180 conn=37056/8180 wrote=36444/8180 done=37188/8180 fail=none af=0 min=30460 at=20702785 ms=8557
456 20711515 - fabric-loop: rounds=7 cap= ok=1 host=mistral switches=0 heap=37228
457 20711521 - fabric-spend: mistral/mistral-large-4-0 in=166552 out=1693 cR=100096 cW=0 n=8 id=mistral-large-4-0
458 20711600 - telegram: msg from R (chat 1000000001): my private shopping list for saturday
459 20715960 - turn spend: ok owner | mistral/mistral-large-4-0 in=166552 out=1693 cR=100096 cW=0 n=8 id=mistral-large-4-0
460 20715976 - turn end: ok host=mistral tokens in=166552 out=1693 tools=10 reply=748B src=owner
0 158 boot boot: n=21 fw=v5.1.1 build=v5.1.1 reason=software-restart(0x0c) wake=none mode=orch t=1794714153 ota=installing_v5.1.1 prev=n20/55467s/idle/h15796
48 255239 - budget: ctx=128K sys=20971 res=5242 tot=41943 (auto)
50 255357 - orchestrator: turn host=mistral (conv=0 heap=37324)
52 266733 - fabric-loop: rounds=2 cap=rounds ok=1 host=mistral switches=0 heap=37396
53 266740 - fabric-spend: mistral/mistral-small in=50603 out=80 cR=25568 cW=0 n=3 id=mistral-small-2603
55 269211 - turn spend: ok loop | mistral/mistral-small in=50603 out=80 cR=25568 cW=0 n=3 id=mistral-small-2603
56 269379 - turn end: ok host=mistral tokens in=50603 out=80 tools=2 reply=0B src=loop
"""

FOLDS_OLD = """\
0 160 boot boot: n=10 fw=v4.6.4 build=v4.6.4 reason=software-restart(0x0c) wake=none mode=orch t=1793468556 ota=installing_v4.6.4 prev=n9/48475s/idle/h1732
1807 173494872 - fold(1000000001): ok on mistral (6306 in / 2097 out)
1808 173495221 - fold(1000000001): digest 5525 B, anchor m000017d9, shift -
2064 178837268 - fold(1000000001): no summary in reply ()
2067 178925218 - fold(1000000001): no summary in reply ()
2071 178983667 - fold(1000000001): no summary in reply ()
2072 178983700 - sfxsync: gate closed (sd=1 rawsd=0 fault=0 sta=1 heap=29584)
2073 178983800 - sfxsync: gate closed (sd=1 rawsd=0 fault=0 sta=1 heap=29584)
"""

NEW = """\
0 150 boot boot: n=1 fw=v5.1.2 build=v5.1.2 reason=poweron wake=none mode=orch t=1795000000 ota=none prev=-
10 1000 - budget: mistral/mistral-large-4-0 window 1024K target 100K tok (360 KB) hist 206/165/111 KB (auto)
11 1100 - context: sent 24/24 live rows (today 12/12), cut 0 (0 B), summary age 3.5h, round0 210000 B / budget 360000 B, hist 4 ms 0 card reads lock 3
12 1200 - orchestrator: turn host=mistral (conv=0 heap=37000)
13 9000 - transport: heaptrace api.mistral.ai/v1/chat/completions body=204800 pre=37000/8180 conn=37000/8180 wrote=36000/8180 done=37000/8180 fail=none af=0 min=30000 at=1300 ms=7700 wms=4000 pms=1500
14 9100 - transport: up 204800 B in 4000 ms, paced 1500
15 9200 - fabric-loop: rounds=0 cap= ok=1 host=mistral switches=0 heap=37000
16 9300 - fabric-spend: mistral/mistral-large-4-0 in=60000 out=500 cR=0 cW=0 n=1 id=mistral-large-4-0
17 9400 - turn spend: ok owner | mistral/mistral-large-4-0 in=60000 out=500 cR=0 cW=0 n=1 id=mistral-large-4-0
18 9500 - turn end: ok host=mistral tokens in=60000 out=500 tools=0 reply=200B src=owner
20 99300 - context: sent 20/22 live rows (today 11/12), cut 2 (3100 B), summary age 30h, round0 350000 B / budget 360000 B, hist 120 ms 3 card reads lock 40
21 99300 - orchestrator: turn host=mistral (conv=0 heap=37000)
22 110000 - transport: heaptrace api.mistral.ai/v1/chat/completions body=307200 pre=37000/8180 conn=37000/8180 wrote=36000/8180 done=37000/8180 fail=none af=0 min=30000 at=99400 ms=10000 wms=7200 pms=3000
23 110100 - transport: up 307200 B in 7200 ms, paced 3000
24 110200 - fabric-loop: rounds=1 cap= ok=1 host=mistral switches=0 heap=37000
25 110300 - fabric-spend: mistral/mistral-large-4-0 in=62000 out=700 cR=45000 cW=0 n=2 id=mistral-large-4-0
26 110400 - turn spend: ok owner | mistral/mistral-large-4-0 in=62000 out=700 cR=45000 cW=0 n=2 id=mistral-large-4-0
27 110500 - turn end: ok host=mistral tokens in=62000 out=700 tools=1 reply=300B src=owner
30 200000 - fold(1000000001): rung 1/3 mistral/mistral-medium-latest failed -> empty (0 B answered)
31 260000 - fold(1000000001): rung 2/3 anthropic/claude-haiku-5-5 failed -> http4xx (400)
32 320000 - fold(1000000001): cycle failed, next try 2026-11-18 10:15
33 1300000 - fold(1000000001): ok on openai (7000 in / 3000 out)
34 1300100 - fold: blob save 41234 B in 85 ms
"""


def write(tmp_path, name: str, text: str) -> str:
    p = tmp_path / name
    p.write_text(text)
    return str(p)


@pytest.fixture
def old(tmp_path):
    return cr.report(cr.parse([write(tmp_path, "old.log", OLD)]))


@pytest.fixture
def new(tmp_path):
    return cr.report(cr.parse([write(tmp_path, "new.log", NEW)]))


# ---- the 15 Nov excerpt (old lines) -------------------------------------------------------


def test_owner_turns_cut_and_left_out(old):
    h = old["history"]
    assert (h["owner_turns_cut"], h["owner_turns"]) == (2, 2)
    assert h["left_out_per_cut_turn"]["min"] == 18 and h["left_out_per_cut_turn"]["max"] == 20
    assert old["window"]["turns_by_source"] == {"loop": 1, "owner": 2}


def test_large_cache_share_and_in_turn_ceiling(old):
    ml = old["mistral_large"]
    assert (ml["turns"], ml["requests"], ml["input"], ml["cached_read"]) == (2, 9, 185722, 100096)
    # ceiling = 0 + 166,552 x 7/8 = 145,733 tokens of 185,722 input
    assert ml["cached_share_pct"] == 53.9
    assert ml["in_turn_ceiling_pct"] == 78.5
    assert ml["of_ceiling_pct"] == 68.7


def test_refits_turn_fits_and_capped_finals(old):
    rf = old["refits"]
    assert (rf["refusals"], rf["at_round0"], rf["after_round0"]) == (2, 0, 2)
    assert (rf["large_refit_turns"], rf["large_refits_after_round0"]) == (1, 2)
    assert rf["large_refit_shortfall_tokens"] == 45637  # 145,733 ideal - 100,096 read
    assert old["turn_fit"]["turn_fits"] == 2
    assert old["turn_fit"]["dropped"] == {"recalled": 2, "playbook": 1}
    assert old["capped"] == {"capped_finals": 1, "by_reason": {"rounds": 1}}


def test_cross_turn_sample_needs_a_previous_request_since_boot(old):
    x = old["xturn"]
    # turn 1 has no earlier request; turn 2 waited 76 s and read under the in-turn ideal;
    # the loop turn after the reboot has no earlier request either.
    assert (x["mistral_turns"], x["turns"], x["hits"]) == (3, 1, 0)
    assert [b["turns"] for b in x["by_idle"]] == [0, 1, 0, 0, 0, 0]


def test_costs_three_ways(old):
    c = old["cache"]["mistral/mistral-large-4-0"]["usd"]
    # ledger: 185,722 x $2 + 2,014 x $6; repo row with cache: 85,626 x $2 + 100,096 x $0.20 + out;
    # published: 85,626 x $0.70 + 100,096 x $0.07 + 2,014 x $2.10
    assert c == {"ledger": 0.38, "repo_cache": 0.2, "published": 0.07}


def test_wall_time_from_the_boot_line(old):
    # boot t=1794658683 at millis 160; the first event at millis 20,497,297: t + 20,497.137 s
    assert old["window"]["first"] == "2026-11-14 17:59:40 UTC"
    assert old["window"]["firmware"] == ["v5.1.0", "v5.1.1"]


def test_last_fold_and_lines_after_it(tmp_path):
    r = cr.report(cr.parse([write(tmp_path, "f.log", FOLDS_OLD)]))
    f = r["fold"]
    assert f["ok"] == 1 and f["last_ok"] == "2026-11-02 17:54:10 UTC"
    assert f["failures"] == {"no_summary": 3}
    assert f["lines_after_last_fold_line"] == 2


# ---- v5.1.2 lines (synthetic) -------------------------------------------------------------


def test_context_line_figures(new):
    c = new["history"]["context_lines"]
    assert c["turns"] == 2
    assert c["today_complete_pct"] == 50.0
    assert c["turns_with_cut"] == 1
    assert c["card_reads"]["max"] == 3 and c["hist_ms"]["max"] == 120 and c["lock_ms"]["max"] == 40
    assert c["summary_age_h"]["min"] == 3.5 and c["summary_age_h"]["max"] == 30.0
    assert c["cut_bytes"]["max"] == 3100


def test_new_lines_upload_and_cross_turn_hit(new):
    u = new["upload"]
    assert u["wms_per_kb"]["min"] == 20.0 and u["wms_per_kb"]["max"] == 24.0  # 4,000 ms / 200 KB, 7,200 / 300
    assert u["pms"]["max"] == 3000 and u["up_lines"] == 2 and u["up_over_6s"] == 1
    x = new["xturn"]
    assert (x["turns"], x["hits"]) == (1, 1)  # 45,000 read vs a 31,000 in-turn ideal: 14,000 cross-turn


def test_fold_rung_cycle_and_blob_lines(new):
    f = new["fold"]
    assert f["ok"] == 1
    assert f["failures"] == {"cycle_failed": 1, "rung_failed": 2}
    assert f["blob_save_ms"]["max"] == 85


# ---- turn boundaries and the counter-tests ------------------------------------------------


def test_history_line_before_turn_start_belongs_to_that_turn(tmp_path):
    text = (
        OLD.splitlines()[0]
        + "\n"
        + "\n".join(
            [
                "1 1000 - history: 6 message(s) left out to keep the request under 85000 B",
                "2 1100 - orchestrator: turn host=mistral (conv=0 heap=1)",
                "3 1200 - turn spend: ok owner | mistral/mistral-large-4-0 in=10 out=1 cR=0 cW=0 n=1",
                "4 1300 - turn end: ok host=mistral tokens in=10 out=1 tools=0 reply=1B src=owner",
                "5 1400 - orchestrator: turn host=mistral (conv=0 heap=1)",
                "6 1500 - turn spend: ok owner | mistral/mistral-large-4-0 in=10 out=1 cR=0 cW=0 n=1",
                "7 1600 - turn end: ok host=mistral tokens in=10 out=1 tools=0 reply=1B src=owner",
            ]
        )
    )
    h = cr.report(cr.parse([write(tmp_path, "b.log", text)]))["history"]
    assert (h["owner_turns_cut"], h["owner_turns"]) == (1, 2)


def test_a_boot_closes_a_turn_that_never_ended(tmp_path):
    boot = OLD.splitlines()[0]
    text = "\n".join(
        [
            boot,
            "1 1000 - history: 40 message(s) left out to keep the request under 85000 B",
            "2 1100 - orchestrator: turn host=mistral (conv=0 heap=1)",
            boot.replace("n=20", "n=21"),
            "1 1000 - history: 4 message(s) left out to keep the request under 85000 B",
            "2 1100 - orchestrator: turn host=mistral (conv=0 heap=1)",
            "3 1200 - turn spend: ok owner | mistral/mistral-large-4-0 in=10 out=1 cR=0 cW=0 n=1",
            "4 1300 - turn end: ok host=mistral tokens in=10 out=1 tools=0 reply=1B src=owner",
        ]
    )
    h = cr.report(cr.parse([write(tmp_path, "c.log", text)]))["history"]
    assert h["unfinished_turns"] == 1
    assert (h["owner_turns_cut"], h["owner_turns"]) == (1, 1)
    assert h["left_out_per_cut_turn"]["max"] == 4  # never 40 + 4 across the reboot


def test_a_capture_notice_is_not_a_turn(tmp_path):
    text = OLD.splitlines()[0] + "\n1 1000 - turn end: loop (routine) captured in quiet hours\n"
    assert cr.report(cr.parse([write(tmp_path, "d.log", text)]))["window"]["turns"] == 0


def test_turns_before_the_since_firmware_are_counted_apart(tmp_path):
    text = OLD.replace("fw=v5.1.0 build=v5.1.0", "fw=v4.6.4 build=v4.6.4")
    h = cr.report(cr.parse([write(tmp_path, "e.log", text)]))["history"]
    assert (h["owner_turns"], h["owner_turns_before"], h["owner_turns_cut_before"]) == (0, 2, 2)


def test_no_message_text_reaches_the_output(old):
    out = cr.text_report(old) + json.dumps(old)
    assert "shopping" not in out and "saturday" not in out


def test_rotation_order_is_oldest_first(tmp_path):
    for n in ("nimbus.log", "nimbus.log.1", "nimbus.log.3", "nimbus.log.2", "other.txt"):
        (tmp_path / n).write_text("")
    names = [pathlib.Path(p).name for p in cr.rotation_files(str(tmp_path))]
    assert names == ["nimbus.log.3", "nimbus.log.2", "nimbus.log.1", "nimbus.log"]


def test_check_baseline_reports_a_miss(old):
    misses = cr.check_baseline(old)
    assert any(m.startswith("owner_turns_cut: got 2") for m in misses)


def test_anthropic_share_counts_cache_outside_input(tmp_path):
    # Anthropic's input_tokens excludes cache reads and writes: the prompt is in + cR + cW.
    text = (
        OLD.splitlines()[0]
        + "\n"
        + "\n".join(
            [
                "1 1000 - orchestrator: turn host=anthropic (conv=0 heap=1)",
                "2 1200 - turn spend: ok owner | anthropic/claude-sonnet-5-5 in=1000 out=100 cR=90000 cW=9000 n=4",
                "3 1300 - turn end: ok host=anthropic tokens in=1000 out=100 tools=0 reply=1B src=owner",
                "4 2000 - orchestrator: turn host=cumulo (conv=0 heap=1)",
                "5 2200 - turn spend: ok owner | cumulo/anthropic/claude-sonnet-5-5 in=1000 out=100 cR=90000 cW=9000 n=4",
                "6 2300 - turn end: ok host=cumulo tokens in=1000 out=100 tools=0 reply=1B src=owner",
            ]
        )
    )
    c = cr.report(cr.parse([write(tmp_path, "a.log", text)]))["cache"]
    for key in ("anthropic/claude-sonnet-5-5", "cumulo/anthropic/claude-sonnet-5-5"):
        assert c[key]["prompt"] == 100000 and c[key]["cached_share_pct"] == 90.0
        assert c[key]["in_turn_ceiling_pct"] == 75.0
        # 1,000 x $2 + 90,000 x $0.20 + 9,000 x $2.50 + 100 x $10 per million, priced at the upstream's row
        assert c[key]["usd"]["ledger"] == round((1000 * 2 + 90000 * 0.2 + 9000 * 2.5 + 100 * 10) / 1e6, 2)


def test_repo_price_rows_come_from_the_header():
    rows = cr.repo_price_rows()
    assert rows[("mistral", "mistral-large-4-0")][:2] == (2.0, 6.0)
    assert rows[("anthropic", "claude-haiku-5-5")][2] == 0.01


# ---- the full 15 Nov baseline (only where the saved logs are) -----------------------------


def evidence_dir() -> pathlib.Path | None:
    cands = [os.environ.get("NIMBUS_LUMI_EVIDENCE_DIR", "")]
    tail = pathlib.Path("program-2026-10-18", "evidence", "lumi-history-2026-11-15")
    repo = _HERE.parents[1]
    cands += [str(p / tail) for p in (repo.parent, repo.parents[3] if len(repo.parents) > 3 else repo.parent)]
    for c in cands:
        if c and (pathlib.Path(c) / "nimbus.log.3").exists():
            return pathlib.Path(c)
    return None


@pytest.mark.skipif(evidence_dir() is None, reason="the saved Lumi logs are not on this machine")
def test_full_15_nov_baseline_reproduces():
    r = cr.report(cr.parse(cr.rotation_files(str(evidence_dir()))))
    assert cr.check_baseline(r) == []
    assert r["fold"]["last_ok"] == "2026-11-02 17:54:10 UTC"
    assert r["fold"]["lines_after_last_fold_line"] == 7805
    assert r["upload"]["largest_body_ok"] == 1000047 and r["upload"]["af_nonzero"] == 0
    assert r["refits"]["budgets"] == [85000]


if __name__ == "__main__":
    import sys

    sys.exit(pytest.main([__file__, "-q"]))
