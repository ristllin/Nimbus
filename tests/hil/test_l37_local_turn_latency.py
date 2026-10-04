"""L37 - a local turn never waits on the Telegram long-poll (CUM-462).

On-device voice, web and serial turns reach the orchestrator through one inbound
queue that the tg_poll task drains once per loop cycle. With a bot token that task
spends almost all of its time blocked in the getUpdates long-poll (30 s), and that
wait never looked at the queue: a hold-to-talk transcript could sit there ~30 s
before its turn started. The fix (nimbus/net/tg_poll_sched.h, host-tested in
test/test_tg_poll_sched) cuts an idle long-poll short for a waiting local turn,
ends the pauses early, and skips one poll at most (the next cycle checks Telegram
first) - still one task, one TLS session.

The automatable class (asserted here over serial)
-------------------------------------------------
With the DEDICATED test bot provisioned and its long-poll running, ``VOICE <text>``
injects a turn on the voice channel exactly as a hold-to-talk release does. When
tg_poll takes it, the firmware logs ``telegram: local turn from voice waited <N> ms``.
N must stay under LOCAL_TURN_MAX_WAIT_MS wherever the inject lands in the poll
cycle (before the fix it reached ~30000). The injects are spread over the cycle,
and at least one must land mid-long-poll and cut it short. ``getUpdates 409`` must
never appear: cutting a long-poll short must not look like a second device polling
the bot. No provider key is needed: the turn's own outcome is not under test.

What stays a human leg
----------------------
A real hold-to-talk turn on a token device (the turn starts within about a second
of "Thinking"), and a Telegram message sent right after it still being answered -
the manual test at the bottom.

Gated ``@pytest.mark.hil`` + ``net``. The log-line parser is host-tested in
test_l37_local_turn_latency_host.py.
"""

from __future__ import annotations

import re
import time

import pytest

from device import ExpectTimeout

# The issue's bench bar is "within about 1 s". The rule's own bound is far tighter
# (one 50 ms pause slice, or a poll reconnect already under way); the margin
# covers a reconnect on a slow link.
LOCAL_TURN_MAX_WAIT_MS = 1500

# Seconds to wait before each inject, measured from the previous turn's drain: they
# land early, mid and late in the 30 s long-poll (and one inside the poll cycle's
# reconnect window right after a turn).
INJECT_DELAYS_S = (1.0, 6.0, 14.0, 23.0)

WAITED_RE = re.compile(r"telegram: local turn from (?P<src>\S+) waited (?P<ms>\d+) ms")


def parse_waited(line: str) -> tuple[str, int]:
    """``telegram: local turn from voice waited 12 ms`` -> ("voice", 12). Raises on
    anything else, so a renamed log line fails loud instead of passing vacuously."""
    m = WAITED_RE.search(line)
    if not m:
        raise ValueError(f"no local-turn wait in: {line!r}")
    return m.group("src"), int(m.group("ms"))


@pytest.fixture
def token_device(device, net, secrets, require_secret):
    """Orchestrator mode on Wi-Fi with the DEDICATED test bot polling, set through
    the web API exactly as the owner sets it (a live token swap, no reboot). The
    token is cleared afterwards, so the unit never keeps polling the shared test bot
    (a second unit polling it would draw the 409 this test forbids)."""
    require_secret(secrets.require_test_telegram)
    require_secret(secrets.require_sta)
    device.ensure_mode(1)
    st = device.status()
    if st.group("wifi") == "1":
        net.ip = st.group("ip")
    else:
        net.provision(secrets.sta_ssid, secrets.sta_pass)
        net.wait_got_ip(timeout=25.0)
    r = net.post("/api/orch", {"tgToken": secrets.tg_test_token, "tgAllow": secrets.tg_test_chat}, timeout=8.0)
    assert r.status_code == 200, f"setting the test bot token failed: HTTP {r.status_code}"
    try:
        # Drained at the poll loop top: within a second with no token, or once a
        # previous bot's long-poll returns (up to ~30 s).
        device.expect("telegram: token swapped live", timeout=45.0)
        time.sleep(5.0)  # the new bot's first long-poll is connected and waiting
        yield device
    finally:
        net.post("/api/orch", {"clr_tgToken": "1"}, timeout=8.0)


def _next_line(device, deadline: float, seen: list) -> str | None:
    """The next serial line (recorded in ``seen``), or None once ``deadline`` passes."""
    left = deadline - time.time()
    if left <= 0:
        return None
    try:
        line = device.expect_re(r".+", timeout=left).group(0)
    except ExpectTimeout:
        return None
    seen.append(line)
    return line


def _await_wait_line(device, timeout: float, seen: list) -> tuple[str, int]:
    deadline = time.time() + timeout
    while (line := _next_line(device, deadline, seen)) is not None:
        if WAITED_RE.search(line):
            return parse_waited(line)
    pytest.fail(f"tg_poll never took the inject within {timeout} s (no 'waited' line); tail: {seen[-15:]}")


@pytest.mark.hil
@pytest.mark.net
def test_voice_inject_never_waits_on_the_long_poll(token_device):
    device = token_device
    seen: list = []  # every serial line this test read, for the 409 / cut-short checks
    waits = []
    for i, delay in enumerate(INJECT_DELAYS_S):
        time.sleep(delay)
        device.send(f"VOICE latency probe {i}")
        src, ms = _await_wait_line(device, 45.0, seen)
        assert src == "voice", f"expected the voice channel, got {src!r}"
        waits.append(ms)
        assert ms < LOCAL_TURN_MAX_WAIT_MS, (
            f"inject {i} (after {delay} s) waited {ms} ms for tg_poll - the long-poll held it (CUM-462); all waits: {waits}"
        )
    # The poll after the last cut-short one must come back clean too.
    deadline = time.time() + 10.0
    while _next_line(device, deadline, seen) is not None:
        pass
    assert not [ln for ln in seen if "getUpdates 409" in ln], "a cut-short long-poll drew a 409 Conflict"
    assert any("long-poll cut short for a local turn" in ln for ln in seen), (
        f"no inject landed mid-long-poll, so the cut-short path went unexercised; waits: {waits}"
    )


@pytest.mark.hil
@pytest.mark.net
@pytest.mark.manual
def test_hold_to_talk_turn_starts_promptly_with_a_token(token_device, require_manual):
    """Owner leg: a real spoken turn with Telegram polling, then Telegram itself."""
    require_manual.confirm(
        "LOCAL TURN LATENCY (owner leg, Telegram bot configured):\n"
        "  1. Leave the device idle for 20 s. Hold the mic, say 'what time is it', release.\n"
        "     Confirm: 'Thinking' appears and the reply follows as fast as the provider\n"
        "     answers - no ~30 s pause before anything happens.\n"
        "  2. Within 5 s of the reply, send the bot a Telegram message.\n"
        "     Confirm: the bot answers it (Telegram is not starved).\n"
        "  3. Repeat step 1 three times in a row, then once more.\n"
        "     Confirm: no 'another device appears to be using this same Telegram bot'\n"
        "     message ever arrives.\n"
        "Did every step behave as described?"
    )
