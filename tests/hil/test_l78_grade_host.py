"""Host tests for L78's grading (test_l78_owner_first.py), no board.

Leg b is calibrated to an 85,000 B request body and pins it on the unit from v5.1.2 (the
head's own budget is larger): every refit must name the pinned figure. Leg c's notice
pattern covers what replaced v5.1.1's pause notices (lane F: a failed cycle in the log, the
daily alert), and never an ordinary fold line.
"""

from __future__ import annotations

import os
import sys

import pytest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import test_l78_owner_first as l78  # noqa: E402

pytestmark = pytest.mark.host

REPLY = [{"text": "The last word is QUOKKA."}]
REFIT = "headloop: round 7 body 86541 B over the {b} B budget - folded 7966 B of the replayed tool rounds"
BODY = "transport: heaptrace api.mistral.ai/v1/chat/completions body={n} pre=1/1 fail=none af=0"


def test_leg_b_passes_when_every_refit_names_the_pinned_budget():
    rec = l78._grade_b(REPLY, [BODY.format(n=84990), REFIT.format(b=85000), BODY.format(n=78885)], 61.0)
    assert rec["grade"] == "PASS" and rec["refits_freed"] == [7966] and rec["largest_body"] == 84990


def test_leg_b_fails_when_a_refit_names_the_heads_own_budget():
    # The pin did not hold: the loop refitted against the head's own 360,000 B.
    rec = l78._grade_b(REPLY, [REFIT.format(b=360000)], 61.0)
    assert rec["grade"] == "FAIL" and not rec["checks"]["every refit names the pinned budget"]


def test_leg_b_fails_on_a_body_over_the_pin_and_is_blocked_with_no_refit():
    over = l78._grade_b(REPLY, [BODY.format(n=120000), REFIT.format(b=85000)], 61.0)
    assert over["grade"] == "FAIL" and not over["checks"]["no body over the budget sent"]
    assert l78._grade_b(REPLY, [BODY.format(n=60000)], 61.0)["grade"] == "BLOCKED"


def test_the_notice_pattern_covers_the_old_and_the_new_copy_only():
    told = [
        "Automatic summaries are paused for this chat because the conversation grew too long again.",
        "fold(web): cycle failed, next try 2026-11-18 00:48 (1 failed cycles in a row)",
        "⚠ This chat's summary hasn't updated since 2026-11-02. It keeps retrying on its own; send /compact to try now.",
    ]
    assert all(l78.PAUSED_RE.search(t) for t in told)
    fine = [
        "fold(web): ok on openai (3487 in / 577 out, gpt-5.6-luna)",
        "fold(web): rung 1/5 mistral/mistral-small-latest failed -> empty (the fold came back with no text)",
        "✓ Compacted. Summary updated: covered 42 messages from 2026-11-14 to 2026-11-16.",
        "fold: chatsum.txt 4231 B saved in 414 ms",
    ]
    assert not any(l78.PAUSED_RE.search(t) for t in fine)
