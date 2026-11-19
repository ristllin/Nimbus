#!/usr/bin/env python3
"""Offline self-test of the replay rig and tools/context_gaps.py: no keys, no network.

  python3 tests/evals/replay/test_replay_offline.py

Needs Pillow (the photo generator); the provider SDKs are not imported.
"""

from __future__ import annotations

import json
import re
import sys
import tempfile
import unittest
from datetime import datetime
from pathlib import Path
from zoneinfo import ZoneInfo

HERE = Path(__file__).resolve().parent
REPO = HERE.parents[2]
sys.path[:0] = [
    str(HERE),
    str(REPO / "tools"),
    str(REPO / "tools" / "refloop"),
    str(REPO / "tests" / "evals" / "behaviour"),
]

import context_gaps as cg  # noqa: E402
import refloop  # noqa: E402
import report  # noqa: E402
import run_replay  # noqa: E402
import synth  # noqa: E402

TZ = ZoneInfo("Europe/London")
SECRET = "ZQXJ-owner-words"  # planted in every text field of the fake corpus; must never come out
T0 = int(datetime(2026, 11, 10, 9, 0, tzinfo=TZ).timestamp())


# ------------------------------------------------------------------ fake corpus
def _row(i, role, kind, t, tags, text=SECRET):
    return {
        "id": f"m{i:08x}",
        "session": "1",
        "ts": t // 3600,
        "role": role,
        "kind": kind,
        "text": text,
        "blob": "",
        "tags": tags,
        "channel": "telegram",
    }


def fake_corpus(d: Path):
    """Four owner turns: gaps 100 s (warm), 400 s (cold at 5 min), 4,000 s (cold at 60 min). Turn 3 is
    two photos sent in the same second (an album) plus the media turn's text row (att); turn 4 has a
    follow-on text 4 s after the first, before the reply."""
    rows, i = [], 0

    def add(*a, **k):
        nonlocal i
        i += 1
        rows.append(_row(i, *a, **k))
        return rows[-1]["id"]

    def turn(t_start, msgs, reply_after=20):
        ids = []
        for kind, dt, extra in msgs:
            tag = f"sent:{t_start + dt}" if kind == "image" else f"at:{t_start + dt},from:R,src:tg:1:{i}{extra}"
            ids.append(add("user", kind, t_start + dt, tag))
        tid = ids[0]
        add("tool", "tool_output", t_start + 5, f"tool:files_read,turn:{tid}")
        add("tool", "tool_output", t_start + 6, f"tool:{SECRET} x,turn:{tid}")
        add("assistant", "message", t_start + reply_after, f"out:reply,at:{t_start + reply_after},turn:{tid}")
        model = "claude-sonnet-5-5" if t_start != T0 else SECRET + " model"
        end = json.dumps(
            {
                "host": "anthropic",
                "model": model,
                "src": "owner",
                "tools": 1,
                "ok": True,
                "in": 1000,
                "out": 50,
                "cr": SECRET,
                "reply": 10,
            }
        )
        add("system", "log", t_start + reply_after, f"ev:turnend,turn:{tid}", text=end)
        return t_start + reply_after

    e = turn(T0, [("message", 0, "")])
    e = turn(e + 100, [("message", 0, "")])
    e = turn(e + 400, [("image", 0, ""), ("image", 0, ""), ("message", 12, ",att:1")])
    turn(e + 4000, [("message", 0, ""), ("message", 4, "")])
    (d / "episodic.jsonl").write_text("\n".join(json.dumps(r) for r in rows) + "\n")
    log = [
        f"0 100 boot boot: n=1 fw=v5 t={T0}",
        "1 2000 - telegram: msg from R (chat 1): " + SECRET,
        "2 8100 - telegram: settle dispatch n=1 after 6000 ms (quiet)",
        "3 9000 - telegram: settle dispatch n=1 after 6100 ms (quiet)",
        "4 20000 - telegram: settle dispatch n=2 after 9500 ms (quiet)",
        "5 90000 - telegram: settle dispatch n=3 after 30000 ms (old)",
        "6 99000 - telegram: settle dispatch n=2 after 31000 ms (full)",
    ]
    (d / "nimbus.log").write_text("\n".join(log) + "\n")


