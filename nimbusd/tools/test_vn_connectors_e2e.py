#!/usr/bin/env python3
"""Offline tests for vn_connectors_e2e.py's rate-limit handling (CUM-465).

The e2e retries a turn the provider rate-limited. It recognizes the engine's 429
replies by their copy, so the copy and the matcher are pinned together here: the
reply and slug tables are READ from lib/harness/src/rate_limit.cpp, and every
window's reply must map to its own slug. A copy change, or a new window with no
matcher entry, fails this test instead of silently disabling the retry (the old
"rate-limited" match went dead exactly that way).

Run: python3 nimbusd/tools/test_vn_connectors_e2e.py  (part of `make test`)
"""

import pathlib
import re
import sys
import unittest
from unittest import mock

HERE = pathlib.Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import vn_connectors_e2e as vn  # noqa: E402

RATE_LIMIT_CPP = HERE.parent.parent / "lib" / "harness" / "src" / "rate_limit.cpp"
STRINGS = r'((?:"(?:[^"\\]|\\.)*"\s*)+)'


def function_body(src, signature):
    start = src.index(signature)
    return src[start : src.index("\n}\n", start)]


def returns_by_kind(body):
    """{RateLimit kind: returned C string} for one switch-on-kind function; the
    trailing return after the switch is the Unknown (fallback) case."""
    out = {}
    for m in re.finditer(r"(?:case RateLimit::(\w+):\s*)?return\s+" + STRINGS + ";", body):
        out[m.group(1) or "Unknown"] = "".join(re.findall(r'"((?:[^"\\]|\\.)*)"', m.group(2)))
    return out


def engine_tables():
    src = RATE_LIMIT_CPP.read_text()
    replies = returns_by_kind(function_body(src, "const char* rateLimitReply(RateLimit k)"))
    slugs = returns_by_kind(function_body(src, "const char* rateLimitSlug(RateLimit k)"))
    return replies, slugs


class ReplyTableTest(unittest.TestCase):
    def setUp(self):
        self.replies, self.slugs = engine_tables()

    def test_tables_parsed(self):
        # A refactor that breaks the parse must fail loudly, not pass on nothing.
        self.assertGreaterEqual(len(self.replies), 6, self.replies)
        self.assertIn("Unknown", self.replies)
        self.assertEqual(set(self.replies), set(self.slugs))

    def test_every_window_reply_maps_to_its_slug(self):
        for kind, reply in self.replies.items():
            with self.subTest(kind=kind):
                self.assertEqual(vn.rate_limit_window(reply), self.slugs[kind], reply)

    def test_other_failures_are_not_rate_limits(self):
        for text in (
            "That didn't finish - the provider had a server error. Nothing is still running; ask again to retry.",
            "Low on working memory right now, so that couldn't finish. Nothing is still running; "
            "try again in a few seconds.",
            "Couldn't start that agent on mistral.",
            "On it.",
            "You have a dentist appointment at 3pm and a daily limit review at 5pm.",
        ):
            with self.subTest(text=text):
                self.assertIsNone(vn.rate_limit_window(text))


class RetryTest(unittest.TestCase):
    def setUp(self):
        self.replies, self.slugs = engine_tables()

    def run_attempts(self, *turns):
        """ask_with_retry over scripted turns (each a list of reply texts)."""
        script = [
            ([{"seq": i, "t": 0, "text": t} for i, t in enumerate(turn)], f"turn={n}") for n, turn in enumerate(turns)
        ]
        with mock.patch.object(vn, "ask", side_effect=script), mock.patch.object(vn.time, "sleep") as sleep:
            attempts = vn.ask_with_retry(inst=None, text="q", secs=1, settle=7)
        return attempts, sleep

    def test_per_minute_limit_retries_once(self):
        attempts, sleep = self.run_attempts([self.replies["PerMinute"]], ["Here are your events."])
        self.assertEqual(len(attempts), 2)
        self.assertEqual(attempts[0]["rate_limit"], ["minute"])
        sleep.assert_called_once_with(7)

    def test_unnamed_limit_retries_once(self):
        attempts, _ = self.run_attempts([self.replies["Unknown"]], ["Here are your events."])
        self.assertEqual(len(attempts), 2)

    def test_windows_that_cannot_reopen_do_not_retry(self):
        for kind in ("DailyUtc", "Daily", "NotAllowed", "Quota"):
            with self.subTest(kind=kind):
                attempts, sleep = self.run_attempts([self.replies[kind]])
                self.assertEqual(len(attempts), 1)
                self.assertEqual(attempts[0]["rate_limit"], [self.slugs[kind]])
                sleep.assert_not_called()

    def test_rate_limited_synthesis_with_fresh_results_does_not_retry(self):
        attempts, _ = self.run_attempts(["On it.", "[FRESH RESULTS] 2 events", self.replies["PerMinute"]])
        self.assertEqual(len(attempts), 1)

    def test_a_clean_answer_does_not_retry(self):
        attempts, sleep = self.run_attempts(["Here are your events."])
        self.assertEqual(len(attempts), 1)
        self.assertEqual(attempts[0]["rate_limit"], [])
        sleep.assert_not_called()


if __name__ == "__main__":
    unittest.main(verbosity=2)
