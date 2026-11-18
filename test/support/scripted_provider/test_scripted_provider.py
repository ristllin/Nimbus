#!/usr/bin/env python3
"""Offline test of the scripted provider's Python twin: no network, no keys.

python3 test/support/scripted_provider/test_scripted_provider.py
"""

import json
import sys
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))

from scripted_provider import ScriptedModel, ScriptExhausted, call_id  # noqa: E402  (puts tools/refloop on the path)

import refloop  # noqa: E402

EXAMPLE = HERE / "example_script.json"


class Tools:
    def __init__(self):
        self.calls = []

    def call(self, name, args):
        self.calls.append((name, args))
        return "Boat: Nereid II", False


class ScriptedModelTest(unittest.TestCase):
    def conv(self):
        return refloop.Conversation([{"name": "read_file", "description": "read", "schema": {"type": "object"}}])

    def test_the_shared_script_drives_the_reference_loop(self):
        model, tools, conv = ScriptedModel.from_file(EXAMPLE), Tools(), self.conv()
        reply = refloop.run_turn(conv, model, tools, ["where is my boat?"])
        self.assertEqual(reply, "Your boat is the Nereid II.")
        self.assertEqual(tools.calls, [("read_file", {"path": "notes/boat.md"})])
        self.assertEqual(len(model.seen), 2)
        # The second request carries the first round's call and its result, paired by id.
        last = model.seen[1]
        self.assertEqual(last[1]["content"][1]["id"], call_id(0, 0))
        self.assertEqual(last[2]["content"][0]["tool_use_id"], call_id(0, 0))
        # The first request is a prefix of the second (append only).
        self.assertEqual(model.seen[0], last[: len(model.seen[0])])
        self.assertEqual(conv.prompt_tokens, 60 + 2100)

    def test_running_out_of_script_raises(self):
        model = ScriptedModel([])
        with self.assertRaises(ScriptExhausted):
            refloop.run_turn(self.conv(), model, Tools(), ["hi"])

    def test_cut_and_other_dialects(self):
        model = ScriptedModel.from_script({"steps": [{"text": "half", "finish": "cut"}]})
        self.assertIn("(stopped early: max_tokens)", refloop.run_turn(self.conv(), model, Tools(), ["hi"]))
        with self.assertRaises(ValueError):
            ScriptedModel.from_script({"dialect": "chat", "steps": []})

    def test_unreplayable_or_malformed_steps_are_refused(self):
        for bad in ({"status": 503}, {"transport_cut": True}, {"calls": [{"args": {}}]}, {"cals": []}):
            with self.assertRaises(ValueError):
                ScriptedModel([bad])
        ScriptedModel([{"text": "ok", "status": 200}])

    def test_call_ids_match_the_cpp_provider(self):
        self.assertEqual(call_id(0, 0), "toolu_s000c00")
        self.assertEqual(call_id(12, 3), "toolu_s012c03")
        self.assertIn("steps", json.loads(EXAMPLE.read_text()))


if __name__ == "__main__":
    unittest.main()
