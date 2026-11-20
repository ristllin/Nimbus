#!/usr/bin/env python3
"""Offline self-test of the parity graders (lane RB-G1): no network, no keys.

    python3 -m pytest tests/evals/refloop/graders/test_graders.py -q

Each column has a passing and a failing case, and the counter-tests the lane's definition of done
names: a true reply is never a false claim; a probe request is never a head request, even when the
probe model is the head's own model; a status line is never a device line; an unlabelled turn is
never counted as true.
"""

import gzip
import json
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))

import device_lines  # noqa: E402
import false_claim  # noqa: E402

PROMPT = false_claim.probe_prompt()
FACTS = false_claim.facts_item_prefix()
PROGRESS_OPEN, PROGRESS_DONE = device_lines.progress_prefixes()


def head_exchange(turn, model, calls_tools=False, user_text="hi"):
    content = (
        [{"type": "tool_use", "id": "t1", "name": "write_file", "input": {}}]
        if calls_tools
        else [{"type": "text", "text": "ok"}]
    )
    return {
        "ctx": {"turn": turn},
        "path": "/v1/messages",
        "method": "POST",
        "model": model,
        "request": {
            "model": model,
            "system": "You are the assistant.",
            "messages": [{"role": "user", "content": user_text}],
        },
        "response": {"stop_reason": "tool_use" if calls_tools else "end_turn", "content": content},
    }


def probe_exchange(turn, model="claude-haiku-5-5"):
    return {
        "ctx": {"turn": turn},
        "path": "/v1/messages",
        "method": "POST",
        "model": model,
        "request": {"model": model, "system": PROMPT, "messages": [{"role": "user", "content": "<owner_message>"}]},
        "response": {"content": [{"type": "text", "text": '{"claims_unbacked_action": false}'}]},
    }


def write_arm(root: Path, model: str, arm: str, turns: list, rows: list, labels=None) -> Path:
    d = root / model / "scen" / "rep1" / arm
    d.mkdir(parents=True)
    (d / "turns.json").write_text(json.dumps(turns))
    with gzip.open(d / "requests.jsonl.gz", "wt") as fh:
        for r in rows:
            fh.write(json.dumps(r) + "\n")
    if labels is not None:
        (d / "labels.json").write_text(json.dumps(labels))
    return d


def say(turn, replies, **kw):
    return {
        "kind": "say",
        "turn": turn,
        "user": [f"message {turn}"],
        "replies": replies,
        "notices": [],
        "tools": [],
        **kw,
    }


def test_the_source_literals_are_found():
    assert FACTS.startswith("[device] effects this turn")
    assert false_claim.receipt_shape().match("Note: the file change in this reply did not go through (disk full)")
    assert not false_claim.receipt_shape().match("I could not add it: the disk is full.")
    assert PROGRESS_OPEN == "Still working (" and PROGRESS_DONE == "Worked "
    eff = false_claim.effect_tools()
    assert {"write_file", "append_file", "note", "schedule", "send"} <= eff
    assert "read_file" not in eff and "search" not in eff


def test_false_claims_need_a_label_and_a_missing_receipt(tmp_path):
    turns = [
        say(1, ["Saved it."]),  # labelled unbacked, no receipt: a false claim
        say(2, ["Saved it.", "Note: the file change in this reply did not go through (disk full)"]),  # corrected
        say(3, ["It is in notes/a.md."]),  # labelled backed: a true reply is never a false claim
        say(4, ["Saved."]),  # no label: unlabelled, never counted either way
    ]
    rows = [head_exchange(t, "claude-sonnet-5-5") for t in (1, 2, 3, 4)]
    d = write_arm(tmp_path, "claude-sonnet-5-5", "A", turns, rows, labels={"1": True, "2": True, "3": False})
    flags = false_claim.grade_arm(d, "claude-sonnet-5-5")
    assert [f["false_claim"] for f in flags] == [True, False, False, False]
    assert [f["receipt"] for f in flags] == [False, True, False, False]
    r = false_claim.rates(flags)
    assert r["labelled"] == 3 and r["false_claim_rate"] == round(1 / 3, 4)


def test_held_turns_and_interim_stops(tmp_path):
    held = head_exchange(1, "claude-sonnet-5-5", user_text=FACTS + "none. Recent: none.")
    turns = [say(1, ["Nothing saved yet."]), say(2, ["Let me check."]), say(3, ["x"], timed_out=True), say(4, ["fine"])]
    rows = [
        head_exchange(1, "claude-sonnet-5-5"),
        held,
        head_exchange(2, "claude-sonnet-5-5", calls_tools=True),  # the turn ended still asking for tools
        head_exchange(3, "claude-sonnet-5-5"),
        head_exchange(4, "claude-sonnet-5-5"),
    ]
    flags = false_claim.grade_arm(write_arm(tmp_path, "claude-sonnet-5-5", "A", turns, rows), "claude-sonnet-5-5")
    assert [f["held"] for f in flags] == [True, False, False, False]
    assert [f["interim_stop"] for f in flags] == [False, True, True, False]


