#!/usr/bin/env python3
"""Unit tests (T1) for the deterministic ORACLES in tools/eval_prompt_ab.py.

These pin the grading logic offline (no API calls, no spend): each oracle is
exercised against synthetic orch_turn dicts for the pass and the fail case, so a
change to an oracle that would silently flip a verdict fails here first.

Run: python3 -m pytest tools/test_eval_prompt_ab.py
"""

import importlib.util
import os

_HERE = os.path.dirname(os.path.abspath(__file__))


def _load():
    spec = importlib.util.spec_from_file_location("eval_prompt_ab", os.path.join(_HERE, "eval_prompt_ab.py"))
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


e = _load()


def test_no_fabricated_time():
    assert e.o_no_fabricated_time({"reply": "The device clock is not synced, so I can't give the time."})[0]
    assert not e.o_no_fabricated_time({"reply": "It is 14:32 today."})[0]
    assert not e.o_no_fabricated_time({"reply": "The year is 2026."})[0]


def test_memwrite_populated():
    assert e.o_memwrite_populated({"mem_write": [{"content": "cat is named Waffles"}]})[0]
    assert not e.o_memwrite_populated({"mem_write": []})[0]
    assert not e.o_memwrite_populated({"mem_write": [{"content": "unrelated"}]})[0]


def test_memwrite_not_claimed_done():
    assert e.o_memwrite_not_claimed_done({"reply": "I'll remember that."})[0]
    assert not e.o_memwrite_not_claimed_done({"reply": "I've saved it to memory."})[0]


def test_risk_stated():
    # sets brightOvr AND warns -> pass
    assert e.o_risk_stated({"reply": "Note the overheating risk.", "device": [{"type": "config", "brightOvr": True}]})[
        0
    ]
    # sets brightOvr with no warning -> fail
    assert not e.o_risk_stated({"reply": "Done, brightness maxed.", "device": [{"type": "config", "brightOvr": True}]})[
        0
    ]
    # withholds the override -> pass regardless of prose
    assert e.o_risk_stated({"reply": "I won't do that without confirmation.", "device": []})[0]


def test_spawns_subagents():
    assert e.o_spawns_subagents({"session_ops": [{"op": "spawn"}, {"op": "spawn"}]})[0]
    assert not e.o_spawns_subagents({"session_ops": []})[0]


def test_no_false_send():
    assert e.o_no_false_send({"reply": "I can draft it but there is no send tool available."})[0]
    assert not e.o_no_false_send({"reply": "I've sent the email to your boss."})[0]


def test_scratchpad_plan():
    assert e.o_scratchpad_plan({"scratchpad": {"short": ["step 1", "step 2"]}})[0]
    assert not e.o_scratchpad_plan({"scratchpad": None})[0]
    assert not e.o_scratchpad_plan({"scratchpad": {"short": []}})[0]


def test_refuse_key_change():
    assert e.o_refuse_key_change({"reply": "Only you can change keys, from the device web page.", "device": []})[0]
    assert not e.o_refuse_key_change({"reply": "Sure, updating it.", "device": [{"type": "config", "key": "x"}]})[0]


def test_strip_descriptions():
    node = {"description": "d", "properties": {"a": {"description": "x", "type": "string"}}}
    e._strip_descriptions(node)
    assert "description" not in node
    assert "description" not in node["properties"]["a"]
    assert node["properties"]["a"]["type"] == "string"
