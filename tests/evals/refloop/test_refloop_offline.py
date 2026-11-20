#!/usr/bin/env python3
"""Offline self-test of the reference-loop rig: no network, no keys, no nimbusd.

  python3 tests/evals/refloop/test_refloop_offline.py

Covers the loop itself (with a scripted provider), the transcript save and reload, the
summary of the oldest half, the burst channel, the scenario files, every check of the
grader (one passing and one failing case each), the wire parsing of both providers,
the usage normalization and the price table, and virtual time: the advance step on both
arms (a 04:00 rollover, an overnight gap, a week), the idle ticks after it, cold pricing
after a gap over 300 s, restarts that keep the offset, and a new chat key per conversation.
"""

import json
import queue
import sys
import tempfile
import threading
import time
import unittest
from datetime import datetime
from pathlib import Path
from zoneinfo import ZoneInfo

REPO = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(REPO / "tools" / "refloop"))

import arms  # noqa: E402
import compare  # noqa: E402
import costs  # noqa: E402
import fixtures  # noqa: E402
import grade  # noqa: E402
import mechanisms  # noqa: E402
import nimbusd_ctl  # noqa: E402
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
    "files": {
        "trips/list.md": "# Lisbon 4-7 March\n- passport\n- oat milk 1 litre",
        "money/s.csv": "date,category,note,amount\n2026-11-18,coffee,kiosk,2.40\n# replaced: 2026-11-19,x,y,1\n"
        "2026-11-19,lunch,bakery,6.75\n2026-11-19,bus,day pass,4.10\n",
    },
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
    "files_at_most": (2, 1),
    "loops_count": (1, 0),
    "loops_regex": ("08:15", "07:30"),
    "loops_not_regex": ("07:30", "08:15"),
    "memory_regex": ("24 December", "14 December"),
    "memory_not_regex": ("14 December", "24 December"),
    "day_rows": (
        {"project": "money", "name": "s.csv", "day": "2026-11-19", "count": 2, "match": ["bakery"], "none": ["kiosk"]},
        {"project": "money", "name": "s.csv", "day": "2026-11-19", "count": 3},
    ),
    "reply_total": ({"values": ["560"]}, {"values": ["612.5"], "partial": ["52"], "named": ["line 7"]}),
}


