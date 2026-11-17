#!/usr/bin/env python3
"""Offline self-test of the reference-loop rig: no network, no keys, no nimbusd.

  python3 tests/evals/refloop/test_refloop_offline.py

Covers the loop itself (with a scripted provider), the transcript save and reload, the
summary of the oldest half, the burst channel, the scenario files, every check of the
grader (one passing and one failing case each), the wire parsing of both providers,
the usage normalization and the price table.
"""

import json
import queue
import sys
import tempfile
import threading
import unittest
from pathlib import Path

REPO = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(REPO / "tools" / "refloop"))

import arms  # noqa: E402
import costs  # noqa: E402
import fixtures  # noqa: E402
import grade  # noqa: E402
import mechanisms  # noqa: E402
import refloop  # noqa: E402
import scenario  # noqa: E402
from capture_proxy import normalize_usage  # noqa: E402

SCENARIOS = REPO / "tests" / "evals" / "refloop" / "scenarios"


class FakeTools:
    def __init__(self):
        self.calls = []

    def specs(self):
        return [{"name": "files_read", "description": "read", "schema": {"type": "object", "properties": {}}}]

    def call(self, name, args):
        self.calls.append((name, args))
        return f"result of {name}", False


class ScriptedProvider:
    """Answers from a script: each entry is (tool calls, text). Messages use the Anthropic shapes."""

    is_owner = staticmethod(refloop.Anthropic.is_owner)
    without_thinking = staticmethod(refloop.Anthropic.without_thinking)
    user = refloop.Anthropic.user
    results = refloop.Anthropic.results

    def __init__(self, script, prompt_tokens=100):
        self.script, self.prompt_tokens, self.seen = list(script), prompt_tokens, []

    def step(self, conv, messages=None):
        self.seen.append(json.loads(json.dumps(conv.messages if messages is None else messages)))
        calls, text = self.script.pop(0)
        content = [{"type": "thinking", "thinking": "", "signature": "sig"}]
        content += [{"type": "text", "text": text}] if text else []
        content += [{"type": "tool_use", "id": cid, "name": name, "input": args} for cid, name, args in calls]
        return refloop.Step([{"role": "assistant", "content": content}], calls, text, self.prompt_tokens)


