"""Host tests for the food coach eval: no network, no keys, no device.

They pin the parts that make a result trustworthy: the scenarios are well formed,
the world the driver is given matches what the judge is told, the judge's answer
is read strictly (a missing verdict fails), the verdict rules, the pricing, and
the runner end to end with a fake driver and a fake judge.
"""

import datetime as dt
import json
import re
import stat
import sys
from pathlib import Path

import pytest

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))

import fcjudge  # noqa: E402
import fcreport  # noqa: E402
import fcrun  # noqa: E402
import fcworld  # noqa: E402
import run_eval  # noqa: E402

CFG = fcworld.load_config()
SCENARIOS = fcworld.load_scenarios()
TODAY = dt.date(2026, 10, 26)
EM_DASH = chr(0x2014)  # built, not written: the project bans the character itself


# ---- scenarios -------------------------------------------------------------------
def test_there_are_scenarios_for_every_seen_failure():
    ids = {s["id"] for s in SCENARIOS}
    assert {"qty-vague", "qty-pressed", "plan-stated", "plan-earlier", "complaint-recovery"} <= ids
    assert any(s["kind"] == "guard" for s in SCENARIOS), "an over-correction guard is required"


@pytest.mark.parametrize("scn", SCENARIOS, ids=lambda s: s["id"])
def test_scenario_is_well_formed(scn):
    assert fcworld.validate_scenario(scn, CFG) == []
    assert (HERE / "scenarios" / f"{scn['id']}.json").exists()


@pytest.mark.parametrize("scn", SCENARIOS, ids=lambda s: s["id"])
def test_scenario_text_follows_the_copy_rules(scn):
    text = json.dumps(scn, ensure_ascii=False)
    assert EM_DASH not in text, "no em dash anywhere"
    for chunk in (HERE / "rubric.md", HERE / "README.md"):
        if chunk.exists():
            assert EM_DASH not in chunk.read_text(encoding="utf-8")


def test_invalid_scenarios_are_reported():
    bad = {
        "id": "x",
        "title": "t",
        "kind": "oops",
        "seen": "s",
        "world": {"log_set": "nope"},
        "turns": [""],
        "facts": ["{missing}"],
        "criteria": [{"id": "a"}, {"id": "a"}],
    }
    problems = " ".join(fcworld.validate_scenario(bad, CFG))
    for needle in ("kind", "log_set", "turns", "unique", "required", "facts"):
        assert needle in problems


def test_history_must_alternate_and_start_with_the_owner():
    assert fcworld._history_problems([{"role": "assistant", "text": "x"}])
    assert fcworld._history_problems([{"role": "user", "text": "x"}, {"role": "user", "text": "y"}])
    assert fcworld._history_problems([{"role": "user", "text": "x"}, {"role": "assistant", "text": "y"}]) == []


def test_log_set_notes_state_the_true_totals():
    for name, ls in fcworld.log_sets()["sets"].items():
        kcal, protein = fcworld.totals(ls["rows"])
        m = re.search(r"Logged ([\d,]+) kcal and ([\d.]+) g protein", ls["note"])
        assert m, name
        assert int(m.group(1).replace(",", "")) == kcal and float(m.group(2)) == protein, name


def test_facts_use_the_log_the_driver_is_seeded_with():
    scn = fcworld.load_scenarios(["qty-vague"])[0]
    facts = " ".join(fcworld.facts_for(scn, CFG))
    values = fcworld.fact_values(scn, CFG)
    assert values["remaining_kcal"] == "800" and "800 kcal remain" in facts
    payload = fcworld.run_payload(scn, CFG, TODAY)
    csv = payload["seed"]["files"][0]["text"]
    rows = csv.strip().splitlines()[1:]
    assert sum(float(r.split(",")[7]) for r in rows) == 1600
    assert all(r.startswith("2026-10-26,") for r in rows)


def test_payload_carries_the_world_a_device_would_have():
    scn = fcworld.load_scenarios(["plan-earlier"])[0]
    p = fcworld.run_payload(scn, CFG, TODAY)
    assert p["profile"]["directive"].startswith("Your name is ") and p["profile"]["clock"] is True
    assert p["seed"]["skills"][0]["id"] == "meal-photo-science"
    assert "2400 kcal" in p["seed"]["memory"][0]
    assert len(p["seed"]["history"]) == 6 and p["turns"] == scn["turns"]


def test_unknown_scenario_id_is_an_error():
    with pytest.raises(KeyError):
        fcworld.load_scenarios(["nope"])


# ---- judge -----------------------------------------------------------------------
def _turn(reply="Eat 300 g of pasta and 4 slices.", **kw):
    return {"owner": "How much?", "reply": reply, "tool_calls": [], "tool_results": [], "ok": True, **kw}