class ContextGaps(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.d = Path(self.tmp.name)
        fake_corpus(self.d)
        self.rows = cg.load_rows(self.d)

    def tearDown(self):
        self.tmp.cleanup()

    def test_cold_share_counts_gaps_from_the_previous_reply(self):
        c = cg.cold_share(cg.owner_turns(self.rows))
        self.assertEqual(c["turns_with_gap"], 3)
        self.assertEqual((c["cold_5m"]["k"], c["cold_60m"]["k"], c["cold_open_270s"]["k"]), (2, 1, 2))

    def test_follow_ons_skip_albums_and_media_text(self):
        f = cg.follow_ons(self.rows)
        self.assertEqual(f["album_members"], 1)
        self.assertEqual(f["gaps_s"], [4])  # the att row is not an arrival
        self.assertEqual((f["within_2s"]["k"], f["within_6s"]["k"]), (0, 1))

    def test_hold_joins_split_by_cause_and_timed_from_the_window(self):
        h = cg.hold_payoffs(self.d)
        self.assertEqual((h["dispatches"], h["joined"], h["joined_quiet"], h["joined_old"]), (5, 3, 1, 1))
        self.assertEqual(h["joined_other"], 1)  # a reason the tool does not know is counted, not a crash
        self.assertEqual(h["quiet_joins"][0]["last_after_first_ms"], 9500 - 6050)

    def test_one_hour_rule_boundaries(self):
        self.assertEqual(cg.one_hour_decision(0.5, 0.3)["decision"], "1-hour write")
        self.assertEqual(cg.one_hour_decision(0.5, 0.3125)["decision"], "5-minute write")  # exactly 0.625: not under
        self.assertEqual(cg.one_hour_decision(0, 0)["decision"], "unmeasured")

    def test_no_owner_text_leaves_the_tool(self):
        """The class rule: every output (gaps, shape) is numbers and names, never a text field."""
        turns = cg.owner_turns(self.rows)
        outs = [
            cg.cold_share(turns),
            cg.follow_ons(self.rows),
            cg.hold_payoffs(self.d),
            cg.device_tokens(turns),
            cg.day_shape(turns, ["2026-11-10"]),
        ]
        for o in outs:
            self.assertNotIn(SECRET, json.dumps(o))
        self.assertEqual(len(cg.day_shape(turns, ["2026-11-10"])["turns"]), 4)
        self.assertEqual(cg.turns_per_day(turns)["mean"], 4.0)


class Shapes(unittest.TestCase):
    ALLOWED = {"at_s", "exact", "msgs", "photos", "text_bytes", "tools", "reply_bytes", "device_model", "device_tokens"}

    def test_committed_shapes_hold_numbers_only(self):
        files = sorted(synth.SHAPES.glob("*.json"))
        self.assertEqual(len(files), 3)
        for p in files:
            d = json.loads(p.read_text())
            self.assertEqual(set(d), {"days", "first_local", "turns"})
            for t in d["turns"]:
                self.assertEqual(set(t), self.ALLOWED, p.name)
                for name, size in t["tools"]:
                    self.assertRegex(name, r"^[a-z_]+$")
                    self.assertIsInstance(size, int)
                self.assertTrue(all(isinstance(v, int) for v in t["device_tokens"].values()))

    def test_synthetic_turns_are_deterministic_and_end_in_the_recall(self):
        shape = synth.load_shape("day-2026-11-04")
        a, b = synth.turns_for(shape, "x"), synth.turns_for(shape, "x")
        self.assertEqual(a, b)
        self.assertEqual(len(a), len(shape["turns"]))
        self.assertEqual(a[-1]["kind"], "recall")
        self.assertEqual(sum(1 for t in a if t["kind"] == "planted"), 1)
        for t, s in zip(a[:-1], shape["turns"]):
            self.assertEqual(len(t["photos"]), min(s["photos"], 4))

    def test_photo_is_a_phone_sized_jpeg(self):
        import base64
        import io

        from PIL import Image

        mt, b64, ref = synth.photo("label", 1)
        img = Image.open(io.BytesIO(base64.b64decode(b64)))
        self.assertEqual((mt, img.size, len(ref)), ("image/jpeg", (600, 800), 8))


# ------------------------------------------------------------------ transcript
def tx_with_turn(tx, now, text="hi", results=(), photos=(), thinking=True):
    tx.open_turn(now, text, photos)
    calls = [{"type": "tool_use", "id": f"c{now}{n}", "name": r[0], "input": {}} for n, r in enumerate(results)]
    first = ([{"type": "thinking", "thinking": "", "signature": "sig"}] if thinking else []) + calls
    if calls:
        tx.add_assistant(first)
        tx.add_results([(c["id"], r[0], {}, r[1], False) for c, r in zip(calls, results)], now + 1)
        tx.add_assistant([{"type": "text", "text": "done"}])
    else:
        tx.add_assistant(first + [{"type": "text", "text": "ok"}])
    tx.mark_request(now + 2)


def tool_bodies(msgs):
    return [b["content"] for m in msgs for b in m["content"] if b.get("type") == "tool_result"]


class Transcript(unittest.TestCase):
    BULK = "x" * 5000
    RECEIPT = "row L1 added"

    def test_cold_open_stubs_closed_bulk_and_keeps_receipts(self):
        tx = refloop.E2Transcript("standard")
        tx_with_turn(tx, T0, results=[("read_file", self.BULK), ("ledger_write", self.RECEIPT)])
        self.assertIn(self.BULK, tool_bodies(tx.render_anthropic()))  # whole inside its own turn
        tx_with_turn(tx, T0 + 600)  # cold
        bodies = tool_bodies(tx.render_anthropic())
        self.assertIn(self.RECEIPT, bodies)
        stub = next(b for b in bodies if b.startswith("[stub read_file"))
        self.assertRegex(stub, r"5000B 1L \d\d:\d\d\] x+ \.\.\. \(full: view id=r\d+\)$")
        self.assertLessEqual(len(stub.encode()), 400)
        self.assertEqual(tx.view(re.search(r"view id=(r\d+)", stub).group(1)), (self.BULK, False))

    def test_warm_open_keeps_bulk_until_the_ceiling(self):
        tx = refloop.E2Transcript("standard")
        tx_with_turn(tx, T0, results=[("read_file", self.BULK)])
        tx_with_turn(tx, T0 + 60)
        self.assertIn(self.BULK, tool_bodies(tx.render_anthropic()))
        for k in range(4):  # five closed results of 5,000 B pass 24,576 B at the next warm open
            tx_with_turn(tx, T0 + 120 + 60 * k, results=[("read_file", self.BULK)])
        self.assertEqual(tool_bodies(tx.render_anthropic()).count(self.BULK), 5)
        tx_with_turn(tx, T0 + 400)
        self.assertNotIn(self.BULK, tool_bodies(tx.render_anthropic()))
        self.assertEqual(tx.changes.get("pointer"), 1)

    def test_outside_content_is_a_stub_after_its_own_turn_even_warm(self):
        tx = refloop.E2Transcript("standard")
        tx_with_turn(tx, T0, results=[("web_search", "page text")])
        self.assertIn("page text", tool_bodies(tx.render_anthropic()))
        tx_with_turn(tx, T0 + 30)
        body = tool_bodies(tx.render_anthropic())[0]
        self.assertTrue(body.startswith("[stub web_search({}) r") and "outside content, not shown" in body, body)
        self.assertNotIn("page text", body)

    def test_closed_turn_thinking_dropped_open_turn_kept(self):
        for replay in (False, True):
            tx = refloop.E2Transcript("standard", replay_thinking=replay)
            tx_with_turn(tx, T0, results=[("ledger_day", "rows")])
            tx.open_turn(T0 + 30, "next", ())
            tx.add_assistant(
                [
                    {"type": "thinking", "thinking": "", "signature": "s2"},
                    {"type": "tool_use", "id": "z", "name": "ledger_day", "input": {}},
                ]
            )
            sigs = [b["signature"] for m in tx.render_anthropic() for b in m["content"] if b.get("type") == "thinking"]
            self.assertEqual(sigs, ["sig", "s2"] if replay else ["s2"])

    def test_photos_stay_two_turns_then_leave_for_good(self):
        tx = refloop.E2Transcript("standard")
        ph = [("image/jpeg", "AAAA", "ab12cd34")]

        def images():
            return sum(1 for m in tx.render_anthropic() for b in m["content"] if b.get("type") == "image")

        tx_with_turn(tx, T0, photos=ph)
        self.assertEqual(images(), 1)
        tx_with_turn(tx, T0 + 30)
        tx_with_turn(tx, T0 + 60)
        self.assertEqual(images(), 1)
        tx_with_turn(tx, T0 + 90)
        self.assertEqual(images(), 0)
        texts = [b["text"] for m in tx.render_anthropic() for b in m["content"] if b.get("type") == "text"]
        self.assertTrue(
            any(t.startswith("[photo pab12cd34 10 Nov 2026 09:00 not shown: view id=pab12cd34]") for t in texts)
        )
        self.assertEqual(tx.changes.get("photo_left"), 1)

    def test_photos_leave_after_an_hour_idle(self):
        tx = refloop.E2Transcript("standard")
        tx_with_turn(tx, T0, photos=[("image/jpeg", "AAAA", "p1")])
        tx_with_turn(tx, T0 + 3700)
        self.assertFalse(any(b.get("type") == "image" for m in tx.render_anthropic() for b in m["content"]))

    def test_render_is_byte_stable_inside_a_turn(self):
        """Prefix property: every request of a turn starts with the bytes of the one before it."""
        tx = refloop.E2Transcript("standard")
        tx_with_turn(tx, T0, results=[("read_file", self.BULK)])
        tx.open_turn(T0 + 40, "go", ())
        prev = None
        for k in range(4):
            msgs = tx.render_anthropic()
            flat = [json.dumps(b, sort_keys=True) for m in msgs for b in m["content"]]
            if prev is not None:
                self.assertEqual(flat[: len(prev) - 1], prev[:-1])
            prev = flat
            tx.add_assistant([{"type": "tool_use", "id": f"k{k}", "name": "read_file", "input": {}}])
            tx.add_results([(f"k{k}", "read_file", {}, self.BULK, False)], T0 + 41 + k)

    def test_compaction_slice_spares_today_and_the_newest_six(self):
        tx = refloop.E2Transcript("tight")
        day1 = T0 - 86400
        for k in range(12):
            tx_with_turn(tx, day1 + 600 * k, text="y" * 3000)
        for k in range(3):
            tx_with_turn(tx, T0 + 600 * k, text="z" * 100)
        self.assertGreater(tx.resident(), tx.H)
        picked = tx.compaction_slice(T0 + 3000)
        self.assertTrue(picked)
        self.assertTrue(set(picked).isdisjoint(set(range(tx.turn - 5, tx.turn + 1))))
        self.assertTrue(all(tx._turn_time(t) < T0 - 4 * 3600 for t in picked))
        tx.apply_digests(picked, [("Mon 09 Nov 2026", "First fact. " * 100)])
        self.assertLessEqual(len(tx.digests[-1][1].encode()), refloop.DIGEST_MAX)
        self.assertTrue(tx.digests[-1][1].endswith("."))
        self.assertLessEqual(tx.resident(), tx.H)
        shown = sum(1 for m in tx.render_anthropic() for b in m["content"] if b.get("text", "").endswith("y" * 3000))
        self.assertEqual(shown, 12 - len(picked))

    def test_full_is_due_by_real_tokens_before_its_byte_mark(self):
        """Lane RB-T4 (ruling 82): at full, compaction is due once the last request reached the due
        mark in tokens (the aim less the next request's growth), or the resident passed H; one token
        under the mark with the resident under H is not due."""
        tx = refloop.E2Transcript("full")
        self.assertEqual(tx.due_tokens, refloop.FULL_DUE_TOKENS)
        day1 = T0 - 86400
        for k in range(45):
            tx_with_turn(tx, day1 + 600 * k, text="y" * 3000)
        for k in range(7):
            tx_with_turn(tx, T0 + 600 * k, text="z" * 100)
        self.assertLess(tx.L, tx.resident())
        self.assertLess(tx.resident(), tx.H)
        tx.note_prompt(refloop.FULL_DUE_TOKENS - 1)
        self.assertEqual(tx.compaction_slice(T0 + 6000), [])
        tx.note_prompt(refloop.FULL_DUE_TOKENS)
        self.assertTrue(tx.compaction_slice(T0 + 6000))
        # The fixed profiles keep their byte rule only.
        std = refloop.E2Transcript("standard")
        std.note_prompt(10**6)
        self.assertIsNone(std.due_tokens)

    def test_digest_block_keeps_under_its_cap(self):
        tx = refloop.E2Transcript("tight")
        tx.seed([], [(f"day {k}", "d" * 700) for k in range(10)])
        self.assertLessEqual(len(tx.digest_block().encode()), refloop.PROFILES["tight"][2])
        self.assertTrue(tx.archived)

    def test_slice_text_never_carries_results(self):
        tx = refloop.E2Transcript("standard")
        tx_with_turn(
            tx,
            T0,
            results=[("ledger_write", "L1 1234 kcal"), ("web_search", "outside words"), ("read_file", "file body")],
        )
        text = "\n".join(tx.slice_text([1]).values())
        for leak in ("1234", "outside words", "file body"):
            self.assertNotIn(leak, text)
        self.assertIn("[ledger change]", text)


class TranscriptEdges(unittest.TestCase):
    def test_a_write_supersedes_earlier_reads_of_its_path_at_its_turn_close(self):
        tx = refloop.E2Transcript("standard")
        tx.open_turn(T0, "read", ())
        tx.add_assistant([{"type": "tool_use", "id": "a", "name": "read_file", "input": {"path": "p/x"}}])
        tx.add_results([("a", "read_file", {"path": "p/x"}, "short body", False)], T0)
        tx.add_assistant([{"type": "text", "text": "ok"}])
        tx.mark_request(T0 + 1)
        tx.open_turn(T0 + 30, "write", ())
        tx.add_assistant([{"type": "tool_use", "id": "b", "name": "write_file", "input": {"path": "p/x"}}])
        tx.add_results([("b", "write_file", {"path": "p/x"}, "written", False)], T0 + 31)
        self.assertIn("short body", tool_bodies(tx.render_anthropic()))  # still whole inside the writing turn
        tx.add_assistant([{"type": "text", "text": "done"}])
        tx.mark_request(T0 + 32)
        tx.open_turn(T0 + 60, "next", ())
        self.assertTrue(tool_bodies(tx.render_anthropic())[0].startswith("[stub read_file"))
        self.assertEqual(tx.changes.get("superseded"), 1)

    def test_a_ledger_write_supersedes_views_of_that_day_only(self):
        tx = refloop.E2Transcript("standard")
        tx.open_turn(T0, "view", ())
        tx.add_results(
            [
                ("a", "ledger_day", {}, "today rows", False),
                ("b", "ledger_day", {"day": "2026-11-01"}, "old rows", False),
            ],
            T0,
        )
        tx.add_results([("c", "ledger_write", {"ops": [{"op": "add", "row": {}}]}, "added", False)], T0 + 1)
        tx.mark_request(T0 + 2)
        tx.open_turn(T0 + 30, "next", ())
        bodies = tool_bodies(tx.render_anthropic())
        self.assertTrue(bodies[0].startswith("[stub ledger_day"))
        self.assertEqual(bodies[1], "old rows")

    def test_ledger_receipts_stay_whole_to_4096_bytes(self):
        tx = refloop.E2Transcript("standard")
        tx_with_turn(tx, T0, results=[("ledger_day", "r" * 4096), ("read_file", "f" * 2049)])
        tx_with_turn(tx, T0 + 600)
        bodies = tool_bodies(tx.render_anthropic())
        self.assertIn("r" * 4096, bodies)
        self.assertFalse(any(b == "f" * 2049 for b in bodies))

    def test_at_most_four_photos_in_view(self):
        tx = refloop.E2Transcript("standard")
        tx_with_turn(tx, T0, photos=[("image/jpeg", "A", f"a{k}") for k in range(3)])
        tx_with_turn(tx, T0 + 30, photos=[("image/jpeg", "B", f"b{k}") for k in range(3)])
        tx_with_turn(tx, T0 + 60)
        self.assertEqual(sum(1 for m in tx.render_anthropic() for b in m["content"] if b.get("type") == "image"), 4)

    def test_the_ledger_day_turns_at_four(self):
        tx = refloop.E2Transcript("standard")
        night = datetime(2026, 11, 10, 3, 30, tzinfo=TZ).timestamp()
        self.assertEqual(tx._ledger_day(night), "2026-11-09")
        self.assertEqual(tx._ledger_day(night + 3600), "2026-11-10")

    def test_owner_message_never_merges_into_results(self):
        tx = refloop.E2Transcript("standard")
        tx.open_turn(T0, "a", ())
        tx.add_assistant([{"type": "tool_use", "id": "x", "name": "ledger_day", "input": {}}])
        tx.add_results([("x", "ledger_day", {}, "rows", False)], T0)
        tx.mark_request(T0 + 1)
        tx.open_turn(T0 + 30, "b", ())
        msgs = tx.render_anthropic()
        self.assertEqual([b["type"] for b in msgs[-1]["content"]], ["text"])
        self.assertIn("cache_control", msgs[-2]["content"][-1])

    def test_warm_threshold_is_a_head_setting(self):
        tx = refloop.E2Transcript("lean", warm_bulk_max=8_192)
        tx_with_turn(tx, T0, results=[("read_file", "x" * 9000)])
        tx_with_turn(tx, T0 + 30)
        self.assertNotIn("x" * 9000, tool_bodies(tx.render_anthropic()))

    def test_photo_refs_are_unique_per_photo(self):
        refs = {synth.photo("label", i)[2] for i in range(8)}
        self.assertEqual(len(refs), 8)


class Pricing(unittest.TestCase):
    U = {"prompt": 10_000, "cached": 8_000, "written": 1_000, "output": 500}

    def test_cold_reprices_cached_tokens_at_the_write_price(self):
        warm = run_replay.price("claude-sonnet-5-5", self.U, False)
        cold = run_replay.price("claude-sonnet-5-5", self.U, True)
        self.assertAlmostEqual(warm, (1000 * 2 + 8000 * 0.1 + 1000 * 2.5 + 500 * 10) / 1e6)
        self.assertAlmostEqual(cold - warm, 8000 * (2.5 - 0.1) / 1e6)
        self.assertAlmostEqual(run_replay.price("claude-sonnet-5-5", self.U, False, "1h") - warm, 1000 * 1.5 / 1e6)

    def test_checks(self):
        from world import World

        w = World(now=datetime(2026, 11, 10, 12, tzinfo=TZ), fail={}, variant="L1uy")
        self.assertTrue(run_replay.check(w, ("reply", synth.PLANTED_CHECK), "you had 135 g eggs and 35 g salmon", 0))
        self.assertFalse(run_replay.check(w, ("reply", synth.PLANTED_CHECK), "you had eggs", 0))
        self.assertFalse(run_replay.check(w, ("rows_added", 1), "", 0))
        self.assertIsNone(run_replay.check(w, None, "", 0))

    def test_e1_openai_cold_is_the_input_price(self):
        import e1_arm

        u = {"input": 10_000, "cached": 8_000, "cache_write": 0, "output": 0}
        self.assertAlmostEqual(e1_arm.usd("gpt-5.6", u, True), 10_000 * 5.0 / 1e6)
        self.assertAlmostEqual(e1_arm.usd("gpt-5.6", u), (2_000 * 5.0 + 8_000 * 0.5) / 1e6)


class FakeHead:
    """A scripted head: round 1 views the ledger, round 2 answers; usage grows with the prompt."""

    def __init__(self, model):
        self.model = model

    def step(self, system, tools, messages):
        n = len(json.dumps(messages))
        last = messages[-1]["content"][-1]
        if last.get("type") == "tool_result":
            content, calls = [{"type": "text", "text": "Done: 135 g and 35 g."}], []
        else:
            content = [{"type": "tool_use", "id": f"t{n}", "name": "ledger_day", "input": {}}]
            calls = [(f"t{n}", "ledger_day", {})]
        usage = {"prompt": n // 4, "cached": n // 8, "written": n // 16, "output": 50, "thinking": 0}
        return {
            "content": content,
            "calls": calls,
            "text": content[0].get("text", ""),
            "stop": "end_turn",
            "usage": usage,
            "dropped_thinking": 0,
            "s": 1.0,
        }

    def summarize(self, text):
        return (
            "A digest sentence of enough length to be accepted by the rule. " * 3,
            {"prompt": 100, "cached": 0, "written": 0, "output": 60},
            "end_turn",
        )


class ReplayLoop(unittest.TestCase):
    def test_a_scripted_replay_records_every_request_and_prices_the_first_cold(self):
        import argparse

        cfg = argparse.Namespace(
            shape="day-2026-11-04",
            profile="tight",
            model="claude-sonnet-5-5",
            replay_thinking=False,
            limit=6,
            warm=0,
            aged=True,
        )
        orig = run_replay.Head
        run_replay.Head = FakeHead
        try:
            with tempfile.TemporaryDirectory() as d:
                rec = run_replay.Replay(cfg, Path(d) / "run.json").run()
                self.assertEqual(json.loads((Path(d) / "run.json").read_text())["turns"], rec["turns"])
        finally:
            run_replay.Head = orig
        self.assertEqual(len(rec["turns"]), 6)
        self.assertEqual(len(rec["requests"]), 12)  # two rounds a turn
        self.assertTrue(rec["requests"][0]["cold"])
        self.assertFalse(rec["requests"][1]["cold"])
        for t in rec["turns"]:
            mine = [q for q in rec["requests"] if q["turn"] == t["n"]]
            self.assertAlmostEqual(t["usd_5m"], round(sum(q["usd_5m"] for q in mine), 6))
            self.assertGreaterEqual(t["usd_5m"], t["usd_billed"])
        self.assertEqual(rec["turns"][0]["check"], False)  # the scripted head never writes a row


class E1Pricing(unittest.TestCase):
    def test_only_the_first_head_request_of_a_turn_after_a_gap_is_cold(self):
        import e1_arm

        u = {"input": 10_000, "cached": 9_000, "cache_write": 0, "output": 0}

        def req(seq, turn, model="claude-sonnet-5-5"):
            return {"seq": seq, "ctx": {"turn": turn}, "model": model, "usage": u}

        rec = {
            "steps": [{"shape_gap_s": None}, {"shape_gap_s": 60}, {"shape_gap_s": 900}],
            "requests": [req(1, 1, "text-embedding-3-small"), req(2, 1), req(3, 1), req(4, 2), req(5, 3), req(6, 3)],
        }
        e1_arm.price_steps(rec, "claude-sonnet-5-5")
        warm, cold = e1_arm.usd("claude-sonnet-5-5", u), e1_arm.usd("claude-sonnet-5-5", u, True)
        emb = e1_arm.usd("text-embedding-3-small", u)
        self.assertEqual([s["cold_open"] for s in rec["steps"]], [True, False, True])
        self.assertAlmostEqual(rec["steps"][0]["usd_shape"], round(emb + cold + warm, 6))
        self.assertAlmostEqual(rec["steps"][1]["usd_shape"], round(warm, 6))
        self.assertAlmostEqual(rec["steps"][1]["usd_cold"], round(cold, 6))
        self.assertAlmostEqual(rec["steps"][2]["usd_shape"], round(cold + warm, 6))


class Report(unittest.TestCase):
    def test_month_is_blended_cold_and_warm_plus_background(self):
        req = {
            "usage": {"prompt": 100, "cached": 0, "written": 0, "output": 0, "thinking": 0},
            "cold": True,
            "s": 1.0,
            "usd_billed": 0.01,
            "body_bytes": 10,
            "turn": 1,
            "round": 1,
        }
        turns = [
            {
                "n": 1,
                "cold": True,
                "usd_5m": 0.10,
                "usd_1h": 0.1,
                "usd_billed": 0.01,
                "photos": 0,
                "tools": [],
                "seconds": 5,
                "check": True,
                "first_cached_share": 0.0,
            },
            {
                "n": 2,
                "cold": False,
                "usd_5m": 0.02,
                "usd_1h": 0.02,
                "usd_billed": 0.02,
                "photos": 0,
                "tools": ["x"],
                "seconds": 7,
                "check": False,
                "first_cached_share": 0.9,
            },
        ]
        run = {
            "turns": turns,
            "requests": [req, dict(req, turn=2, cold=False)],
            "background": [{"usd": 0.02}],
            "compactions": [],
            "changes": {},
        }
        run["model"] = "claude-sonnet-5-5"
        p = report.e2_profile([run], c5=0.5, per_day=12, c60=0.25)
        self.assertAlmostEqual(p["blended_turn_usd_at_measured_cold_share"], 0.06)
        self.assertEqual(p["month_usd_at_12_turns_a_day"], round(360 * (0.06 + 0.01) + report.OTHER_SERVICES_USD, 2))
        self.assertEqual(p["checks_passed"], {"k": 1, "n": 2})
        # 100 fresh tokens a request: 5m cold = warm = 0.0002; the 1-hour arm the same (no writes)
        self.assertEqual(p["cache_write_ttl"]["usd_per_turn_5m"], 0.0002)

    def test_ttl_decision_prices_cache_writes_by_their_gap_band(self):
        u = {"prompt": 10_000, "cached": 9_000, "written": 1_000, "output": 0}
        mk = lambda n, cold: {"turn": n, "round": 1, "cold": cold, "usage": u}  # noqa: E731
        run = {"model": "claude-sonnet-5-5", "turns": [{"n": 1}, {"n": 2}], "requests": [mk(1, True), mk(2, False)]}
        d = report.ttl_decision([run], c5=0.6, c60=0.3)
        warm5 = (9000 * 0.1 + 1000 * 2.5) / 1e6
        cold5 = 10_000 * 2.5 / 1e6
        warm1, cold1 = (9000 * 0.1 + 1000 * 4.0) / 1e6, 10_000 * 4.0 / 1e6
        self.assertAlmostEqual(d["usd_per_turn_5m"], round(0.6 * cold5 + 0.4 * warm5, 6))
        self.assertAlmostEqual(d["usd_per_turn_1h"], round(0.3 * cold1 + 0.3 * warm1 + 0.4 * warm1, 6))

    def test_corrected_months_scale_the_turn_cost_not_the_other_services(self):
        e2 = {
            p: {"blended_turn_usd_at_measured_cold_share": 0.05, "background_usd_per_turn": 0.0}
            for p in report.PROFILES
        }
        ab = {"standard": {"usd_ratio_new_over_old": 0.9}, "tight": {"usd_ratio_new_over_old": 0.8}}
        c = report.corrected(e2, ab)
        self.assertEqual(
            c["standard"]["month_usd_at_12_turns_a_day"], [round(360 * 0.05 * 0.9 + report.OTHER_SERVICES_USD, 2)]
        )
        self.assertEqual(c["lean"]["usd_ratio_applied"], [0.8, 0.9])
        self.assertIsNone(report.corrected(e2, {"standard": {}}))

    def test_e1_month_carries_the_same_other_services(self):
        step = {
            "kind": "log",
            "usd_cold": 0.1,
            "usd_billed": 0.02,
            "usd_shape": 0.05,
            "head_requests": 2,
            "other_requests": 1,
            "tokens": {"input": 10, "cached": 5},
            "latency_s": 3,
        }
        m = report.e1_model({"steps": [step]}, c5=0.5, per_day=12)
        self.assertEqual(m["month_usd_at_12_turns_a_day"], round(360 * 0.06 + report.OTHER_SERVICES_USD, 2))


if __name__ == "__main__":
    unittest.main(verbosity=1)
