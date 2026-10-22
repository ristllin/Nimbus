"""§L55 - the provider wire on real hardware: a cut answer and a billing refusal.

Two bench legs for the provider-wire rules the host suites pin
(test_provider_class, test_harness_wire_anthropic):

1. A cut answer is retried ONCE in the same step with more room, never failed
   over and never resent identically: the test console caps every request's
   FIRST attempt at a few tokens (``CAPTEST <tokens>`` ->
   ``agent::providers::setTestFirstAttemptCap``), a multi-round tool turn runs on
   the real head, and /api/log must show the retry and no provider switch.
2. A billing refusal parks the host: the test console makes the head's API host
   answer HTTP 400 "credit balance is too low" without sending anything
   (``PROVFAULT <api-host> <status> <json>`` ->
   ``agent::providers::setTestStatusOverride``). The first turn switches once and
   parks the host; the second turn starts on the next host with NO request to
   the parked one (no per-turn retries) and no second notice.

Both console verbs are TEST-build only; a board whose console does not know them
skips loudly (the verbs ride the test console, which lane N owns).

Each turn is read to ITS OWN answer (``GET /api/chat?turn=<id>``), never from the
legacy single slot, which still holds the previous reply while the next message
waits; and the log facts come from the serial console, because the /api/log ring
holds about 1 KB, too little for one turn (the v4.5.14 release run read turn 1's
tail as turn 2 both ways). ``CAPTEST 0`` must end the cap, or every later turn and
file runs cut.

Markers: ``hil`` + ``net`` + ``agent`` (real provider calls on the head, about
$0.02 on claude-haiku-5-5). Needs --allow-hardware, NIMBUS_TEST_IP and
NIMBUS_TEST_TOKEN, the serial port of the same board, an Anthropic head
(claude-haiku-5-5) and a second keyed provider in the priority list.
"""

from __future__ import annotations

import time

import pytest

from device import ExpectTimeout
from webrig import ip_tok_or_skip, make_session, require_orchestrator

pytestmark = [pytest.mark.hil, pytest.mark.net, pytest.mark.agent]

CREDIT_400 = (
    '{"type":"error","error":{"type":"invalid_request_error","message":"Your credit balance is too '
    'low to access the Anthropic API. Please go to Plans & Billing to upgrade or purchase credits."}}'
)

_S = None
CUT_CAP = 128  # leg 1's first-attempt cap (output tokens)


def _serial_lines(device, window_s: float) -> list:
    """Every console line printed in the next ``window_s`` seconds."""
    lines = []
    end = time.time() + window_s
    while time.time() < end:
        try:
            lines.append(device.expect_re(r".", timeout=max(0.2, end - time.time())).string)
        except ExpectTimeout:
            break
    return lines


def _turn(device, ip: str, text: str, timeout_s: int = 150):
    """Run one web-chat turn to its own answer; return (reply, the console lines it printed)."""
    device.drain(quiet=0.3)
    r = _S.post(f"http://{ip}/api/chat", data={"text": text}, timeout=10)
    assert r.status_code == 200, f"chat send {r.status_code}: {r.text[:200]}"
    turn = str(r.json().get("turn") or "")
    assert turn, f"/api/chat gave no turn id: {r.text[:200]}"
    lines = []
    deadline = time.time() + timeout_s
    while time.time() < deadline:
        lines += _serial_lines(device, 2.0)
        j = _S.get(f"http://{ip}/api/chat", params={"turn": turn}, timeout=10).json()
        assert not j.get("gone"), f"turn {turn} is gone before it was answered"
        if j.get("done"):
            lines += _serial_lines(device, 3.0)  # the turn's last lines, still in the buffer
            return str(j.get("reply") or ""), "\n".join(lines)
    pytest.fail(f"turn {turn} was not answered within {timeout_s}s: {text[:60]}")


@pytest.fixture(scope="module")
def board(device):
    global _S
    ip, tok = ip_tok_or_skip("L55")
    _S = make_session(tok)
    require_orchestrator(_S, ip, "L55")
    try:
        reply = device.cmd("CAPTEST 0", "CAPTEST", timeout=5)
    except Exception:  # noqa: BLE001 - an unknown verb never echoes it
        reply = "unknown"
    if "unknown" in reply.lower():
        pytest.skip("test console has no CAPTEST/PROVFAULT verbs (flash the [env:test] build with them)")
    yield ip
    try:
        device.cmd("CAPTEST 0", "CAPTEST", timeout=5)
        device.cmd("PROVFAULT off", "PROVFAULT", timeout=5)
    except Exception:  # noqa: BLE001 - restore best-effort; a leak only lasts until reboot
        pass


def test_captest_zero_ends_the_cap(board, device):
    """CAPTEST 0 ends the cap (it was refused with a usage line, so the cap leaked
    into every later turn and file until a restart)."""
    assert device.cmd(f"CAPTEST {CUT_CAP}", "CAPTEST", timeout=5).strip() == f"CAPTEST {CUT_CAP}"
    assert device.cmd("CAPTEST 0", "CAPTEST", timeout=5).strip() == "CAPTEST off"


def test_cut_answer_retried_with_headroom_no_failover(board, device):
    ip = board
    device.cmd(f"CAPTEST {CUT_CAP}", "CAPTEST", timeout=5)
    try:
        reply, log = _turn(
            device,
            ip,
            "Do these one at a time with your tools: search your memory for my bike, then list my "
            "files, then search memory for my scooter, then write a 200-word note about bikes as "
            "your reply.",
        )
    finally:
        cap_off = device.cmd("CAPTEST 0", "CAPTEST", timeout=5).strip()
    assert cap_off == "CAPTEST off", f"CAPTEST 0 did not end the cap: {cap_off!r}"
    assert reply, "the cut turn produced no reply"
    assert "one retry with" in log, "a cut first attempt must be retried once with headroom"
    assert "fabric: switching" not in log, "a cut answer must not fail over"
    assert "cut at the output limit" not in log, "the headroom retry must finish the round"


def test_billing_refusal_parks_the_host(board, device):
    ip = board
    device.cmd(f"PROVFAULT api.anthropic.com 400 {CREDIT_400}", "PROVFAULT", timeout=5)
    try:
        first, log1 = _turn(device, ip, "What is two plus two? Answer in one short sentence.")
        second, log2 = _turn(device, ip, "And three plus three? One short sentence.")
    finally:
        device.cmd("PROVFAULT off", "PROVFAULT", timeout=5)
    keys = ("park", "switch", "override", "low memory", "turn end")
    for n, (reply, log) in enumerate(((first, log1), (second, log2)), 1):
        print(f"turn {n} reply: {reply[:300]!r}")
        print("\n".join(ln for ln in log.splitlines() if any(k in ln for k in keys)))
    assert first and second, "both turns must be answered by the next provider"
    assert f"at {CUT_CAP} tokens" not in log1 + log2, "leg 1's CAPTEST cap leaked into this leg"
    # Turn 1: one refused request, no same-host retry, a park, one switch.
    assert log1.count("TEST override answered api.anthropic.com") == 1, log1[-2000:]
    assert "parked (billing, HTTP 400)" in log1
    assert log1.count("fabric: switching anthropic") == 1
    # Turn 2: started past the parked head - no request to it, no new switch.
    assert "is parked (billing) - starting on" in log2
    assert "TEST override answered api.anthropic.com" not in log2, "a parked host got a request"
    assert "fabric: switching anthropic" not in log2