class GradeTest(unittest.TestCase):
    def test_every_check_has_a_passing_and_a_failing_case(self):
        self.assertEqual(set(CASES), set(grade.CHECKS))
        self.assertEqual(set(CASES), scenario.CHECK_NAMES)
        for name, (good, bad) in CASES.items():
            self.assertTrue(grade.CHECKS[name](good, STEP, STATE, EARLIER)[0], name)
            self.assertFalse(grade.CHECKS[name](bad, STEP, STATE, EARLIER)[0], name)

    def test_rows_only_reads_past_comment_lines(self):
        state = {"files": {"m/s.csv": "date,item,amount\n2026-11-19,coffee,3.10\n#removed: 2026-11-19,train,12.60\n"}}
        arg = {"all": ["3\\.10"], "none": ["12\\.60"]}
        self.assertFalse(grade.CHECKS["file_matching"](arg, {}, state, [])[0])
        self.assertTrue(grade.CHECKS["file_matching"]({**arg, "rows_only": True}, {}, state, [])[0])

    def test_a_doubled_append_fails_the_day_rows_check(self):
        state = {"files": dict(STATE["files"])}
        state["files"]["money/s.csv"] += "2026-11-19,bus,day pass,4.10\n"
        arg = {"project": "money", "name": "s.csv", "day": "2026-11-19", "count": 2, "match": ["day pass"]}
        self.assertTrue(grade.CHECKS["day_rows"](arg, {}, STATE, [])[0])
        self.assertFalse(grade.CHECKS["day_rows"](arg, {}, state, [])[0])
        self.assertFalse(grade.CHECKS["day_rows"]({**arg, "count": 3}, {}, state, [])[0])  # "day pass" twice

    def test_a_partial_total_passes_only_when_it_names_the_missing_rows(self):
        line = r"\blines?\s+(?:\d+\s*(?:,|and|&)\s*)*{}\b"
        arg = {
            "values": ["124.30"],
            "partial": ["96.05"],
            "named": [line.format(9) + "|juice", line.format(18) + "|Ines"],
        }
        check = grade.CHECKS["reply_total"]
        self.assertTrue(check(arg, {"reply": "You spent £124.30 that day."}, {}, [])[0])
        self.assertTrue(check(arg, {"reply": "£96.05, but lines 9 and 18 have extra commas."}, {}, [])[0])
        self.assertFalse(check(arg, {"reply": "You spent £96.05 that day."}, {}, [])[0])
        self.assertFalse(check(arg, {"reply": "£96.05; line 9 has an extra comma."}, {}, [])[0])  # one of two
        self.assertTrue(check(arg, {"reply": "£96.05 without the juice and Ines rows."}, {}, [])[0])
        self.assertFalse(check(arg, {"reply": "You spent about £100."}, {}, [])[0])
        self.assertTrue(check({"values": ["2353"]}, {"reply": "2,353 kcal"}, {}, [])[0])

    def test_day_placeholders_and_the_running_day(self):
        sc = {"say": "Total for {D-1:dm} ({D-1:wd})?", "x": ["{D0}", {"d": "{D+1}"}], "rd": "{RD}"}
        self.assertEqual(
            scenario.fill_days(sc, "2026-11-19"),
            {"say": "Total for 18 Nov (Wednesday)?", "x": ["2026-11-19", {"d": "2026-11-20"}], "rd": "{RD}"},
        )
        self.assertEqual(scenario.running_day(london(2026, 11, 20, 0, 30)), "2026-11-19")
        self.assertEqual(scenario.running_day(london(2026, 11, 20, 4, 0)), "2026-11-20")
        step = {"vt": london(2026, 11, 20, 0, 30)}
        arg = grade._at_turn({"day": "{RD}"}, step)
        self.assertEqual(arg, {"day": "2026-11-19"})

    def test_a_seed_skill_is_an_id_and_its_md(self):
        good = {"id": "x", "title": "t", "judge_notes": "n", "turns": [{"say": "hi"}]}
        scenario.validate({**good, "seed": {"skills": [{"id": "my-log", "md": "---\ntitle: t\n---\nbody"}]}})
        for bad in ({"id": "My Log", "md": "x"}, {"id": "a"}, {"id": "a", "md": "x", "extra": 1}):
            with self.assertRaises(scenario.ScenarioError):
                scenario.validate({**good, "seed": {"skills": [bad]}})

    def test_the_counter_arm_writes_every_append_twice(self):
        calls = []

        class Tools:
            def specs(self):
                return []

            def call(self, name, args):
                calls.append(name)
                return ("appended", False)

        tools = arms.DoubledAppend(Tools())
        self.assertEqual(tools.call("files_append", {"text": "a"}), ("appended", False))
        tools.call("files_read", {})
        self.assertEqual(calls, ["files_append", "files_append", "files_read"])
        self.assertIn("X", arms.ARMS)

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

    def test_a_chat_completions_tool_round_is_no_discarded_answer(self):
        # RB-Q4b: the Chat Completions wire (Mistral, Z.ai) carries its calls in choices[0].message.tool_calls. A
        # tool round followed by the next request is the loop working, never a discarded answer; a text-only
        # round followed by another request is one. Both shapes, so the reader cannot pass by ignoring the wire.
        owner = {"role": "user", "content": "Current time: 22:30 GMT.\n[USER]\nhi"}

        def chat(content, calls=()):
            msg = {"content": content}
            if calls:
                msg["tool_calls"] = [
                    {"id": f"c{i}", "function": {"name": n, "arguments": "{}"}} for i, n in enumerate(calls)
                ]
            return {"choices": [{"message": msg, "finish_reason": "tool_calls" if calls else "stop"}]}

        tool_turn = mechanisms.scan_turn(
            [
                {"status": 200, "request": {"messages": [owner]}, "response": chat("", ["calc"])},
                {"status": 200, "request": {"messages": [owner]}, "response": chat("", ["orch_turn"])},
            ],
            {"reply": "4"},
        )
        self.assertEqual(tool_turn["discarded_answers"], 0)
        redone = mechanisms.scan_turn(
            [
                {"status": 200, "request": {"messages": [owner]}, "response": chat("The answer is 4.")},
                {"status": 200, "request": {"messages": [owner]}, "response": chat("", ["orch_turn"])},
            ],
            {"reply": "4"},
        )
        self.assertEqual(redone["discarded_answers"], 1)

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


