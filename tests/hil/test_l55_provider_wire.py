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

Wave-2 legs (lane K2):

3. CAPTEST at round 6 of an 8-round turn on claude-opus-5 (L19-44): the cap is set
   while round 5 is in flight, so round 6's first attempt is cut; the one retry
   keeps more room and the round finishes there - no ``fabric: switching``.
4. The one per-provider park (L19-8): ``PROVFAULT api.openai.com 429
   credit_balance_exhausted`` refuses the EMBEDDING seam (a turn that recalls or
   writes memory); then a caption request (``VISIONCHECK``) and a spawn send nothing
   to api.openai.com; after a restart the park is still there (kept on flash) and
   the embedding seam still sends nothing.
5. Unsent (L19-14): ``UNSENT api.anthropic.com`` makes the head's first write leave
   0 bytes under ``FAULT sram_ballast``: no host switch, at most one resend, and one
   honest reply naming the device.
6. The reply spill (L19-36): a sub-agent asked for a long report; the whole reply
   lands on the card in the job's project, larger than the 16 KB envelope, with real
   text at offset 20,000, and the envelope's marker names the file.

Markers: ``hil`` + ``net`` + ``agent`` (real provider calls on the head, about
$0.02 on claude-haiku-5-5 for legs 1-2 and 4-5; leg 3 runs claude-opus-5, about
$0.30; leg 6 a claude-haiku-5-5 sub writing a long report, about $0.05). Needs
--allow-hardware, NIMBUS_TEST_IP and NIMBUS_TEST_TOKEN, the serial port of the same
board, an Anthropic head (claude-haiku-5-5) and a second keyed provider in the
priority list; legs 4 and 6 also need an OpenAI key (the embedding provider) and the
SD card.
"""

from __future__ import annotations

import os
import re
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


# ---- wave-2 legs (lane K2) ---------------------------------------------------------

CREDIT_EXHAUSTED_429 = (
    '{"error":{"message":"You have run out of credit.","type":"insufficient_quota","code":"credit_balance_exhausted"}}'
)
# A 1x1 white JPEG: enough for one caption request to be built (or refused).
TINY_JPEG = bytes.fromhex(
    "ffd8ffe000104a46494600010100000100010000ffdb004300080606070605080707070909080a0c140d0c0b0b0c1912130f141d1a"
    "1f1e1d1a1c1c20242e2720222c231c1c2837292c30313434341f27393d38323c2e333432ffc0000b080001000101011100ffc4001f00"
    "00010501010101010100000000000000000102030405060708090a0bffc400b5100002010303020403050504040000017d0102030004"
    "1105122131410613516107227114328191a1082342b1c11552d1f02433627282090a161718191a25262728292a3435363738393a4344"
    "45464748494a535455565758595a636465666768696a737475767778797a838485868788898a92939495969798999aa2a3a4a5a6a7a8"
    "a9aab2b3b4b5b6b7b8b9bac2c3c4c5c6c7c8c9cad2d3d4d5d6d7d8d9dae1e2e3e4e5e6e7e8e9eaf1f2f3f4f5f6f7f8f9faffda000801"
    "0100003f00fbd3ffd9"
)


def _orch_get(ip: str) -> dict:
    return _S.get(f"http://{ip}/api/orch", timeout=10).json()


def _orch_set(ip: str, fields: dict) -> None:
    r = _S.post(f"http://{ip}/api/orch", data=fields, timeout=10)
    assert r.status_code == 200, f"/api/orch {fields}: {r.status_code} {r.text[:200]}"


def _verb_or_skip(device, line: str, expect: str) -> str:
    try:
        reply = device.cmd(line, expect, timeout=8)
    except Exception:  # noqa: BLE001 - an unknown verb answers "ERR unknown", never the expect
        pytest.skip(f"test console has no {line.split()[0]} verb (flash this lane's [env:test] build)")
    return reply


def test_captest_at_round_six_retries_with_more_room_on_opus(board, device):
    """L19-44 on claude-opus-5: round 6's first attempt is cut on demand; its one retry
    has more room and finishes the round on the same host."""
    ip = board
    model = os.environ.get("NIMBUS_L55_CAP_MODEL", "claude-opus-5")
    before = _orch_get(ip)
    _orch_set(ip, {"orchM_anthropic": model, "loopRounds": "10"})
    try:
        device.drain(quiet=0.3)
        r = _S.post(
            f"http://{ip}/api/chat",
            data={
                "text": "Use your tools one call per step, in order, and do not batch them: search your "
                "memory for bike, then scooter, then train, then bus, then ferry, then tram, then taxi, "
                "then list my files. After all eight, answer with one short sentence."
            },
            timeout=10,
        )
        turn = str(r.json().get("turn") or "")
        assert turn, r.text[:200]
        lines, armed, released = [], False, False
        deadline = time.time() + 600
        while time.time() < deadline:
            chunk = _serial_lines(device, 1.0)
            lines += chunk
            text = "\n".join(chunk)
            if not armed and "headloop round=4" in text:
                device.send(f"CAPTEST {CUT_CAP}")  # round 5 is in flight: round 6 gets the cap
                armed = True
            if armed and not released and "one retry with" in text:
                device.send("CAPTEST 0")
                released = True
            j = _S.get(f"http://{ip}/api/chat", params={"turn": turn}, timeout=10).json()
            if j.get("done"):
                lines += _serial_lines(device, 3.0)
                break
        log = "\n".join(lines)
    finally:
        device.cmd("CAPTEST 0", "CAPTEST", timeout=5)
        _orch_set(
            ip,
            {
                "orchM_anthropic": before.get("orchM_anthropic", "claude-haiku-5-5"),
                "loopRounds": str(before.get("loopRounds", 6)),
            },
        )
    print("\n".join(ln for ln in log.splitlines() if "headloop round=" in ln or "retry" in ln or "fabric" in ln))
    assert armed, "the turn never reached round 5 (headloop round=4): raise loopRounds or reword the turn"
    m = re.search(rf"cut at {CUT_CAP} tokens - one retry with (\d+)", log)
    assert m, "round 6's capped first attempt was not retried once with more room"
    assert int(m.group(1)) > CUT_CAP
    assert "fabric: switching" not in log, "a cut answer must not fail over"
    assert "cut at the output limit" not in log, "the retry with more room must finish the round"


def test_one_park_closes_every_seam_and_survives_a_restart(board, device):
    """L19-8: a billing refusal learned on the EMBEDDING seam closes openai for the
    caption step and for spawns (no request to api.openai.com), and is still there after
    a restart (the park lives on flash)."""
    ip = board
    _verb_or_skip(device, "VISIONCHECK /nonexistent.jpg", "VISIONCHECK described=")
    before = _orch_get(ip)
    device.cmd(f"PROVFAULT api.openai.com 429 {CREDIT_EXHAUSTED_429}", "PROVFAULT", timeout=5)
    try:
        # 1. The embedding seam meets the refusal (the turn's memory write embeds).
        _, log1 = _turn(device, ip, "Remember this for later: my bike is a blue Brompton. Reply with one word.")
        assert "park: openai parked (billing, HTTP 429) from embeddings" in log1, log1[-2000:]
        # 2. The caption step: nothing to openai, it walks on or says why.
        from tg_archive_bench import fsput

        fsput(device, TINY_JPEG, "/k2_park.jpg")
        device.drain(quiet=0.3)
        device.send("VISIONCHECK /k2_park.jpg")
        vision = "\n".join(_serial_lines(device, 60.0))
        assert "VISIONCHECK described=" in vision
        assert (
            "vision: skipped a closed provider (openai says the account is out of credit" in vision
            or "vision: no provider open - openai says the account is out of credit" in vision
        ), vision[-1500:]
        assert "TEST override answered api.openai.com" not in vision
        # 3. A spawn: the sub-priority puts openai first, the spawn runs elsewhere.
        _orch_set(ip, {"subPrio": "openai,anthropic,mistral"})
        _, log3 = _turn(
            device,
            ip,
            "Start one background helper to find the capital of France and report "
            "back in one sentence. Then tell me it started.",
        )
        assert re.search(r"spawned job\d+ -> (anthropic|mistral):", log3), log3[-2000:]
        assert "TEST override answered api.openai.com" not in log3, "a closed provider got a request"
    finally:
        device.cmd("PROVFAULT off", "PROVFAULT", timeout=5)
        _orch_set(ip, {"subPrio": before.get("subPrio", "anthropic,openai,mistral")})
    # 4. A restart: the override is gone (RAM), the park is not (flash).
    device.reboot_and_confirm()
    require_orchestrator(_S, ip, "L55")
    _, log4 = _turn(device, ip, "What did I tell you about my bike? One sentence.")
    assert "park: openai closed for embeddings" in log4, "the park did not survive the restart: " + log4[-2000:]


def test_unsent_first_write_no_switch_one_honest_reply(board, device):
    """L19-14: the head's first write leaves 0 bytes (UNSENT) with internal memory
    pinned (FAULT sram_ballast): no host switch, at most one resend, one reply."""
    ip = board
    _verb_or_skip(device, "UNSENT off", "UNSENT off")
    m = device.cmd_re("FAULT sram_ballast 0", r"intFree=(\d+) intLargest=(\d+)", timeout=5)
    free = int(m.group(1))
    ballast = max(0, free - 16500)  # total free stays above the old 16 KB threshold
    device.cmd(f"FAULT sram_ballast {ballast}", "FAULT sram_ballast=", timeout=5)
    device.cmd("UNSENT api.anthropic.com", "UNSENT api.anthropic.com armed", timeout=5)
    try:
        reply, log = _turn(device, ip, "What is two plus two? One short sentence.", timeout_s=120)
    finally:
        device.cmd("UNSENT off", "UNSENT off", timeout=5)
        device.cmd("FAULT sram_ballast 0", "FAULT sram_ballast=", timeout=5)
    print(f"reply: {reply!r}")
    attempts = log.count("TEST override: request to api.anthropic.com")
    assert 1 <= attempts <= 2, f"{attempts} attempts (at most one resend)"
    assert "fabric: switching" not in log, "a request the device could not send must not switch host"
    assert "could not finish sending its request" in reply
    assert "switching to" not in reply


def test_long_sub_reply_is_saved_whole(board, device):
    """L19-36: a sub-agent's long report is saved whole in its project (bigger than the
    16 KB envelope), with real text at offset 20,000, and the head is told where."""
    ip = board
    project = "k2bench"
    reply, log = _turn(
        device,
        ip,
        f"Start one background helper in project {project} named longreport: ask it to write a "
        "detailed 5,000-word report on the history of the bicycle, in plain paragraphs, and "
        "return the whole report as its reply. Tell me when it has started.",
        timeout_s=180,
    )
    assert re.search(r"spawned job\d+", log), log[-2000:]
    deadline = time.time() + 900
    saved = None
    while time.time() < deadline and not saved:
        lines = "\n".join(_serial_lines(device, 10.0))
        saved = re.search(r"reply was (\d+) B - saved whole as (\S+)", lines)
    assert saved, "the long reply was never spilled (it fit the envelope, or the job did not finish)"
    size, path = int(saved.group(1)), saved.group(2)
    assert size > 16384
    proj, name = path.split("/", 1)
    r = _S.get(f"http://{ip}/api/files/dl", params={"project": proj, "name": name}, timeout=30)
    assert r.status_code == 200 and len(r.content) >= size - 64, (r.status_code, len(r.content))
    tail = r.content[20000:20200].decode("utf-8", "replace")
    assert tail.strip() and "[cut at 16 KB" not in tail, tail
