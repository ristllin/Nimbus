#!/usr/bin/env python3
"""Nightly eval entry point: run only CHANGED suites, budget-capped, then report.

Meant to be cron-able (or a scheduled job on the private repo). It:

  1. registers the known suites for every provider whose key env var is set
     (a suite whose key is absent is skipped cleanly - the T6 rule: each SKIPS
     unless its key is present);
  2. runs a suite only when its inputs hash differs from its last recorded run
     (change detection in tools/eval_harness), unless --force;
  3. caps spend: each suite has its own budget, and --max-usd is a global ceiling
     across the whole run;
  4. regenerates the markdown trend report over the accumulated ledgers.

Default is a real run of changed suites. Use --dry-run to print the plan and spend
nothing (safe to wire up before trusting the schedule). Secrets are read from the
environment (repo-root .env) and never printed.

Examples:
  # what WOULD run tonight, no calls, no spend:
  tools/eval_nightly.py --dry-run
  # the actual nightly (only changed suites, <= $5 total):
  tools/eval_nightly.py --max-usd 5
  # force every suite regardless of change detection:
  tools/eval_nightly.py --force --max-usd 5

Cron (example, 03:17 local; keys sourced from the repo .env):
  17 3 * * *  cd /path/to/nimbus && set -a && . ../cumulo-nimbus/.env && set +a \
              && python3 tools/eval_nightly.py --max-usd 5 >> ~/nimbus-evals/nightly.log 2>&1
"""

import argparse
import os
import sys

_HERE = os.path.dirname(os.path.abspath(__file__))
if _HERE not in sys.path:
    sys.path.insert(0, _HERE)
import eval_harness as H  # noqa: E402
import eval_prompt_ab as ab  # noqa: E402

# Which env var gates each provider. A suite for a provider whose var is unset is
# skipped (never a hard error) so an unattended nightly degrades cleanly.
PROVIDER_KEY_ENV = {"anthropic": "ANTHROPIC_API_KEY", "openai": "OPENAI_API_KEY", "mistral": "MISTRAL_API_KEY"}


def register_known_suites():
    """Register every suite whose provider key is present. Returns (registered, skipped)."""
    registered, skipped = [], []
    for provider in ("anthropic", "openai"):
        env = PROVIDER_KEY_ENV[provider]
        if os.environ.get(env):
            registered.append(ab.build_n11_suite(provider))
        else:
            skipped.append((f"n11_prompt_ab_{provider}", f"{env} not set"))
    return registered, skipped


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--reps", type=int, default=2, help="reps per case (aggregate reads less noisy)")
    ap.add_argument("--force", action="store_true", help="run every suite even if inputs are unchanged")
    ap.add_argument("--dry-run", action="store_true", help="print the plan, make no calls, spend nothing")
    ap.add_argument("--max-usd", type=float, default=5.0, help="global spend ceiling across all suites")
    ap.add_argument("--trend-out", default=os.path.join(H.EVALS_DIR, "trend.md"), help="trend report path")
    args = ap.parse_args(argv)

    registered, skipped = register_known_suites()
    for sid, why in skipped:
        print(f"[skip] {sid}: {why}")
    if not registered:
        print("no suites to run (no provider keys set). Trend report still refreshed.")

    spent = 0.0
    ran, no_change = [], []
    for suite in registered:
        # Trim this suite's budget to whatever global headroom is left.
        remaining = max(0.0, args.max_usd - spent)
        if remaining <= 0 and not args.dry_run:
            print(f"[budget] global ${args.max_usd} reached; not starting {suite.id}")
            break
        suite.budget.max_usd = min(suite.budget.max_usd, remaining) if not args.dry_run else suite.budget.max_usd
        if not args.force and not args.dry_run and not H.suite_changed(suite):
            print(f"[unchanged] {suite.id}: inputs hash matches last run; skipping")
            no_change.append(suite.id)
            continue
        summary = H.run_suite(suite, reps=args.reps, force=args.force, dry_run=args.dry_run)
        if not args.dry_run and not summary.get("skipped"):
            spent += summary.get("cost_usd", 0.0)
            ran.append(suite.id)

    md = H.trend_markdown()
    os.makedirs(H.EVALS_DIR, exist_ok=True)
    with open(os.path.expanduser(args.trend_out), "w", encoding="utf-8") as f:
        f.write(md)
    print(f"\nran: {ran or '-'}   unchanged: {no_change or '-'}   spent: ${spent:.4f} / ${args.max_usd}")
    print(f"trend report -> {args.trend_out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