class LoopTest(unittest.TestCase):
    def test_loops_until_no_tool_call_and_appends_only(self):
        tools = FakeTools()
        provider = ScriptedProvider(
            [([("c1", "files_read", {"name": "a"})], ""), ([("c2", "files_read", {"name": "b"})], ""), ([], "done")]
        )
        conv = refloop.Conversation(tools.specs())
        self.assertEqual(refloop.run_turn(conv, provider, tools, ["read both"]), "done")
        self.assertEqual([c[1]["name"] for c in tools.calls], ["a", "b"])
        roles = [m["role"] for m in conv.messages]
        self.assertEqual(roles, ["user", "assistant", "user", "assistant", "user", "assistant"])
        # Append only: every request's messages are a prefix of the next request's.
        for earlier, later in zip(provider.seen, provider.seen[1:]):
            self.assertEqual(later[: len(earlier)], earlier)
        # The tool result rides as its own message, tied to the call.
        self.assertEqual(conv.messages[2]["content"][0]["tool_use_id"], "c1")
        self.assertIn("read both", conv.messages[0]["content"][-1]["text"])

    def test_system_prompt_is_small_and_frozen(self):
        conv = refloop.Conversation([])
        self.assertLess(len(conv.system.encode()), 1500)
        before = conv.system
        refloop.run_turn(conv, ScriptedProvider([([], "hi")]), FakeTools(), ["hello"])
        self.assertEqual(conv.system, before)

    def test_round_cap_stops_a_runaway_turn(self):
        provider = ScriptedProvider([([("c", "files_read", {})], "")] * (refloop.MAX_ROUNDS + 1))
        reply = refloop.run_turn(refloop.Conversation([]), provider, FakeTools(), ["loop forever"])
        self.assertIn("too many tool rounds", reply)

    def test_transcript_survives_a_reload(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "t.json"
            conv = refloop.Conversation(FakeTools().specs(), path)
            refloop.run_turn(
                conv, ScriptedProvider([([("c1", "files_read", {})], ""), ([], "first")]), FakeTools(), ["one"]
            )
            again = refloop.Conversation.load(path)
            self.assertEqual(again.messages, conv.messages)
            self.assertEqual(again.system, conv.system)
            self.assertEqual(again.specs, conv.specs)
            provider = ScriptedProvider([([], "second")])
            refloop.run_turn(again, provider, FakeTools(), ["two"])
            self.assertEqual(provider.seen[0][: len(conv.messages)], conv.messages)

    def test_summary_replaces_only_the_oldest_half(self):
        script = [([], f"answer {n}") for n in range(4)] + [([], "SUMMARY TEXT")]
        provider = ScriptedProvider(script, prompt_tokens=10)
        conv = refloop.Conversation([])
        for n in range(3):
            refloop.run_turn(conv, provider, FakeTools(), [f"question {n}"])
        self.assertEqual(len(conv.messages), 6)  # nothing summarized under the threshold
        provider.prompt_tokens = refloop.SUMMARIZE_AT + 1
        refloop.run_turn(conv, provider, FakeTools(), ["question 3"])
        texts = [b.get("text", "") for m in conv.messages for b in m["content"]]
        self.assertIn("SUMMARY TEXT", texts[0])
        self.assertFalse(any("question 0" in t or "question 1" in t for t in texts))
        self.assertTrue(any("question 2" in t for t in texts) and any("question 3" in t for t in texts))
        # A thinking block is bound to the history before it, so none survives in the kept half.
        self.assertFalse(any(b.get("type") == "thinking" for m in conv.messages for b in m["content"]))
        # The summary request saw the oldest half plus the ask, never the kept half.
        self.assertFalse(any("question 2" in b.get("text", "") for m in provider.seen[-1] for b in m["content"]))

    def test_burst_messages_that_arrive_mid_turn_ride_the_next_turn_together(self):
        started, gate = threading.Event(), threading.Event()

        class SlowFirst(ScriptedProvider):
            def step(self, conv, messages=None):
                if not started.is_set():  # the first turn holds here until the burst has arrived
                    started.set()
                    gate.wait(5)
                return super().step(conv, messages)

        provider = SlowFirst([([], "reply one"), ([], "reply two")])
        conv, inbox, replies = refloop.Conversation([]), queue.Queue(), []
        worker = threading.Thread(target=refloop.serve, args=(conv, provider, FakeTools(), inbox, replies.append))
        worker.start()
        inbox.put("first")
        self.assertTrue(started.wait(5))
        inbox.put("second")
        inbox.put("third")
        inbox.put(None)
        gate.set()
        worker.join(5)
        self.assertEqual(replies, ["reply one", "reply two"])
        second_request = [b.get("text", "") for m in provider.seen[1] for b in m["content"]]
        self.assertTrue(any("second" in t for t in second_request) and any("third" in t for t in second_request))

    def test_mcp_tool_names_lose_their_dot_and_map_back(self):
        tools = refloop.McpTools("http://127.0.0.1:1", "t")
        listing = {
            "tools": [
                {"name": "files.read", "description": "d", "inputSchema": {"type": "object", "properties": {}}},
                {"name": "device.status", "description": "d", "inputSchema": {}},
                {"name": "image.look", "description": "d", "inputSchema": {}},
            ]
        }
        sent = []
        tools._rpc = lambda method, params: (
            sent.append((method, params))
            or (listing if method == "tools/list" else {"content": [{"type": "text", "text": "ok"}], "isError": False})
        )
        specs = tools.specs()
        self.assertEqual([s["name"] for s in specs], ["files_read", "device_status"])  # image.look is not offered
        self.assertEqual(specs[1]["schema"], {"type": "object", "properties": {}})
        self.assertEqual(tools.call("files_read", {"name": "x"}), ("ok", False))
        self.assertEqual(sent[-1], ("tools/call", {"name": "files.read", "arguments": {"name": "x"}}))


class ScenarioTest(unittest.TestCase):
    def test_every_scenario_loads_and_names_real_fixtures(self):
        scenarios = scenario.load_all([SCENARIOS])
        self.assertGreaterEqual(len(scenarios), 14)
        for sc in scenarios:
            for step in sc["turns"]:
                for name in step.get("photos", []):
                    self.assertIn(name, fixtures.PHOTOS, sc["id"])
            for f in (sc.get("seed") or {}).get("files", []):
                if "fixture" in f:
                    self.assertIn(f["fixture"], fixtures.TEXTS, sc["id"])
            for check in [c for s in sc["turns"] for c in s.get("expect") or []] + (sc.get("final") or []):
                self.assertIn(next(iter(check)), grade.CHECKS, sc["id"])

    def test_the_brief_shapes_are_all_there(self):
        by_id = {s["id"]: s for s in scenario.load_all([SCENARIOS])}
        self.assertEqual(len(scenario.user_turns(by_id["coherence-30"])), 30)
        self.assertEqual(len(by_id["album-four"]["turns"][0]["photos"]), 4)
        self.assertTrue(any("restart" in s for s in by_id["restart-midway"]["turns"]))
        self.assertEqual(len(by_id["burst-three"]["turns"][0]["burst"]), 3)
        steps = by_id["memory-20-turns"]["turns"]
        cut = next(i for i, s in enumerate(steps) if "new_conversation" in s)
        self.assertGreaterEqual(len(steps) - cut - 1 - 2, 20)  # twenty turns before the two that need the memory
        self.assertGreater(len(fixtures.handbook().encode()), 39_000)

    def test_the_long_haul_scenario_matches_its_fixtures(self):
        sc = {s["id"]: s for s in scenario.load_all([SCENARIOS])}["long-haul"]
        self.assertEqual(len((sc["seed"] or {})["files"]), fixtures.VOLUMES)
        for i in range(1, fixtures.VOLUMES + 1):
            site, code, balance = fixtures.volume_facts(i)
            body = fixtures.volume(i)
            self.assertIn(code, body[:4000])  # the gate code is near the top
            self.assertTrue(body.rstrip().endswith(f"{balance} pounds."))
            self.assertEqual(sc["turns"][i - 1]["expect"], [{"reply_regex": code}])
            self.assertIn(f"gate code {code}, balance {balance} pounds", sc["judge_notes"])
        self.assertEqual(len(sc["turns"][-1]["expect"]), fixtures.VOLUMES)

    def test_the_long_chat_scenario_matches_its_fixtures(self):
        sc = {s["id"]: s for s in scenario.load_all([SCENARIOS])}["long-chat"]
        entries = [s["say"] for s in sc["turns"][: fixtures.JOURNAL_DAYS]]
        self.assertEqual(entries, [fixtures.journal(d) for d in range(1, fixtures.JOURNAL_DAYS + 1)])
        self.assertGreater(sum(len(e) for e in entries), 48 * 1024)  # past the harness's fold trigger
        self.assertLess(max(len(e) for e in entries), 6144)  # each under its history row clip
        day4, day16, day9 = fixtures.journal_facts(4), fixtures.journal_facts(16), fixtures.journal_facts(9)
        self.assertIn(f"day 4 cost {day4['total']} pounds in total and day 16 cost {day16['total']}", sc["judge_notes"])
        self.assertIn({"reply_regex": day9["met"]}, sc["turns"][23]["expect"])
        self.assertIn("23F", fixtures.journal(1))
        self.assertIn("14 pounds 50", fixtures.journal(2))

    def test_a_typo_never_loads(self):
        good = {"id": "x", "title": "t", "judge_notes": "n", "turns": [{"say": "hi"}]}
        scenario.validate(dict(good))
        for bad in (
            {**good, "turnz": []},
            {**good, "turns": [{"say": "hi", "expect": [{"reply_regx": "a"}]}]},
            {**good, "turns": [{"say": "a", "burst": ["b", "c"]}]},
            {**good, "final": [{"reply_regex": "a"}]},
            {**good, "turns": [{"restart": True, "expect": [{"reply_regex": "a"}]}]},
        ):
            with self.assertRaises(scenario.ScenarioError):
                scenario.validate(bad)


STATE = {
    "files": {"trips/list.md": "# Lisbon 4-7 March\n- passport\n- oat milk 1 litre"},
    "loops": [{"id": "l1", "at": "08:15"}],
    "memories": ["Dentist appointment is on 24 December at 10:00"],
    "rules": "",
    "scratchpad": "",
}
STEP = {
    "reply": "It was 560 against 612, so 52 kcal off.",
    "replies": ["first idea", "chickpea and spinach curry"],
    "tools": [{"name": "files.read"}, {"name": "artifact_save"}],
}
EARLIER = [{"reply": "About 560 kcal for 180 g of chicken."}, STEP]
# check -> (argument that passes, argument that fails) against STEP / STATE / EARLIER.
CASES = {
    "reply_regex": ("52 kcal", "99 kcal"),
    "reply_not_regex": ("99 kcal", "52 kcal"),
    "last_reply_regex": ("chickpea", "first idea"),
    "max_replies": (2, 1),
    "difference_from_turn": ({"turn": 1, "truth": 612}, {"turn": 1, "truth": 700}),
    "tool_called": (["files_read"], ["files_delete"]),
    "tool_not_called": (["files_delete"], ["artifact_save"]),
    "min_tool_calls": (2, 3),
    "file_matching": (
        {"all": ["passport", "Lisbon"], "none": ["sunglasses"]},
        {"all": ["passport"], "none": ["oat milk"]},
    ),
    "file_contains": (
        {"project": "trips", "name": "list.md", "regex": "oat milk"},
        {"project": "trips", "name": "list.md", "regex": "bread"},
    ),
    "file_absent": ({"project": "trips", "name": "gone.md"}, {"project": "trips", "name": "list.md"}),
    "files_at_most": (1, 0),
    "loops_count": (1, 0),
    "loops_regex": ("08:15", "07:30"),
    "loops_not_regex": ("07:30", "08:15"),
    "memory_regex": ("24 December", "14 December"),
    "memory_not_regex": ("14 December", "24 December"),
}


class GradeTest(unittest.TestCase):
    def test_every_check_has_a_passing_and_a_failing_case(self):
        self.assertEqual(set(CASES), set(grade.CHECKS))
        self.assertEqual(set(CASES), scenario.CHECK_NAMES)
        for name, (good, bad) in CASES.items():
            self.assertTrue(grade.CHECKS[name](good, STEP, STATE, EARLIER)[0], name)
            self.assertFalse(grade.CHECKS[name](bad, STEP, STATE, EARLIER)[0], name)

    def test_a_difference_said_in_words_counts(self):
        turns = [
            {"reply": "About 210 miles by road."},
            {"reply": "Nine miles. My guess was 210 and the planner said 219."},
        ]
        self.assertTrue(grade.CHECKS["difference_from_turn"]({"turn": 1, "truth": 219}, turns[1], {}, turns)[0])
        wrong = {"reply": "Four miles. My guess was 215."}
        self.assertFalse(
            grade.CHECKS["difference_from_turn"]({"turn": 1, "truth": 219}, wrong, {}, [turns[0], wrong])[0]
        )

    def test_a_turn_that_never_ran_fails_its_checks(self):
        sc = {
            "turns": [
                {"say": "a", "expect": [{"reply_regex": "x"}]},
                {"say": "b", "expect": [{"reply_not_regex": "x"}]},
            ],
            "final": [{"loops_count": 0}],
        }
        rows = grade.run_checks(sc, {"steps": [{"kind": "say", "turn": 1, "reply": "x"}]})
        self.assertEqual([r["pass"] for r in rows], [True, False, False])

    def test_outcome_band_and_composite(self):
        full = {k: 5 for k in grade.RUBRIC}
        self.assertEqual(grade.composite(1.0, full), 1.0)
        self.assertEqual(grade.composite(0.5, None), 0.5)
        self.assertEqual(grade.outcome(0.80, 0.84), "tie")
        self.assertEqual(grade.outcome(0.80, 0.90), "loss")
        self.assertEqual(grade.outcome(0.90, 0.80), "win")

    def test_two_judge_passes_are_averaged_and_a_disagreement_is_a_split(self):
        def one(order, a, b, better):
            scores = lambda n: {**{k: n for k in grade.RUBRIC}, "notes": f"n{n}"}  # noqa: E731
            return {
                "A": scores(a),
                "B": scores(b),
                "better": better,
                "why": "w",
                "order": order,
                "judge_model": "m",
                "cost_usd": 0.01,
            }

        merged = grade.merge_passes([one("A=X", 3, 5, "B"), one("A=Y", 4, 5, "B")])
        self.assertEqual((merged["A"]["correct"], merged["B"]["correct"], merged["better"]), (3.5, 5.0, "B"))
        self.assertEqual((merged["passes"], merged["cost_usd"], merged["order"]), (2, 0.02, "A=X+A=Y"))
        self.assertEqual(grade.merge_passes([one("A=X", 3, 5, "B"), one("A=Y", 5, 4, "A")])["better"], "split")
        self.assertIsNone(grade.merge_passes([]))
        self.assertEqual(grade._stored_passes({"judge": one("A=X", 3, 5, "B")}), [one("A=X", 3, 5, "B")])
        self.assertEqual(grade._stored_passes({"judge": merged, "judge_passes": [1, 2]}), [1, 2])

    def test_the_judge_transcript_names_no_arm(self):
        rec = {
            "steps": [
                {
                    "kind": "say",
                    "turn": 1,
                    "user": ["hi"],
                    "photos": ["receipt"],
                    "replies": ["hello"],
                    "tools": [
                        {"name": "memory.write", "args": {"content": "x"}, "result": "stored", "via": "mem_write"}
                    ],
                },
                {"kind": "restart"},
            ]
        }
        text = grade.render_transcript(rec)
        self.assertIn("memory_write(", text)
        self.assertIn("GREENLEAF MARKET", text)
        self.assertIn("restarted", text)
        for word in ("orch_turn", "nimbusd", "refloop", "harness", "arm A", "arm B"):
            self.assertNotIn(word, text)


class WireTest(unittest.TestCase):
    def test_anthropic_exchanges_give_calls_results_and_reply_fields(self):
        exchanges = [
            {
                "request": {"messages": []},
                "response": {
                    "content": [{"type": "tool_use", "id": "t1", "name": "files_read", "input": {"name": "a"}}]
                },
            },
            {
                "request": {
                    "messages": [
                        {"role": "user", "content": [{"type": "tool_result", "tool_use_id": "t1", "content": "bytes"}]}
                    ]
                },
                "response": {
                    "content": [
                        {
                            "type": "tool_use",
                            "id": "t2",
                            "name": "orch_turn",
                            "input": {"reply": "ok", "mem_write": [{"content": "fact"}], "ask": ""},
                        }
                    ]
                },
            },
        ]
        tools, harness = arms.harness_calls(exchanges)
        self.assertEqual(
            [(t["name"], t["result"][:5]) for t in tools], [("files_read", "bytes"), ("memory_write", "(repl")]
        )
        self.assertEqual(harness["fields"], {"mem_write": [{"content": "fact"}]})
        self.assertEqual(harness["orch_turn_calls"], 1)

    def test_a_spawn_is_seen_and_its_follow_up_turn_is_kept(self):
        spawn = {
            "type": "tool_use",
            "id": "o1",
            "name": "orch_turn",
            "input": {"reply": "started a reader", "session_ops": [{"op": "spawn", "task": "read it all"}]},
        }
        later = {
            "type": "tool_use",
            "id": "o2",
            "name": "orch_turn",
            "input": {"reply": "the fee is 38", "mem_write": [{"content": "fee 38"}]},
        }
        first = [{"request": {"messages": []}, "response": {"content": [spawn]}}]
        self.assertTrue(arms.spawned(first))
        self.assertFalse(arms.spawned([{"request": {"messages": []}, "response": {"content": [later]}}]))
        tools, harness = arms.harness_calls(first + [{"request": {"messages": []}, "response": {"content": [later]}}])
        self.assertEqual([t["name"] for t in tools], ["session_spawn", "memory_write"])
        self.assertEqual(set(harness["fields"]), {"session_ops", "mem_write"})

    def test_automatic_recall_is_read_off_the_request_and_shown_to_the_judge(self):
        block = "Current time: 09:00.\nThis turn you may call tools.\n\n## RELEVANT MEMORIES\n- Locker number is 86.\n- Car reg KN19 ZTF.\n\n\n[CHANNEL] web\n[USER]\nWhich locker is mine?"
        ant = [
            {"request": {"messages": [{"role": "user", "content": [{"type": "text", "text": block}]}]}, "response": {}}
        ]
        oai = [{"request": {"input": [{"role": "user", "content": block}]}, "response": {}}]
        self.assertEqual(arms.recalled_memories(ant), ["Locker number is 86.", "Car reg KN19 ZTF."])
        self.assertEqual(arms.recalled_memories(oai), ["Locker number is 86.", "Car reg KN19 ZTF."])
        self.assertEqual(arms.recalled_memories([{"request": {"messages": []}, "response": {}}]), [])
        text = grade.render_transcript(
            {
                "steps": [
                    {"kind": "say", "turn": 1, "user": ["x"], "replies": ["86"], "recalled": ["Locker number is 86."]}
                ]
            }
        )
        self.assertIn("put in front of the assistant for this turn: Locker number is 86.", text)

    def test_openai_exchanges_and_a_bounced_final(self):
        call = {
            "type": "function_call",
            "call_id": "c1",
            "name": "orch_turn",
            "arguments": json.dumps({"reply": "path x"}),
        }
        exchanges = [
            {"request": {"input": []}, "response": {"output": [call]}},
            {
                "request": {
                    "input": [
                        call,
                        {"type": "function_call_output", "call_id": "c1", "output": "ERROR: [REPLY CHECK] plumbing"},
                    ]
                },
                "response": {
                    "output": [
                        {
                            "type": "function_call",
                            "call_id": "c2",
                            "name": "orch_turn",
                            "arguments": "{\"reply\": \"ok\"}",
                        }
                    ]
                },
            },
        ]
        tools, harness = arms.harness_calls(exchanges)
        self.assertEqual(tools, [])
        self.assertEqual(harness["orch_turn_calls"], 2)
        self.assertEqual(len(harness["bounced"]), 1)

    def test_mechanisms_are_read_off_the_wire(self):
        block = "Current time: 22:30 GMT.\nyou may call tools over several rounds (at most 2), not more.\n[FOOD PLAYBOOK]\n[USER]\nhi"
        owner = {"role": "user", "content": [{"type": "text", "text": block}]}
        final = {"role": "user", "content": [{"type": "text", "text": block + "\n[SYSTEM] " + mechanisms.FINAL_NOTICE}]}
        prose = {"content": [{"type": "text", "text": "an answer"}]}
        wrapped = {"content": [{"type": "tool_use", "id": "t", "name": "orch_turn", "input": {"reply": "an answer"}}]}
        nudge = {
            "role": "user",
            "content": [{"type": "text", "text": mechanisms.NUDGE + ", keeping the answer you gave above."}],
        }
        cut = {
            "role": "user",
            "content": [{"type": "tool_result", "tool_use_id": "x", "content": "first bytes…[truncated]"}],
        }
        step = {
            "reply": "It is saved.\n\n(Correction: that has not been saved.)",
            "tools": [{"name": "files_read"}],
            "harness": {},
        }
        kept = mechanisms.scan_turn(
            [
                {"status": 200, "request": {"messages": [owner]}, "response": prose},
                {
                    "status": 200,
                    "request": {"messages": [final, {"role": "assistant", "content": prose["content"]}, nudge]},
                    "response": wrapped,
                },
            ],
            step,
        )
        self.assertEqual((kept["finalize_nudge"], kept["final_notice"], kept["discarded_answers"]), (1, 1, 0))
        self.assertEqual((kept["round_cap"], kept["food_block"], kept["earlier_tool_rows"]), (2, 1, 0))
        self.assertEqual((kept["correction_added"], kept["requests"], kept["tool_calls"]), (1, 2, 1))
        redone = mechanisms.scan_turn(
            [
                {"status": 200, "request": {"messages": [owner]}, "response": prose},
                {"status": 200, "request": {"messages": [final, cut]}, "response": wrapped},
            ],
            step,
        )
        self.assertEqual(
            (redone["finalize_nudge"], redone["discarded_answers"], redone["truncated_results"]), (0, 1, 1)
        )
        forced = mechanisms.scan_turn(
            [
                {
                    "status": 200,
                    "request": {
                        "input": [{"role": "user", "content": block}],
                        "tool_choice": {"type": "function", "name": "orch_turn"},
                    },
                    "response": {
                        "output": [{"type": "function_call", "call_id": "c", "name": "orch_turn", "arguments": "{}"}]
                    },
                }
            ],
            {"reply": "ok"},
        )
        self.assertEqual(forced["forced_final"], 1)

    def test_the_daemon_notices_are_not_the_assistants_replies(self):
        rows = [
            "The assistant wants to schedule 'Vitamin D reminder'. Approve it under Assistant > Routines, or ignore this and it never runs.",
            "I've created the daily 07:30 reminder. It waits for your approval.",
            "Standing rule saved (p1): Keep answers to one line. Review or remove: /prefs",
        ]
        replies, notices = arms.split_notices(rows)
        self.assertEqual(replies, [rows[1]])
        self.assertEqual(notices, [rows[0], rows[2]])
        text = grade.render_transcript(
            {"steps": [{"kind": "say", "turn": 1, "user": ["x"], "replies": replies, "notices": notices}]}
        )
        self.assertIn("the system itself also showed the owner this notice: The assistant wants to schedule", text)
        self.assertNotIn("(reply 2)", text)

    def test_usage_is_one_shape_for_both_providers(self):
        ant = normalize_usage(
            {
                "usage": {
                    "input_tokens": 2,
                    "cache_read_input_tokens": 100,
                    "cache_creation_input_tokens": 10,
                    "output_tokens": 7,
                }
            }
        )
        self.assertEqual(ant, {"input": 112, "cached": 100, "cache_write": 10, "output": 7})
        oai = normalize_usage(
            {"usage": {"input_tokens": 50, "input_tokens_details": {"cached_tokens": 40}, "output_tokens": 5}}
        )
        self.assertEqual(oai, {"input": 50, "cached": 40, "cache_write": 0, "output": 5})
        self.assertIsNone(normalize_usage({"data": []}))

    def test_prices(self):
        usage = {"input": 1_000_000, "cached": 0, "cache_write": 0, "output": 0}
        self.assertAlmostEqual(costs.cost_usd("claude-sonnet-5-5", usage), 2.00)
        self.assertAlmostEqual(costs.cost_usd("gpt-5.6-luna", usage), 0.20)
        self.assertAlmostEqual(costs.cost_usd("claude-haiku-5-5", {**usage, "input": 50_000}), 0.005)
        self.assertAlmostEqual(costs.cost_usd("claude-haiku-5-5", usage), 0.50)  # over 100K: the long tier
        cached = {"input": 1_000_000, "cached": 900_000, "cache_write": 100_000, "output": 0}
        self.assertAlmostEqual(costs.cost_usd("claude-sonnet-5-5", cached), 0.09 + 0.25)
        self.assertAlmostEqual(costs.cold_cost_usd("claude-sonnet-5-5", cached), 2.00)
        self.assertGreater(costs.cost_usd("some-new-model", usage), 2.0)  # unknown never prices as free


if __name__ == "__main__":
    unittest.main(verbosity=1)
