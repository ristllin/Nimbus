#!/usr/bin/env python3
"""Offline tests for metering_check.py (run by `make check` once the lab has its Python tests)."""

import unittest

import metering_check as mc


def usage(host, path, **u):
    return {"host": host, "path": path, "usage": u}


class LedgerDelta(unittest.TestCase):
    def test_anthropic_cache_counts_are_outside_input(self):
        d = mc.ledger_delta(
            "anthropic",
            {
                "input_tokens": 816,
                "output_tokens": 250,
                "cache_read_input_tokens": 9743,
                "cache_creation_input_tokens": 120,
            },
        )
        self.assertEqual(d, {"in": 816, "out": 250, "cr": 9743, "cw": 120})

    def test_openai_cache_counts_are_split_out_of_input(self):
        d = mc.ledger_delta(
            "openai",
            {
                "input_tokens": 1000,
                "output_tokens": 5,
                "input_tokens_details": {"cached_tokens": 600, "cache_write_tokens": 300},
            },
        )
        self.assertEqual(d, {"in": 100, "out": 5, "cr": 600, "cw": 300})

    def test_openai_malformed_cache_counts_never_go_negative(self):
        d = mc.ledger_delta(
            "openai",
            {
                "input_tokens": 10,
                "output_tokens": 1,
                "input_tokens_details": {"cached_tokens": 9744, "cache_write_tokens": 5},
            },
        )
        self.assertEqual(d, {"in": 0, "out": 1, "cr": 10, "cw": 0})

    def test_other_providers_are_not_split(self):
        d = mc.ledger_delta(
            "mistral", {"prompt_tokens": 900, "completion_tokens": 40, "prompt_tokens_details": {"cached_tokens": 800}}
        )
        self.assertEqual(d, {"in": 900, "out": 40, "cr": 0, "cw": 0})


class LedgerLog(unittest.TestCase):
    def test_rows_are_read_from_the_lab_ledger_lines_only(self):
        log = (
            "    ok   something\n"
            'lab-ledger: {"host":"anthropic","model":"claude-haiku-5-5","tag":"turn",'
            '"in":1,"out":2,"cr":3,"cw":4}\n'
            "    ...  ledger row: not json\n"
        )
        rows = mc.ledger_rows_from_log(log)
        self.assertEqual(len(rows), 1)
        self.assertEqual(rows[0]["cw"], 4)


class Compare(unittest.TestCase):
    USAGE = [
        usage(
            "api.anthropic.com",
            "/v1/messages",
            input_tokens=800,
            output_tokens=60,
            cache_read_input_tokens=0,
            cache_creation_input_tokens=9000,
        ),
        usage("api.mistral.ai", "/v1/chat/completions", prompt_tokens=9900, completion_tokens=30),
        usage("api.mistral.ai", "/v1/embeddings", prompt_tokens=12),  # not a chat request
    ]

    def test_switched_turn_matches_per_host(self):
        ledger = [
            {"host": "anthropic", "model": "claude-haiku-5-5", "in": 800, "out": 60, "cr": 0, "cw": 9000},
            {"host": "mistral", "model": "mistral-small-latest", "in": 9900, "out": 30, "cr": 0, "cw": 0},
        ]
        ok, lines = mc.compare(self.USAGE, ledger)
        self.assertTrue(ok, lines)

    def test_spend_filed_under_the_last_host_is_caught(self):
        # Counter-test: the pre-fix engine filed the whole turn under the last host.
        ledger = [{"host": "mistral", "model": "mistral-small-latest", "in": 10700, "out": 90, "cr": 0, "cw": 9000}]
        ok, _ = mc.compare(self.USAGE, ledger)
        self.assertFalse(ok)

    def test_a_failed_turn_that_metered_nothing_is_caught(self):
        ok, _ = mc.compare(self.USAGE[:1], [])
        self.assertFalse(ok)

    def test_nothing_recorded_is_not_a_pass(self):
        ok, _ = mc.compare([], [])
        self.assertFalse(ok)


if __name__ == "__main__":
    unittest.main()