def test_transcript_shows_history_calls_and_every_message():
    text = fcjudge.render_transcript(
        [{"role": "user", "text": "earlier q"}, {"role": "assistant", "text": "earlier a"}],
        [_turn(deliveries=["one", "two"], tool_calls=['files_read({"a":1})'], tool_results=["rows"])],
    )
    for needle in (
        "not graded",
        "OWNER: earlier q",
        "ASSISTANT: earlier a",
        "ASSISTANT CALLED",
        "TOOL RESULT: rows",
        "MESSAGE 1 of 2",
        "MESSAGE 2 of 2",
    ):
        assert needle in text
    assert "(no reply was delivered)" in fcjudge.render_transcript([], [_turn(reply="")])


def test_judge_prompt_lists_every_criterion_and_the_facts():
    scn = fcworld.load_scenarios(["plan-stated"])[0]
    prompt = fcjudge.build_user_prompt("RUBRIC", scn, ["FACT ONE"], "TRANSCRIPT")
    assert "RUBRIC" in prompt and "FACT ONE" in prompt and "TRANSCRIPT" in prompt
    assert all(c["id"] in prompt for c in scn["criteria"])


def test_a_missing_verdict_fails_and_advisory_does_not_gate():
    scn = fcworld.load_scenarios(["plan-stated"])[0]
    required = [c["id"] for c in scn["criteria"] if c["required"]]
    all_pass = {"criteria": [{"id": c["id"], "verdict": "pass", "evidence": "e"} for c in scn["criteria"]]}
    assert fcjudge.normalize(all_pass, scn)["pass"] is True
    dropped = {"criteria": all_pass["criteria"][1:]}
    assert fcjudge.normalize(dropped, scn)["pass"] is False
    advisory_fail = {
        "criteria": [dict(c, verdict="fail" if c["id"] not in required else "pass") for c in all_pass["criteria"]]
    }
    assert fcjudge.normalize(advisory_fail, scn)["pass"] is True


def test_the_judge_is_not_shown_the_assistants_prompt():
    scn = fcworld.load_scenarios(["qty-vague"])[0]
    prompt = fcjudge.build_user_prompt("R", scn, ["f"], fcjudge.render_transcript([], [_turn()]))
    assert "head orchestrator" not in prompt and "orch_turn" not in prompt


def test_keys_are_read_by_name_and_never_echoed(tmp_path, monkeypatch):
    env = tmp_path / ".env"
    env.write_text("# c\nANTHROPIC_API_KEY=sk-test-abc\nOPENAI_API_KEY='sk-test-def'\nOTHER=1\n")
    monkeypatch.delenv("ANTHROPIC_API_KEY", raising=False)
    keys = fcjudge.load_keys(str(env))
    assert keys["ANTHROPIC_API_KEY"] == "sk-test-abc" and keys["OPENAI_API_KEY"] == "sk-test-def"


# ---- verdict rules ---------------------------------------------------------------
def _judge(passed, failed=(), advisory=()):
    rows = [{"id": i, "required": True, "pass": False, "evidence": ""} for i in failed]
    rows += [{"id": i, "required": False, "pass": False, "evidence": ""} for i in advisory]
    return {"judge": "x/y", "pass": passed, "criteria": rows, "cost_usd": 0.0}


def test_decide_primary_strict_and_agreement():
    both = fcrun.decide([_judge(True), _judge(True)], errored=False)
    assert both["pass"] and both["pass_strict"] and both["judges_agree"] is True
    split = fcrun.decide([_judge(True), _judge(False, ["a"])], errored=False)
    assert split["pass"] and not split["pass_strict"] and split["judges_agree"] is False and split["failed"] == ["a"]
    assert fcrun.decide([_judge(True)], errored=True)["pass"] is False, "an errored run never passes"
    assert fcrun.decide([], errored=False)["pass"] is False
    adv = fcrun.decide([_judge(True, advisory=["t"])], errored=False)
    assert adv["pass"] and adv["advisory_failed"] == ["t"]
    broken = fcrun.decide([{"judge": "x", "error": "boom", "pass": False, "criteria": [], "cost_usd": 0}], False)
    assert broken["pass"] is False


# ---- pricing ---------------------------------------------------------------------
def test_head_cost_prices_provider_usage(tmp_path):
    rec = tmp_path / "rec"
    rec.mkdir()
    row = {
        "ts": "t",
        "host": "api.anthropic.com",
        "model": "claude-sonnet-5-5",
        "usage": {
            "input_tokens": 1000,
            "output_tokens": 100,
            "cache_read_input_tokens": 10000,
            "cache_creation_input_tokens": 2000,
        },
    }
    (rec / "usage.jsonl").write_text(json.dumps(row) + "\n")
    cost = fcrun.head_cost(rec)
    expect = (1000 * 2.0 + 100 * 10.0 + 10000 * 0.2 + 2000 * 2.5) / 1e6
    assert cost["total"] == pytest.approx(expect, abs=1e-6)
    assert list(cost["by"]) == ["anthropic claude-sonnet-5-5"]
    assert fcrun.head_cost(tmp_path / "missing")["total"] == 0.0


