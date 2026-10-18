"""Tests for spend_ledger.py and the recorded usage fixtures it prices.

Stdlib unittest (no pytest needed): `make -C tools/harness-lab check` runs it.
"""

import contextlib
import io
import json
import sys
import tempfile
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))

import spend_ledger as sl  # noqa: E402


def run(argv):
    """spend_ledger.main with its report swallowed (the battery log stays readable)."""
    with contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(io.StringIO()):
        return sl.main(argv)


FIXTURES = HERE.parent.parent / "test/support/fixtures/usage_2026-10"
LANE_CAPS = "anthropic=0.5,openai=0.05,mistral=0.1,zai=0.1"


def usage_row(host, model, usage, ts="2026-10-18T00:00:00Z"):
    return json.dumps({"ts": ts, "host": host, "path": "/v1/x", "status": 200, "model": model, "usage": usage})


class Pricing(unittest.TestCase):
    def test_haiku55_prices_each_token_class(self):
        usd, t = sl.cost(
            "anthropic",
            "claude-haiku-5-5",
            {
                "input_tokens": 1_000_000,
                "output_tokens": 1_000_000,
                "cache_read_input_tokens": 0,
                "cache_creation_input_tokens": 0,
            },
        )
        # 1M in + 1M out crosses the 100K long-context line, so the long rates apply.
        self.assertAlmostEqual(usd, 0.50 + 2.50)
        usd, _ = sl.cost(
            "anthropic",
            "claude-haiku-5-5",
            {
                "input_tokens": 10_000,
                "output_tokens": 1_000,
                "cache_read_input_tokens": 50_000,
                "cache_creation_input_tokens": 20_000,
            },
        )
        self.assertAlmostEqual(usd, (10_000 * 0.10 + 1_000 * 0.50 + 50_000 * 0.01 + 20_000 * 0.125) / 1e6)

    def test_openai_input_includes_cached_and_written(self):
        # Recorded shape (gpt-5.6-luna): input 6806 = 6684 written + 0 read + 122 fresh.
        t = sl.tokens(
            "openai",
            {
                "input_tokens": 6806,
                "output_tokens": 43,
                "input_tokens_details": {"cache_write_tokens": 6684, "cached_tokens": 0},
            },
        )
        self.assertEqual(t, {"in": 122, "out": 43, "cache_read": 0, "cache_write": 6684})

    def test_chat_completions_cached_tokens(self):
        t = sl.tokens(
            "mistral",
            {"prompt_tokens": 8827, "completion_tokens": 49, "prompt_tokens_details": {"cached_tokens": 8752}},
        )
        self.assertEqual(t, {"in": 75, "out": 49, "cache_read": 8752, "cache_write": 0})

    def test_unknown_model_fails_closed(self):
        self.assertEqual(sl.rates("anthropic", "claude-unknown-9"), sl.FALLBACK["anthropic"])
        self.assertGreaterEqual(sl.rates("openai", "gpt-9")[0], max(r[1] for r in sl.PRICES["openai"]))

    def test_longest_prefix_wins(self):
        self.assertEqual(sl.rates("openai", "gpt-5.6-luna")[0], 0.20)
        self.assertEqual(sl.rates("openai", "gpt-5.6-sol")[0], 5.0)