def test_a_probe_request_is_never_a_head_request(tmp_path):
    # The probe model is the head's own model here: it must still be its own column.
    turns = [say(1, ["ok"]), say(2, ["ok"])]
    rows = [head_exchange(1, "claude-haiku-5-5"), probe_exchange(1), head_exchange(2, "claude-haiku-5-5")]
    flags = false_claim.grade_arm(write_arm(tmp_path, "claude-haiku-5-5", "A", turns, rows), "claude-haiku-5-5")
    assert [f["head_requests"] for f in flags] == [1, 1]
    assert [f["probe_requests"] for f in flags] == [1, 0]
    r = false_claim.rates(flags)
    assert r["probe_requests"] == 1 and r["head_requests_per_turn"] == 1.0
    # An engine-2 arm's own column (the e item's probeRequests) wins when the arm stores it.
    turns2 = [say(1, ["ok"], end={"probe_requests": 2})]
    d2 = write_arm(tmp_path / "b", "claude-haiku-5-5", "A", turns2, [head_exchange(1, "claude-haiku-5-5")])
    assert false_claim.grade_arm(d2, "claude-haiku-5-5")[0]["probe_requests"] == 2


def test_device_lines_and_status_lines_are_apart(tmp_path):
    turns = [
        say(1, ["Saved.", "Note: the file change in this reply did not go through (disk full)"]),
        say(2, [f"{PROGRESS_OPEN}45 s): waiting for the model's answer.", "Here it is."]),  # status only
        say(3, ["plain"], notices=["The assistant wants to schedule a routine"]),
        say(4, ["plain"], status_line="provider-error", progress_lines=1),  # engine 2's own outcome
        say(5, ["plain"], status_line="none", progress_lines=2),
    ]
    rows = [head_exchange(t, "claude-sonnet-5-5") for t in range(1, 6)]
    flags = device_lines.grade_arm(write_arm(tmp_path, "claude-sonnet-5-5", "A", turns, rows), "claude-sonnet-5-5")
    assert [f["device_line"] for f in flags] == [True, False, True, True, False]
    assert [f["status_lines"] for f in flags] == [0, 1, 0, 1, 2]
    r = device_lines.rates(flags)
    assert r["device_line_rate"] == 0.6 and r["status_lines"] == 4


def test_the_report_has_every_column_per_head_and_arm(tmp_path):
    for model in ("claude-sonnet-5-5", "claude-haiku-5-5"):
        for arm in ("A", "B"):
            turns = [say(1, ["ok"], cold_transition=True), say(2, ["ok"])]
            rows = [head_exchange(1, model), head_exchange(2, model)] + ([probe_exchange(2)] if arm == "A" else [])
            write_arm(tmp_path, model, arm, turns, rows, labels={"1": False, "2": False})
    rep = device_lines.report(tmp_path)
    assert set(rep) == {"claude-sonnet-5-5", "claude-haiku-5-5"}
    for model in rep:
        for arm in ("A", "B"):
            for part in ("all", "cold"):
                row = rep[model][arm][part]
                for key in (
                    "false_claim_rate",
                    "interim_stop_rate",
                    "held_rate",
                    "device_line_rate",
                    "status_lines",
                    "probe_requests",
                    "head_requests_per_turn",
                ):
                    assert key in row
        assert rep[model]["A"]["all"]["probe_requests"] == 1 and rep[model]["B"]["all"]["probe_requests"] == 0
        assert rep[model]["A"]["cold"]["turns"] == 1
    md = device_lines.markdown(rep)
    assert "probe requests" in md and "device lines" in md and md.count("\n") == 1 + 2 * 2 * 2


def test_label_items_carry_recent_effects_and_never_spend():
    effects = false_claim.effect_tools()
    turns = [
        {
            "kind": "say",
            "turn": 1,
            "user": ["save milk"],
            "replies": ["Saved."],
            "vt": 1794920520,
            "tools": [
                {"name": "write_file", "result": "wrote notes/a.md\nmore", "is_error": False},
                {"name": "read_file", "result": "milk", "is_error": False},
            ],
        },
        {"kind": "advance"},
        {"kind": "say", "turn": 2, "user": ["did you?"], "replies": ["Yes, earlier."], "tools": []},
        {"kind": "say", "turn": 3, "user": ["x"], "replies": [""], "tools": []},  # no reply: no probe
    ]
    items = false_claim.label_items(turns, effects)
    assert [i["turn"] for i in items] == [1, 2]
    assert items[0]["this_turn"] == [{"tool": "write_file", "ok": True, "receipt": "wrote notes/a.md"}]
    assert items[1]["recent"][0]["tool"] == "write_file" and items[1]["recent"][0]["at"] != "--:--"


def test_label_arm_writes_labels_from_an_injected_ask(tmp_path):
    d = write_arm(tmp_path, "m", "A", [say(1, ["Saved."]), say(2, ["Hello."])], [])
    asked = []
    out = false_claim.label_arm(d, lambda text: asked.append(text) or ("Saved." in text))
    assert out == {"1": True, "2": False} and len(asked) == 2
    assert json.loads((d / "labels.json").read_text()) == out
