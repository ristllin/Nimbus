"""Host tests for the L68 web-stall parsers and verdicts (no board).

The WEBGUARD? line is the test console's (src/test_console.cpp) and the crash lines
are the shapes the owner's device wrote on v4.6.0 after its 26 Oct 2026 17:19:29Z
reset (errlog boot n=5), so a renamed field fails here instead of passing a bench run
vacuously.
"""

from __future__ import annotations

import json

import pytest

from test_l39_crash_forensics import boot_report, parse_log, split_boots
from test_l68_web_stall import (
    ANSWER_MS,
    HOLD_LINE_RE,
    HOLD_MS,
    LOOK_TOOL,
    async_tcp_signature_verdict,
    boot_report_from_state,
    look_fallback,
    parse_webguard,
    photo_ref,
    state_verdict,
    tool_round,
    web_guard_verdict,
)

pytestmark = pytest.mark.host

BEFORE = (
    "WEBGUARD blocking=0 caller=0x00000000 busy=0 netUnderLock=0 netCaller=0x00000000 releases=0 webTask=1 watched=1"
)
AFTER = (
    "WEBGUARD blocking=0 caller=0x00000000 busy=27 netUnderLock=0 netCaller=0x00000000 releases=2 webTask=1 watched=1"
)

# The owner's device, boot n=5 of v4.6.0 (errlog-crash-boot5.txt, the crash lines).
LUMI_SHAPE = [
    "0 98 boot boot: n=5 fw=v4.6.0 build=v4.6.0 reason=crash(watchdog)(0x0c) wake=none mode=orch t=1793035169 "
    "ota=ok_v4.6.0 prev=n4/8616s/turn-message/h9444",
    "5 940 boot crash: fp=fdf034fa new=1 this-reset=1 task=IDLE0 pc=0x4037fa8f cause=1070311600(other) "
    "vaddr=0x00000000 size=56452/65536 elf=e604f0296 fw-match=1",
    "6 942 boot crash-bt: depth=4 corrupt=0 0x4037fa8f,0x4231dd3d,0x40385297,0x40384125",
    "7 943 boot crash-why: Task watchdog got triggered. The following tasks/users did not reset the watchdog "
    "in time:  - async_tcp (CPU 0)",
]


def _rep(lines):
    return boot_report(split_boots(parse_log("\n".join(lines)))[-1])


def _polls(n, ms=180, busy_at=()):
    return [{"t": i, "status": 200, "ms": ms, "cardBusy": i in busy_at} for i in range(n)]


def test_parse_webguard_reads_every_field():
    g = parse_webguard("[20:00] " + AFTER)
    assert g == {
        "blocking": 0,
        "caller": "00000000",
        "busy": 27,
        "netUnderLock": 0,
        "netCaller": "00000000",
        "releases": 2,
        "webTask": 1,
        "watched": 1,
    }
    assert parse_webguard("WEBGUARD busy=1") is None
    assert parse_webguard("") is None


def test_web_guard_verdict_passes_the_fixed_shape():
    ok, why = web_guard_verdict(parse_webguard(BEFORE), parse_webguard(AFTER), expect_busy=True)
    assert ok, why
    ok, why = web_guard_verdict(parse_webguard(BEFORE), parse_webguard(BEFORE), expect_busy=False)
    assert ok, why


@pytest.mark.parametrize(
    "field,value,needle",
    [
        ("blocking", 1, "waited for the card lock"),
        ("netUnderLock", 1, "network request started under the card lock"),
        ("busy", 0, "no busy answer"),
        ("webTask", 0, "not recorded or not watched"),
        ("watched", 0, "not recorded or not watched"),
    ],
)
def test_web_guard_verdict_fails_each_broken_counter(field, value, needle):
    after = parse_webguard(AFTER)
    after[field] = value
    ok, why = web_guard_verdict(parse_webguard(BEFORE), after, expect_busy=True)
    assert not ok and needle in why


