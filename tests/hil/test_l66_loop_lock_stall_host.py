"""Host tests for the L66 loop-lock stall parsers and verdicts (no board).

The LOOPGUARD? line is the test console's (src/test_console.cpp) and the crash lines
are the 22 Oct 2026 shapes the owner's device wrote after its v4.5.15 update (the
formatters are pinned by test/test_crash_forensics and test/test_loop_guard), so a
renamed field fails here instead of passing a bench run vacuously.
"""

from __future__ import annotations

import json

import pytest

from test_l39_crash_forensics import boot_report, parse_log, split_boots
from test_l66_loop_lock_stall import (
    HOLD_LINE_RE,
    LONG_HOLD_MS,
    SLOW_PASS_MS,
    STALL_MS,
    STALL_TOOL,
    loop_verdict,
    lumi_signature_verdict,
    parse_loopguard,
    tool_round,
)

pytestmark = pytest.mark.host

BEFORE = (
    "LOOPGUARD blocking=0 caller=0x00000000 busy=0 parked=0 dropped=0 longHolds=0 "
    "longestHoldMs=12 tool=memory.search maxPassMs=140 slowPasses=0"
)
AFTER = (
    "LOOPGUARD blocking=0 caller=0x00000000 busy=731 parked=0 dropped=0 longHolds=1 "
    "longestHoldMs=15032 tool=memory.scratchpad maxPassMs=180 slowPasses=0"
)

# The owner's device, boot n=1 of v4.5.15 reading its last v4.5.13 dump, re-shaped as a
# crash boot (this-reset=1, the reset reason of the crash itself).
LUMI_SHAPE = [
    "0 167 boot boot: n=9 fw=v4.5.15 build=v4.5.15 reason=crash(watchdog)(0x0c) wake=none mode=orch t=1792672051 prev=turn:loop/r1",
    "5 1005 boot crash: fp=3a68814b new=1 this-reset=1 task=IDLE1 pc=0x4037f97f cause=1070313136(other) "
    "vaddr=0x00000000 size=54820/65536 elf=4a7e68837 fw-match=1",
    "6 1007 boot crash-bt: depth=4 corrupt=0 0x4037f97f,0x4226c501,0x40385187,0x40384015",
    "7 1009 boot crash-why: Task watchdog got triggered. The following tasks/users did not reset the watchdog "
    "in time:  - loopTask (CPU 1)",
]


def _rep(lines):
    return boot_report(split_boots(parse_log("\n".join(lines)))[-1])


def test_parse_loopguard_reads_every_field():
    g = parse_loopguard("[12:00] " + AFTER)
    assert g == {
        "blocking": 0,
        "caller": "00000000",
        "busy": 731,
        "parked": 0,
        "dropped": 0,
        "longHolds": 1,
        "longestHoldMs": 15032,
        "tool": "memory.scratchpad",
        "maxPassMs": 180,
        "slowPasses": 0,
    }
    assert parse_loopguard("LOOPGUARD busy=1") is None
    assert parse_loopguard("") is None


def test_loop_verdict_passes_the_fixed_shape():
    ok, why = loop_verdict(parse_loopguard(BEFORE), parse_loopguard(AFTER), STALL_MS)
    assert ok, why


@pytest.mark.parametrize(
    "field,value,needle",
    [
        ("blocking", 1, "waited for the card lock"),
        ("busy", 0, "no busy skip"),
        ("maxPassMs", SLOW_PASS_MS, "loop stalled"),
        ("longHolds", 0, "not recorded"),
        ("longestHoldMs", STALL_MS - 1, "longest hold"),
        ("tool", "memory.search", "longest hold"),
    ],
)
def test_loop_verdict_fails_each_broken_counter(field, value, needle):
    after = parse_loopguard(AFTER)
    after[field] = value
    ok, why = loop_verdict(parse_loopguard(BEFORE), after, STALL_MS)
    assert not ok and needle in why


def test_lumi_signature_matches_the_owner_dump():
    ok, why = lumi_signature_verdict(_rep(LUMI_SHAPE))
    assert ok, why


@pytest.mark.parametrize(
    "index,old,new,needle",
    [
        (0, "crash(watchdog)", "crash(panic)", "boot reason"),
        (1, "this-reset=1", "this-reset=0", "this-reset"),
        (3, "loopTask (CPU 1)", "IDLE0 (CPU 0)", "does not name loopTask"),
    ],
)
def test_lumi_signature_rejects_other_crashes(index, old, new, needle):
    lines = list(LUMI_SHAPE)
    lines[index] = lines[index].replace(old, new)
    ok, why = lumi_signature_verdict(_rep(lines))
    assert not ok and needle in why


def test_tool_round_asks_for_the_stall_tool_only():
    body = json.loads(tool_round("model-x"))
    assert body["model"] == "model-x"
    calls = [c for c in body["content"] if c["type"] == "tool_use"]
    assert [c["name"] for c in calls] == [STALL_TOOL.replace(".", "_")]
    assert all(c["name"] != "orch_turn" for c in calls)  # never finishes the turn itself
    assert body["stop_reason"] == "tool_use"


def test_hold_line_shape_and_bars():
    m = HOLD_LINE_RE.search("12 3456 - memory: tool memory.scratchpad held the card lock for 15032 ms")
    assert m and m.group("tool") == STALL_TOOL and int(m.group("ms")) == 15032
    assert STALL_MS > 8000 + 2 * LONG_HOLD_MS  # past the watchdog, well past the hold bar