def test_judge_cost_uses_the_judge_provider(tmp_path):
    usd = fcrun.judge_cost("anthropic/claude-sonnet-5-5", {"input_tokens": 1000, "output_tokens": 100})
    assert usd == pytest.approx((1000 * 2.0 + 100 * 10.0) / 1e6, abs=1e-6)


# ---- the runner, end to end with fakes -------------------------------------------
FAKE_DRIVER = """#!/usr/bin/env python3
import json, sys
a = sys.argv
run = json.load(open(a[a.index("--scenario") + 1]))
rec = a[a.index("--record") + 1]
open(rec + "/usage.jsonl", "w").write(json.dumps({"ts": "t", "host": "api.anthropic.com", "model": "m-test",
    "usage": {"input_tokens": 10, "output_tokens": 10}}) + "\\n")
turns = [{"owner": t, "reply": "Eat 300 g of pasta and 4 slices.", "ok": True, "tool_calls": [], "tool_results": [],
          "deliveries": ["Eat 300 g of pasta and 4 slices."]} for t in run["turns"]]
json.dump({"turns": turns, "banner": "fake", "host": "h", "model": "m"}, open(a[a.index("--out") + 1], "w"))
"""


def _task(tmp_path, scn_id="qty-vague", rep=1, judges=("a/x", "b/y")):
    return fcrun.Task(
        scn=fcworld.load_scenarios([scn_id])[0],
        rep=rep,
        label="t",
        outdir=tmp_path,
        host="anthropic",
        model="m-test",
        cfg=CFG,
        keys={},
        judges=list(judges),
        retries=0,
    )


@pytest.fixture
def fakes(tmp_path, monkeypatch):
    driver = tmp_path / "fake_driver.py"
    driver.write_text(FAKE_DRIVER)
    driver.chmod(driver.stat().st_mode | stat.S_IXUSR)
    monkeypatch.setattr(fcrun, "DRIVER", driver)
    calls = []

    def fake_judge(spec, keys, user):
        calls.append(spec)
        ids = re.findall(r"^- ([a-z\-]+):", user, re.M)
        verdict = {"criteria": [{"id": i, "verdict": "pass", "evidence": "ok"} for i in ids], "summary": "s"}
        return verdict, {"input_tokens": 100, "output_tokens": 10}

    monkeypatch.setattr(fcjudge, "call_judge", fake_judge)
    monkeypatch.setattr(fcrun, "judge_cost", lambda spec, usage: 0.001)
    return calls


def test_one_run_is_saved_judged_priced_and_resumable(tmp_path, fakes):
    rec = fcrun.execute(_task(tmp_path), fcrun.Budget(1.0))
    assert rec["verdict"]["pass"] and rec["verdict"]["pass_strict"] and rec["verdict"]["judges_agree"] is True
    assert rec["cost_usd"]["head"]["total"] > 0 and rec["cost_usd"]["judges"] == pytest.approx(0.002)
    assert (tmp_path / "qty-vague" / "rep1.json").exists() and (tmp_path / "qty-vague" / "rep1.run.json").exists()
    assert fakes == ["a/x", "b/y"]
    again = fcrun.execute(_task(tmp_path), fcrun.Budget(1.0))
    assert again == rec and fakes == ["a/x", "b/y"], "a saved run is not repeated or re-judged"


def test_an_exhausted_budget_skips_new_runs(tmp_path, fakes):
    budget = fcrun.Budget(0.0)
    assert fcrun.execute(_task(tmp_path), budget) == {"scenario": "qty-vague", "rep": 1, "skipped": "budget"}
    assert fakes == []


def test_a_driver_failure_is_an_errored_run_not_a_pass(tmp_path, fakes, monkeypatch):
    bad = tmp_path / "bad_driver.sh"
    bad.write_text("#!/bin/sh\nexit 3\n")
    bad.chmod(bad.stat().st_mode | stat.S_IXUSR)
    monkeypatch.setattr(fcrun, "DRIVER", bad)
    monkeypatch.setattr(fcrun, "RETRY_WAIT_S", 0)
    rec = fcrun.execute(_task(tmp_path), fcrun.Budget(1.0))
    assert rec["verdict"]["errored"] and not rec["verdict"]["pass"] and rec["error"]