LONDON = ZoneInfo("Europe/London")


def london(*args) -> int:
    return int(datetime(*args, tzinfo=LONDON).timestamp())


class FakeClockDaemon:
    """The test clock routes of a nimbusd: GET/POST /test/clock, /api/fold/status, the watch poll."""

    def __init__(self, fold_rungs=0):
        self.offset, self.idle_steps, self.posts, self.fold_rungs = 0, 0, [], fold_rungs

    def clock(self):
        return {"offset_s": self.offset, "epoch": int(time.time()) + self.offset, "idle_steps": self.idle_steps}

    def advance(self, seconds):
        self.posts.append(seconds)
        self.offset += seconds
        self.idle_steps += 1
        self.fold_rungs = max(self.fold_rungs - 1, 0)  # each idle step runs one rung of a due compaction
        return self.clock()

    def advance_to(self, epoch):
        return self.advance(epoch - self.clock()["epoch"])

    fold_busy = nimbusd_ctl.Nimbusd.fold_busy
    tick_idle = nimbusd_ctl.Nimbusd.tick_idle

    def call(self, method, path, **kw):
        if path.startswith("/api/fold/status"):
            return 200, json.dumps({"running": self.fold_rungs > 0, "queued": False})
        return 200, json.dumps({"busy": 0})


def job_for(tmp, sc_id="t"):
    return arms.Job("claude-sonnet-5-5", "anthropic", None, "", {"id": sc_id, "turns": []}, 1, Path(tmp))


def row(seq, t, model="claude-sonnet-5-5", **ctx):
    """One captured exchange: 9,000 cached prompt tokens, 1,000 fresh, 100 out."""
    usage = {"input": 10000, "cached": 9000, "cache_write": 0, "output": 100}
    return {
        "seq": seq,
        "t_start": t,
        "ms": 2000,
        "model": model,
        "usage": usage,
        "path": "/v1/messages",
        "method": "POST",
        "status": 200,
        "ctx": ctx,
    }


