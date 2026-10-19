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

Markers: ``hil`` + ``net`` + ``agent`` (real provider calls on the head, about
$0.02 on claude-haiku-5-5). Needs --allow-hardware, NIMBUS_TEST_IP and
NIMBUS_TEST_TOKEN, the serial port of the same board, an Anthropic head
(claude-haiku-5-5) and a second keyed provider in the priority list.
"""

from __future__ import annotations

import time

import pytest

from webrig import ip_tok_or_skip, make_session, require_orchestrator

pytestmark = [pytest.mark.hil, pytest.mark.net, pytest.mark.agent]

CREDIT_400 = (
    '{"type":"error","error":{"type":"invalid_request_error","message":"Your credit balance is too '
    'low to access the Anthropic API. Please go to Plans & Billing to upgrade or purchase credits."}}'
)

_S = None


def _chat(ip: str, text: str, timeout_s: int = 150) -> str:
    url = f"http://{ip}/api/chat"
    r = _S.post(url, data={"text": text}, timeout=10)
    assert r.status_code == 200, f"chat send {r.status_code}: {r.text[:200]}"
    deadline = time.time() + timeout_s
    while time.time() < deadline:
        time.sleep(3)
        j = _S.get(url, timeout=10).json()
        if not j.get("pending", True) and j.get("reply"):
            return j["reply"]
    pytest.fail(f"no chat reply within {timeout_s}s for: {text[:60]}")


def _log(ip: str) -> str:
    return _S.get(f"http://{ip}/api/log", timeout=10).text


def _log_since(ip: str, before: str) -> str:
    now = _log(ip)
    # The log is a ring: keep what was added after the snapshot's last line.
    tail = before.strip().splitlines()[-1] if before.strip() else ""
    at = now.rfind(tail) if tail else -1
    return now[at + len(tail) :] if at >= 0 else now


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
        device.send("CAPTEST 0")
        device.send("PROVFAULT off")
    except Exception:  # noqa: BLE001 - restore best-effort; a leak only lasts until reboot
        pass


def test_cut_answer_retried_with_headroom_no_failover(board, device):
    ip = board
    before = _log(ip)
    device.cmd("CAPTEST 128", "CAPTEST", timeout=5)
    try:
        reply = _chat(
            ip,
            "Do these one at a time with your tools: search your memory for my bike, then list my "
            "files, then search memory for my scooter, then write a 200-word note about bikes as "
            "your reply.",
        )
    finally:
        device.cmd("CAPTEST 0", "CAPTEST", timeout=5)
    log = _log_since(ip, before)
    assert reply, "the cut turn produced no reply"
    assert "one retry with" in log, "a cut first attempt must be retried once with headroom"
    assert "fabric: switching" not in log, "a cut answer must not fail over"
    assert "cut at the output limit" not in log, "the headroom retry must finish the round"


def test_billing_refusal_parks_the_host(board, device):
    ip = board
    before = _log(ip)
    device.cmd(f"PROVFAULT api.anthropic.com 400 {CREDIT_400}", "PROVFAULT", timeout=5)
    try:
        first = _chat(ip, "What is two plus two? Answer in one short sentence.")
        log1 = _log_since(ip, before)
        mid = _log(ip)
        second = _chat(ip, "And three plus three? One short sentence.")
        log2 = _log_since(ip, mid)
    finally:
        device.cmd("PROVFAULT off", "PROVFAULT", timeout=5)
    assert first and second, "both turns must be answered by the next provider"
    # Turn 1: one refused request, no same-host retry, a park, one switch.
    assert log1.count("TEST override answered api.anthropic.com") == 1, log1[-2000:]
    assert "parked (billing, HTTP 400)" in log1
    assert log1.count("fabric: switching anthropic") == 1
    # Turn 2: started past the parked head - no request to it, no new switch.
    assert "is parked (billing) - starting on" in log2
    assert "TEST override answered api.anthropic.com" not in log2, "a parked host got a request"
    assert "fabric: switching anthropic" not in log2