class Fixtures(unittest.TestCase):
    """The recorded usage objects the metering lanes test against."""

    def test_every_fixture_prices_and_has_usage(self):
        files = sorted(FIXTURES.glob("*_*.json"))
        self.assertGreaterEqual(len(files), 4)
        for f in files:
            doc = json.loads(f.read_text())
            self.assertTrue(doc["usage"], f.name)
            for u in doc["usage"]:
                usd, _ = sl.cost(doc["provider"], doc["served"], u)
                self.assertGreaterEqual(usd, 0.0, f.name)

    def test_required_providers_recorded(self):
        names = {f.name for f in FIXTURES.glob("*_*.json")}
        for need in (
            "anthropic_claude-haiku-5-5.json",
            "mistral_mistral-small-latest.json",
            "zai_glm-5.3-flash.json",
            "openai_gpt-5.6-sol.json",
        ):
            self.assertIn(need, names)

    def test_openai_fixture_carries_cache_write_tokens(self):
        for name in ("openai_gpt-5.6-sol.json", "openai_gpt-5.6-luna.json"):
            for u in json.loads((FIXTURES / name).read_text())["usage"]:
                self.assertIn("cache_write_tokens", u["input_tokens_details"], name)
        luna = json.loads((FIXTURES / "openai_gpt-5.6-luna.json").read_text())["usage"]
        self.assertTrue(any(u["input_tokens_details"]["cache_write_tokens"] > 0 for u in luna))

    def test_no_secret_shaped_strings(self):
        for f in FIXTURES.glob("*.json"):
            text = f.read_text()
            for needle in ("sk-ant", "sk-proj", "Bearer ", "req_0"):
                self.assertNotIn(needle, text, f.name)


class Ledger(unittest.TestCase):
    def setUp(self):
        self.tmp = Path(tempfile.mkdtemp())
        self.ledger = self.tmp / "ledger.jsonl"
        self.usage = self.tmp / "usage.jsonl"

    def write_usage(self, rows):
        self.usage.write_text("\n".join(rows) + "\n")

    def test_add_is_idempotent_and_totals(self):
        self.write_usage(
            [usage_row("api.mistral.ai", "mistral-small-latest", {"prompt_tokens": 100_000, "completion_tokens": 0})]
        )
        argv = ["add", "--ledger", str(self.ledger), "--lane", "N", "--caps", LANE_CAPS, str(self.usage)]
        self.assertEqual(run(argv), 0)
        self.assertEqual(run(argv), 0)  # same file twice: no double count
        spent, _ = sl.totals(sl.read_ledger(self.ledger))
        self.assertAlmostEqual(spent["mistral"], 0.02)

    def test_over_cap_routes_to_anthropic(self):
        self.write_usage([usage_row("api.z.ai", "glm-5.3", {"prompt_tokens": 100_000, "completion_tokens": 0})])
        rc = run(["add", "--ledger", str(self.ledger), "--lane", "N", "--caps", LANE_CAPS, str(self.usage)])
        self.assertEqual(rc, 3)  # 0.14 > the 0.10 lane cap
        caps = sl.parse_caps(LANE_CAPS)
        self.assertEqual(sl.route("zai", sl.read_ledger(self.ledger), caps), "anthropic")
        self.assertEqual(sl.route("mistral", sl.read_ledger(self.ledger), caps), "mistral")

    def test_refusal_routes_to_anthropic(self):
        run(["refuse", "openai", "--ledger", str(self.ledger), "--why", "insufficient_quota"])
        self.assertEqual(sl.route("openai", sl.read_ledger(self.ledger), sl.parse_caps(None)), "anthropic")

    def test_no_provider_left(self):
        run(["refuse", "anthropic", "--ledger", str(self.ledger), "--why", "credit"])
        run(["refuse", "mistral", "--ledger", str(self.ledger), "--why", "credit"])
        self.assertIsNone(sl.route("mistral", sl.read_ledger(self.ledger), sl.parse_caps(None)))
        self.assertEqual(run(["route", "mistral", "--ledger", str(self.ledger)]), 3)

    def test_estimate_counts_against_the_cap(self):
        caps = sl.parse_caps(LANE_CAPS)
        self.assertEqual(sl.route("openai", [], caps, est=0.04), "openai")
        self.assertEqual(sl.route("openai", [], caps, est=0.06), "anthropic")

    def test_bad_caps_are_refused(self):
        self.assertEqual(run(["status", "--ledger", str(self.ledger), "--caps", "nope=1"]), 2)


if __name__ == "__main__":
    unittest.main()