# ---- reports ---------------------------------------------------------------------
def _rec(scn, rep, passed, failed=()):
    verdict = {
        "pass": passed,
        "pass_strict": passed,
        "failed": [] if passed else list(failed),
        "advisory_failed": [],
        "judges_agree": None,
        "errored": False,
    }
    return {
        "scenario": scn,
        "rep": rep,
        "host": "anthropic",
        "model": "m",
        "verdict": verdict,
        "judges": [{"judge": "a/x"}],
        "cost_usd": {"head": {"total": 0.04}, "judges": 0.01},
    }


def test_the_target_is_four_passing_runs_of_five():
    recs = [_rec("qty-vague", i, i <= 4) for i in range(1, 6)] + [
        _rec("plan-stated", i, i <= 3, ["only-stated-foods"]) for i in range(1, 6)
    ]
    summary = fcreport.summarize(recs, "t", CFG)
    assert summary["scenarios"]["qty-vague"]["meets_target"] is True
    assert summary["scenarios"]["plan-stated"]["meets_target"] is False
    assert summary["scenarios"]["plan-stated"]["failed_criteria"] == {"only-stated-foods": 2}
    assert summary["totals"]["scenarios_meeting_target"] == 1
    assert summary["cost_usd"]["total"] == pytest.approx(0.5)
    md = fcreport.render_markdown(summary)
    assert "MISSED" in md and "met" in md
    assert "plan-stated | 3/5" in fcreport.render_comparison(summary, summary).replace("  ", " ")


def test_cli_lists_and_parses(capsys):
    assert run_eval.main(["list"]) == 0
    assert "qty-vague" in capsys.readouterr().out
    args = run_eval.build_parser().parse_args(["run", "--label", "x", "--model", "m", "--reps", "2"])
    assert args.effort == "adapter" and args.backend == "lab" and args.reps == 2


def test_the_second_judge_only_rechecks_a_pass(tmp_path, fakes, monkeypatch):
    def failing(spec, keys, user):
        calls = fakes
        calls.append(spec)
        ids = re.findall(r"^- ([a-z\-]+):", user, re.M)
        verdict = {"criteria": [{"id": i, "verdict": "fail", "evidence": "no"} for i in ids], "summary": "s"}
        return verdict, {"input_tokens": 100, "output_tokens": 10}

    monkeypatch.setattr(fcjudge, "call_judge", failing)
    task = _task(tmp_path)
    task.extra["second_on_pass"] = True
    rec = fcrun.execute(task, fcrun.Budget(1.0))
    assert fakes == ["a/x"], "a failure needs no second opinion"
    assert not rec["verdict"]["pass"] and rec["verdict"]["judges_agree"] is None


def test_a_device_run_is_judged_on_the_history_it_really_had(tmp_path, fakes, monkeypatch):
    live = [{"role": "user", "text": "earlier"}, {"role": "assistant", "text": "the unit's own reply"}]
    ran = {
        "attempts": 1,
        "error": "",
        "cost": {"total": 0.07, "by": {"anthropic m": 0.07}},
        "driver": {"turns": [_turn()], "history": live, "banner": "device unit A"},
    }
    monkeypatch.setattr(fcrun, "run_device", lambda t, files: ran)
    seen = []
    real = fcjudge.render_transcript
    monkeypatch.setattr(fcjudge, "render_transcript", lambda h, t: seen.append(h) or real(h, t))
    task = _task(tmp_path)
    task.backend = "device"
    rec = fcrun.execute(task, fcrun.Budget(1.0))
    assert seen == [live] and rec["history"] == live
    assert rec["cost_usd"]["head"]["total"] == 0.07 and rec["backend"] == "device"


def test_a_stopped_run_unwinds_so_a_device_leg_restores_the_unit():
    import signal

    old = signal.getsignal(signal.SIGINT), signal.getsignal(signal.SIGTERM)
    try:
        signal.signal(signal.SIGINT, signal.SIG_IGN)  # what a background shell job starts with
        run_eval.install_signal_handlers()
        assert signal.getsignal(signal.SIGINT) is signal.default_int_handler
        with pytest.raises(KeyboardInterrupt):
            signal.getsignal(signal.SIGTERM)(signal.SIGTERM, None)
    finally:
        signal.signal(signal.SIGINT, old[0])
        signal.signal(signal.SIGTERM, old[1])


def test_judge_lists_are_a_first_judge_and_comma_separated_second_opinions():
    parse = run_eval.build_parser().parse_args
    base = ["run", "--label", "x", "--model", "m"]
    one = parse(base + ["--judge", "a/x", "--judge2", "none"])
    assert run_eval._judges(one, CFG) == ["a/x"]
    many = parse(base + ["--judge", "a/x", "--judge2", "b/y,c/z"])
    assert run_eval._judges(many, CFG) == ["a/x", "b/y", "c/z"]
    default = parse(base)
    assert run_eval._judges(default, CFG) == [CFG["models"]["judge"], CFG["models"]["judge2"]]