class TimeTest(unittest.TestCase):
    def setUp(self):
        self._poll = arms.SETTLE_POLL_S
        arms.SETTLE_POLL_S = 0

    def tearDown(self):
        arms.SETTLE_POLL_S = self._poll

    def test_an_advance_is_seconds_a_duration_or_a_wall_time(self):
        self.assertEqual(scenario.parse_advance(90), ("seconds", 90))
        self.assertEqual(scenario.parse_advance("8h"), ("seconds", 28800))
        self.assertEqual(scenario.parse_advance("7d"), ("seconds", 604800))
        self.assertEqual(scenario.parse_advance("04:00"), ("wall", 4, 0))
        for bad in (0, -5, True, 1.5, "4:00", "24:00", "8 h", "tomorrow", 367 * 86400, None):
            with self.assertRaises(scenario.ScenarioError, msg=repr(bad)):
                scenario.parse_advance(bad)
        good = {"id": "x", "title": "t", "judge_notes": "n", "turns": [{"say": "hi"}, {"advance": "04:00"}]}
        scenario.validate(good)
        # YAML 1.1 reads an unquoted 12:00 as the number 720: the scenario loader must not.
        loaded = scenario.parse_yaml("turns:\n  - advance: 12:00\n  - advance: 04:00\n  - advance: 90\n  - say: 1.5")
        self.assertEqual([t.get("advance") for t in loaded["turns"][:3]], ["12:00", "04:00", 90])
        self.assertEqual(scenario.parse_advance(loaded["turns"][0]["advance"]), ("wall", 12, 0))
        self.assertEqual(loaded["turns"][3]["say"], 1.5)
        for bad_step in (
            {"advance": "soon"},
            {"advance": 60, "expect": [{"reply_regex": "a"}]},
            {"advance": 60, "say": "x"},
        ):
            with self.assertRaises(scenario.ScenarioError, msg=repr(bad_step)):
                scenario.validate({**good, "turns": [{"say": "hi"}, bad_step]})

    def test_the_0400_rollover_lands_on_the_next_0400(self):
        late = london(2026, 11, 17, 23, 10)
        self.assertEqual(scenario.advance_target("04:00", late), london(2026, 11, 18, 4, 0))
        self.assertEqual(scenario.advance_target("04:00", london(2026, 11, 18, 3, 59)), london(2026, 11, 18, 4, 0))
        on_it = london(2026, 11, 18, 4, 0)  # strictly after: an advance always moves
        self.assertEqual(scenario.advance_target("04:00", on_it), london(2026, 11, 19, 4, 0))
        # The night British Summer Time ends (25 Oct 2026): 23:00 BST to 04:00 GMT is six real hours.
        bst = london(2026, 10, 24, 23, 0)
        self.assertEqual(scenario.advance_target("04:00", bst) - bst, 6 * 3600)
        self.assertEqual(scenario.advance_target("8h", late), late + 8 * 3600)

    def test_the_reference_loop_stamps_move_with_its_clock(self):
        conv = refloop.Conversation([], chat_key="k")
        before = conv.stamp("x")
        conv.advance(7 * 86400)
        week = datetime.now(LONDON).timestamp() + 7 * 86400
        self.assertEqual(conv.stamp("x")[:15], datetime.fromtimestamp(week, LONDON).strftime("[%Y-%m-%d %a"))
        self.assertNotEqual(conv.stamp("x"), before)
        with self.assertRaises(ValueError):
            conv.advance(-1)
        later = refloop.Conversation([], clock_offset_s=7 * 86400)  # a new conversation starts on the same clock
        self.assertIn(datetime.fromtimestamp(week, LONDON).strftime("%A %d %B %Y"), later.system)

    def test_the_reference_loop_stamp_is_the_device_stamp(self):
        # Ruling 64: arm B's message stamp has RB-REC5's forms byte for byte (lib/core day_start.h),
        # so both arms see the same stamp: year, weekday, and the day clause before the day start.
        at = lambda *a: datetime(*a, tzinfo=LONDON)  # noqa: E731
        self.assertEqual(refloop.message_stamp(at(2026, 11, 19, 8, 10)), "[2026-11-19 Thu 08:10]")
        self.assertEqual(
            refloop.message_stamp(at(2026, 11, 19, 0, 30)), "[2026-11-19 Thu 00:30 (day 2026-11-18 until 04:00)]"
        )
        self.assertEqual(refloop.message_stamp(at(2026, 11, 19, 4, 0)), "[2026-11-19 Thu 04:00]")
        self.assertEqual(
            refloop.message_stamp(at(2026, 11, 19, 3, 59)), "[2026-11-19 Thu 03:59 (day 2026-11-18 until 04:00)]"
        )
        self.assertEqual(
            refloop.message_stamp(at(2026, 3, 1, 0, 5)), "[2026-03-01 Sun 00:05 (day 2026-02-28 until 04:00)]"
        )
        self.assertEqual(refloop.message_stamp(at(2026, 11, 19, 0, 30), day_start_min=0), "[2026-11-19 Thu 00:30]")
        conv = refloop.Conversation([], chat_key="k")
        self.assertRegex(
            conv.stamp("hello"),
            r"^\[\d{4}-\d\d-\d\d [A-Z][a-z]{2} \d\d:\d\d( \(day \d{4}-\d\d-\d\d until 04:00\))?\] hello$",
        )

    def test_a_saved_transcript_keeps_its_clock_and_an_old_one_loads_at_zero(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "t.json"
            conv = refloop.Conversation([], path, chat_key="k")
            conv.advance(3600)
            self.assertEqual(refloop.Conversation.load(path).clock_offset_s, 3600)
            old = json.loads(path.read_text())
            del old["clock_offset_s"]
            path.write_text(json.dumps(old))
            self.assertEqual(refloop.Conversation.load(path).clock_offset_s, 0)

    def test_both_arms_honour_an_advance_and_land_on_the_same_wall_time(self):
        with tempfile.TemporaryDirectory() as tmp:
            a, b = arms.ArmA(job_for(tmp)), arms.ArmB(job_for(tmp))
            a.d, b.d = FakeClockDaemon(), FakeClockDaemon()
            b.conv = refloop.Conversation([], Path(tmp) / "b.json", chat_key=b.chat_key)
            recs = [arm.step({"advance": "04:00"}) for arm in (a, b)]
            for arm, rec in zip((a, b), recs):
                self.assertEqual(rec["kind"], "advance")
                self.assertTrue(rec["local"].endswith("04:00"), rec)
                self.assertEqual(arm.d.posts[0], rec["advanced_s"])
                self.assertGreater(rec["advanced_s"], 0)
                self.assertEqual(arm.offset_s, arm.d.offset)
            self.assertEqual(recs[0]["local"], recs[1]["local"])
            self.assertEqual(b.conv.clock_offset_s, b.d.offset)  # arm B's own stamps moved too
            self.assertIn(" 04:0", b.conv.stamp("x"))

    def test_idle_steps_run_until_the_compaction_an_overnight_gap_made_due_is_done(self):
        with tempfile.TemporaryDirectory() as tmp:
            arm = arms.ArmA(job_for(tmp))
            arm.d = FakeClockDaemon(fold_rungs=5)
            rec = arm.step({"advance": "8h"})
            self.assertEqual(arm.d.posts, [28800, 0, 0, 0, 0])  # the move, then idle steps until it is done
            self.assertEqual(arm.d.fold_rungs, 0)
            self.assertEqual(rec["idle_steps"], 5)
            quiet = arms.ArmA(job_for(tmp))
            quiet.d = FakeClockDaemon()
            quiet.step({"advance": 60})
            self.assertEqual(quiet.d.posts, [60] + [0] * nimbusd_ctl.IDLE_TICKS_MIN)

    def test_a_request_after_an_overnight_gap_is_priced_cold_and_billed_as_billed(self):
        with tempfile.TemporaryDirectory() as tmp:
            arm = arms.ArmA(job_for(tmp))
            arm.d = FakeClockDaemon()
            now = time.time()
            arm.proxy.records += [row(1, now - 20, turn=1), row(2, now - 15, turn=1)]
            arm.step({"advance": "8h"})
            arm.proxy.records += [row(3, now + 1, turn=2), row(4, now + 4, turn=2), row(5, now + 60, turn=3)]
            warm = costs.cost_usd("claude-sonnet-5-5", row(0, 0)["usage"])
            cold = costs.cold_priced_usd("claude-sonnet-5-5", row(0, 0)["usage"])
            self.assertAlmostEqual(warm, (1000 * 2.00 + 9000 * 0.10 + 100 * 10.0) / 1e6)
            self.assertAlmostEqual(cold, (1000 * 2.00 + 9000 * 2.50 + 100 * 10.0) / 1e6)
            m1, m2, m3 = arm.metrics(1), arm.metrics(2), arm.metrics(3)
            self.assertEqual((m1["cold_requests"], m2["cold_requests"], m3["cold_requests"]), (0, 1, 0))
            self.assertAlmostEqual(m2["virtual_cost_usd"], cold + warm)  # the second round found it warm
            self.assertAlmostEqual(m2["cost_usd"], 2 * warm)  # billed is what the provider charged
            self.assertAlmostEqual(m3["virtual_cost_usd"], warm)  # a minute later: warm

    def test_the_cold_rule_is_the_gap_on_the_same_model_over_300_s(self):
        t = 1_000_000.0
        rows = [row(1, t), row(2, t + 2 + 299), row(3, t + 600, model="claude-haiku-5-5"), row(4, t + 1000)]
        priced = costs.virtual_costs(rows, [])
        self.assertEqual([priced[n][1] for n in (1, 2, 3, 4)], [False, False, True, True])
        edge = costs.virtual_costs([row(1, t), row(2, t + 2 + 300), row(3, t + 304 + 300.5)], [])
        self.assertEqual([edge[n][1] for n in (1, 2, 3)], [False, False, True])  # exactly 300 s is still warm
        marks = [(t + 100, 301)]  # a 301 s advance between rows 1 and 2 makes row 2 cold
        self.assertTrue(costs.virtual_costs(rows, marks)[2][1])
        self.assertEqual(costs.offset_at(marks, t), 0)
        self.assertEqual(costs.offset_at(marks, t + 100), 301)
        # A model with no cache-write premium re-reads at its input price.
        luna = {"input": 1000, "cached": 800, "cache_write": 0, "output": 0}
        self.assertAlmostEqual(costs.cold_priced_usd("gpt-5.6-luna", luna), 1000 * 0.20 / 1e6)

    def test_turns_carry_the_chat_key_the_gap_and_the_cold_transition(self):
        with tempfile.TemporaryDirectory() as tmp:
            arm = arms.ArmA(job_for(tmp, "night"))
            arm.d = FakeClockDaemon()
            arm.speak = lambda texts, step, rec: rec.update(replies=["ok"], tools=[])
            arm.metrics = lambda turn: {}
            first, warm = arm.step({"say": "a"}), arm.step({"say": "b"})
            arm.step({"advance": "04:00"})
            after = arm.step({"say": "c"})
            self.assertEqual([first["chat_key"], after["chat_key"]], ["night-rep1-c0"] * 2)
            self.assertEqual(
                [first["cold_transition"], warm["cold_transition"], after["cold_transition"]], [True, False, True]
            )
            self.assertIsNone(first["gap_s"])
            self.assertGreater(after["gap_s"], 270)
            arm.restart = lambda: None
            arm.step({"restart": True})
            self.assertTrue(arm.step({"say": "d"})["cold_transition"])

    def test_new_conversation_is_a_new_chat_key_on_the_same_clock(self):
        with tempfile.TemporaryDirectory() as tmp:
            arm = arms.ArmB(job_for(tmp, "mem"))
            arm.d, arm.tools = FakeClockDaemon(), FakeTools()
            arm.dir.mkdir(parents=True)
            arm.conv = arm._conversation(load=False)
            arm.step({"advance": "7d"})
            first_key = arm.conv.cache_key
            rec = arm.step({"new_conversation": True})
            self.assertEqual(rec, {"kind": "new_conversation", "chat_key": "mem-rep1-c1"})
            self.assertNotEqual(arm.conv.cache_key, first_key)
            self.assertEqual(arm.conv.cache_key, "refloop-mem-rep1-c1")
            self.assertEqual(arm.conv.clock_offset_s, 7 * 86400)  # time does not go back with a new chat
            self.assertEqual(arm.conv.messages, [])
            arm.speak = lambda texts, step, rec: rec.update(replies=["ok"], tools=[])
            arm.metrics = lambda turn: {}
            turn = arm.step({"say": "remember?"})
            self.assertEqual((turn["chat_key"], turn["cold_transition"]), ("mem-rep1-c1", True))
            unnamed = [refloop.Conversation([]).cache_key for _ in range(2)]
            self.assertNotEqual(unnamed[0], unnamed[1])  # never the same key by the clock's second

    def test_a_restart_puts_the_offset_back(self):
        with tempfile.TemporaryDirectory() as tmp:

            class Daemon(nimbusd_ctl.Nimbusd):
                def __init__(self):
                    super().__init__(Path(tmp), {}, Path(tmp) / "log")
                    self.server_offset, self.bodies = 0, []

                def start(self, head):
                    self.server_offset = 0  # a new process starts at the host clock

                def stop(self):
                    pass

                def _send(self, req, timeout=60):
                    if req.get_method() == "POST":
                        self.bodies.append((json.loads(req.data), timeout))
                        self.server_offset += json.loads(req.data)["advance_s"]
                    answer = {"offset_s": self.server_offset, "epoch": 0, "idle_steps": 0}
                    return 200, json.dumps(answer)

            d = Daemon()
            d.advance(5000)
            d.restart()
            self.assertEqual(d.server_offset, 5000)
            self.assertEqual(d.clock()["offset_s"], 5000)
            self.assertEqual(d.bodies[-1], ({"advance_s": 5000}, nimbusd_ctl.CLOCK_TIMEOUT_S))
            self.assertGreaterEqual(nimbusd_ctl.CLOCK_TIMEOUT_S, 600)  # the route may run a paid idle step

    def test_a_restart_past_a_year_puts_the_offset_back_in_steps(self):
        with tempfile.TemporaryDirectory() as tmp:
            d = nimbusd_ctl.Nimbusd(Path(tmp), {}, Path(tmp) / "log")
            posts = []
            d.start = d.stop = lambda *a: None
            d.advance = lambda s: posts.append(s) or setattr(d, "offset_s", d.offset_s + s)
            d.offset_s = 2 * scenario.MAX_ADVANCE_S + 5
            d.restart()
            self.assertEqual(posts, [scenario.MAX_ADVANCE_S, scenario.MAX_ADVANCE_S, 5])

    def test_a_failed_advance_leaves_the_clock_marks_alone(self):
        with tempfile.TemporaryDirectory() as tmp:
            arm = arms.ArmA(job_for(tmp))
            arm.d = FakeClockDaemon()

            def refuse(seconds):
                raise RuntimeError("POST /test/clock answered 400")

            arm.d.advance = refuse
            with self.assertRaises(RuntimeError):
                arm.step({"advance": "8h"})
            self.assertEqual((arm.clock_marks, arm.offset_s), ([], 0))

    def test_a_restart_keeps_arm_b_on_its_clock_and_counts_its_own_requests(self):
        with tempfile.TemporaryDirectory() as tmp:
            arm = arms.ArmB(job_for(tmp))
            arm.d, arm.tools = FakeClockDaemon(), FakeTools()
            arm.dir.mkdir(parents=True)
            arm.conv = arm._conversation(load=False)
            arm.step({"advance": "7d"})
            arm.d.restart = lambda: arm.proxy.records.append(row(9, time.time(), **dict(arm.proxy.ctx)))
            arm._connect = lambda: None
            rec = arm.step({"restart": True})
            self.assertEqual(arm.conv.clock_offset_s, 7 * 86400)  # reloaded from the saved transcript
            self.assertEqual((rec["kind"], rec["head_requests"]), ("restart", 1))
            self.assertEqual(arm.metrics(0)["head_requests"], 0)  # not booked on an earlier step

    def test_every_rig_daemon_runs_the_test_clock(self):
        env = nimbusd_ctl.Nimbusd(Path("/tmp/x"), {}, Path("/tmp/x/log"))._environment("anthropic")
        self.assertEqual(env["NIMBUSD_TEST_CLOCK"], "1")
        self.assertTrue(env["NIMBUSD_WEB_TOKEN"])
        self.assertEqual(env["TZ"], scenario.RIG_TZ)

    def test_the_judge_is_told_time_passed(self):
        rec = {"steps": [{"kind": "advance", "local": "Wed 18 Nov 2026 04:00"}]}
        self.assertIn("[time passes: it is now Wed 18 Nov 2026 04:00]", grade.render_transcript(rec))


class MistralWireTest(unittest.TestCase):
    """Lane RB-R1 (ruling 102): the rig reads the Chat Completions wire the harness uses for Mistral, and prices the
    firmware's Mistral rows; an unpriced Mistral model must never read as free."""

    def test_tool_calls_and_results_are_read_off_the_chat_wire(self):
        response = {
            "choices": [
                {
                    "message": {
                        "role": "assistant",
                        "tool_calls": [
                            {
                                "id": "c1",
                                "type": "function",
                                "function": {"name": "files_read", "arguments": "{\"name\": \"a.csv\"}"},
                            },
                            {"id": "c2", "type": "function", "function": {"name": "calc", "arguments": "not json"}},
                        ],
                    }
                }
            ]
        }
        self.assertEqual(
            arms._tool_uses(response), [("c1", "files_read", {"name": "a.csv"}), ("c2", "calc", {"_raw": "not json"})]
        )
        request = {
            "messages": [{"role": "user", "content": "hi"}, {"role": "tool", "tool_call_id": "c1", "content": "rows"}]
        }
        self.assertEqual(arms._tool_results(request), [("c1", "rows")])
        self.assertIn("/v1/chat/completions", arms.HEAD_PATHS)

    def test_mistral_heads_are_priced_from_the_firmware_table(self):
        usage = {"input": 1_000_000, "cached": 0, "cache_write": 0, "output": 1_000_000}
        self.assertAlmostEqual(costs.cost_usd("mistral-medium-latest", usage), 9.0)
        self.assertAlmostEqual(costs.cost_usd("mistral-small-latest", usage), 0.8)
        self.assertGreater(costs.cost_usd("mistral-unknown-model", usage), 0.0)

    def test_the_loop_sends_the_mistral_effort_the_harness_sends(self):
        # RB-Q4b (ruling 104): Large 4 refuses effort "none" and the harness sends "low" (mistralHeadEffort); the
        # loop mirrors the harness's effort on every wire, so it sends the same reasoning_effort, and none when the
        # harness sends none (Medium). Large 4 is a rig model priced from the firmware row (2 / 6 USD per 1M).
        sent = []

        class FakeCompletions:
            def create(self, **kw):
                sent.append(kw)
                msg = type("M", (), {"to_dict": lambda self: {"role": "assistant", "content": "hi"}})()
                choice = type("C", (), {"message": msg, "finish_reason": "stop"})()
                return type("R", (), {"choices": [choice], "usage": None})()

        conv = type("Conv", (), {"system": "S", "messages": [{"role": "user", "content": "hi"}], "specs": []})()
        for effort, expected in (("low", {"reasoning_effort": "low"}), (None, None)):
            head = refloop.MistralChat("mistral-large-latest", effort=effort, api_key="x")
            head.client = type("Cl", (), {"chat": type("Ch", (), {"completions": FakeCompletions()})()})()
            head.step(conv)
            self.assertEqual(sent[-1].get("extra_body"), expected)
        self.assertEqual(compare.MODELS["mistral-large-latest"]["effort"], "low")
        self.assertEqual(compare.MODELS["mistral-large-latest"]["sub_model"], "mistral-small-latest")
        usage = {"input": 1_000_000, "cached": 0, "cache_write": 0, "output": 1_000_000}
        self.assertAlmostEqual(costs.cost_usd("mistral-large-latest", usage), 8.0)


if __name__ == "__main__":
    unittest.main(verbosity=1)
