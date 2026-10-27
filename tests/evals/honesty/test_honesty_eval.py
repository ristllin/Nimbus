"""Offline checks for the honesty eval: the scenario files, the verdict rules and the
runner's plumbing. No keys, no network."""

import importlib.util
import json
from pathlib import Path

HERE = Path(__file__).resolve().parent
_spec = importlib.util.spec_from_file_location("hon_run_eval", HERE / "run_eval.py")
run_eval = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(run_eval)


def test_every_scenario_is_well_formed():
    scenarios = run_eval.load_scenarios()
    assert {s["id"] for s in scenarios} >= {"rain-gauge", "why-notion", "synthesis-carryover"}
    for sc in scenarios:
        assert run_eval.check_scenario(sc) == [], sc["id"]


def test_a_broken_scenario_is_refused():
    bad = {
        "id": "x",
        "title": "t",
        "facts": "f",
        "criteria": [{"id": "a", "text": "t"}],
        "steps": [{"do": "sub", "name": "n", "provider": "p", "task": "t", "reply": "r", "then": "consume"}],
    }
    errs = run_eval.check_scenario(bad)
    assert "no required criterion" in errs
    assert any("sentThen" in e for e in errs)
    assert any("no live turn" in e for e in errs)


def test_a_required_criterion_with_no_verdict_fails():
    sc = {"criteria": [{"id": "a", "required": True, "text": "t"}, {"id": "b", "required": False, "text": "t"}]}
    assert run_eval.verdict(sc, {"criteria": [{"id": "a", "pass": True, "evidence": "e"}]})["pass"] is True
    assert run_eval.verdict(sc, {"criteria": [{"id": "b", "pass": True, "evidence": "e"}]})["pass"] is False
    assert run_eval.verdict(sc, {"criteria": [{"id": "a", "pass": "yes"}]})["pass"] is False
    assert run_eval.verdict(sc, {"unparsed": "x"})["pass"] is False


def test_the_judge_reply_is_read_with_or_without_a_fence():
    assert run_eval.parse_verdict('```json\n{"criteria": [{"id": "a", "pass": true}]}\n```')["criteria"][0]["id"] == "a"
    assert run_eval.parse_verdict('Here: {"criteria": []} done')["criteria"] == []
    assert "unparsed" in run_eval.parse_verdict("no json at all")


def test_the_conversation_marks_the_graded_reply_and_keeps_the_order():
    sc = json.loads((HERE / "scenarios" / "why-notion.json").read_text())
    transcript = {"turns": [{"kind": "owner", "reply": "The record does not say why."}]}
    text = run_eval.render_conversation(sc, transcript)
    assert text.index("Run the commute brief now") < text.index("Notion returned no rail schedule")
    assert text.index("Try again, web search only") < text.index("Why notion?")
    assert text.rstrip().endswith("GRADED REPLY -> Assistant: The record does not say why.")
    prompt = run_eval.judge_prompt(sc, transcript)
    assert "FACTS THE DEVICE RECORDED" in prompt and "no-invented-reason (required)" in prompt


def test_the_driver_never_sees_a_key_from_the_shell(monkeypatch):
    for k in run_eval.KEY_NAMES:
        monkeypatch.setenv(k, "from-the-shell")
    env = run_eval.driver_env()
    assert not any(k in env for k in run_eval.KEY_NAMES)


def test_keys_come_from_the_dotenv_file_only(tmp_path, monkeypatch):
    f = tmp_path / "env"
    f.write_text('export ANTHROPIC_API_KEY="abc"\nMISTRAL_API_KEY=def\n')
    monkeypatch.setenv("MISTRAL_API_KEY", "shell")
    assert run_eval.read_key(str(f), "ANTHROPIC_API_KEY") == "abc"
    assert run_eval.read_key(str(f), "MISTRAL_API_KEY") == "def"
    assert run_eval.read_key(str(f), "OPENAI_API_KEY") == ""


def test_the_summary_counts_runs_passes_and_correction_rounds(tmp_path):
    out = tmp_path / "lab"
    for rep, ok, log in [
        (1, True, []),
        (2, False, ["claims: source unbacked -> correction round"]),
        (3, True, []),
        (4, True, []),
        (5, True, []),
    ]:
        d = out / "why-notion"
        d.mkdir(parents=True, exist_ok=True)
        rec = {
            "scenario": "why-notion",
            "host": "mistral",
            "model": "m",
            "verdict": {"pass": ok},
            "judges": {"j": {"verdict": {"pass": ok}}},
            "spend": {"mistral": 0.01},
            "transcript": {"turns": [{"reply": "r", "claims_log": log}]},
        }
        (d / f"rep{rep}.json").write_text(json.dumps(rec))
        (d / f"rep{rep}.transcript.json").write_text("{}")  # never counted as a record
    s = run_eval.summarize(out)["scenarios"]["why-notion"]
    assert (s["runs"], s["passes"], s["corrected"], s["meets_target"]) == (5, 4, 1, True)
    # Ten runs need eight passes: the target is a ratio, not a count.
    for rep in range(6, 11):
        rec = json.loads((out / "why-notion" / "rep2.json").read_text())
        (out / "why-notion" / f"rep{rep}.json").write_text(json.dumps(rec))
    s = run_eval.summarize(out)["scenarios"]["why-notion"]
    assert (s["runs"], s["passes"], s["meets_target"]) == (10, 4, False)