def test_state_verdict_holds_every_poll_to_the_bar():
    ok, why = state_verdict(_polls(40, busy_at={5, 6}), expect_busy=True)
    assert ok, why
    ok, why = state_verdict(_polls(40), expect_busy=False)  # the image.look leg: never busy
    assert ok, why
    late = _polls(40, busy_at={5})
    late[7]["ms"] = ANSWER_MS + 1
    assert not state_verdict(late, expect_busy=True)[0]
    dead = _polls(40, busy_at={5})
    dead[9] = {"t": 9, "status": 0, "ms": 5001, "error": "ReadTimeout"}  # the 26 Oct shape
    ok, why = state_verdict(dead, expect_busy=True)
    assert not ok and "late or failed" in why
    ok, why = state_verdict(_polls(40), expect_busy=True)
    assert not ok and "cardBusy" in why
    assert not state_verdict([], expect_busy=False)[0]


def test_the_owner_signature_matches_the_26_oct_record():
    ok, why = async_tcp_signature_verdict(_rep(LUMI_SHAPE))
    assert ok, why


@pytest.mark.parametrize(
    "index,old,new,needle",
    [
        (0, "crash(watchdog)", "crash(panic)", "boot reason"),
        (1, "this-reset=1", "this-reset=0", "this-reset"),
        (3, "async_tcp (CPU 0)", "loopTask (CPU 1)", "does not name async_tcp"),
    ],
)
def test_the_owner_signature_rejects_other_crashes(index, old, new, needle):
    lines = list(LUMI_SHAPE)
    lines[index] = lines[index].replace(old, new)
    ok, why = async_tcp_signature_verdict(_rep(lines))
    assert not ok and needle in why


def test_tool_round_asks_for_one_tool_only():
    body = json.loads(tool_round(LOOK_TOOL, {"ref": "0123456789abcdef", "question": "q"}, model="model-x"))
    assert body["model"] == "model-x"
    calls = [c for c in body["content"] if c["type"] == "tool_use"]
    assert [c["name"] for c in calls] == ["image_look"]
    assert calls[0]["input"]["ref"] == "0123456789abcdef"
    assert body["stop_reason"] == "tool_use"  # never finishes the turn itself


def test_photo_ref_is_sixteen_hex():
    ref = photo_ref(b"\xff\xd8\xff photo bytes")
    assert len(ref) == 16 and all(c in "0123456789abcdef" for c in ref)
    assert ref == photo_ref(b"\xff\xd8\xff photo bytes")


def test_look_fallback_prefers_a_keyed_cheap_captioner():
    assert look_fallback({"mistral": {"hasKey": True}, "openai": {"hasKey": True}}) == "mistral"
    assert look_fallback({"mistral": {"hasKey": False}, "openai": {"hasKey": True}}) == "openai"
    assert look_fallback({"anthropic": {"hasKey": True}}) is None
    assert look_fallback({}) is None


def test_hold_line_shape_and_the_hold_length():
    m = HOLD_LINE_RE.search("12 3456 - memory: tool image.look held the card lock for 120477 ms")
    assert m and m.group("tool") == LOOK_TOOL and int(m.group("ms")) == 120477
    assert HOLD_MS > 8000 * 3  # several watchdog windows of polls inside one hold


def test_the_running_boot_record_reads_the_signature():
    state = {
        "boot": {
            "n": 77,
            "reason": "crash(watchdog)",
            "crash": {
                "thisReset": True,
                "task": "IDLE1",
                "why": "Task watchdog got triggered. ... - async_tcp (CPU 1)",
            },
        }
    }
    ok, why = async_tcp_signature_verdict(boot_report_from_state(state))
    assert ok, why
    state["boot"]["crash"]["thisReset"] = False  # an older crash's record kept across a boot
    assert not async_tcp_signature_verdict(boot_report_from_state(state))[0]
    assert not async_tcp_signature_verdict(boot_report_from_state({"boot": {"reason": "usb", "crash": None}}))[0]
