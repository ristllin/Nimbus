"""§L31 - HIL ring lights-out after a turn (CUM-221 regression).

The recurring owner bug: "Nimbus stays with the ring on after it answers Telegram."
A turn paints the orchestrator's blue "head" arc (and any sub-agent arcs) while it
works; when the reply is delivered every one of those arcs must collapse and the
ring must go fully dark. It has stranded lit 5 times because the terminating edges
run on the tg_poll task while the always-alive main-loop watchdog only aged out
*attention* arcs - a terminal Done ember had no main-loop backstop (see
``Router::forceExpireDoneArcs``, lib/core/src/attention.cpp).

This asserts the observable invariant on real hardware: after a completed turn the
ring returns to ``dark`` within a bounded window, and the belt-and-braces backstop
counter stays 0 (the PRIMARY path cleared the arc, not a watchdog - the CUM-11
acceptance metric, extended to CUM-221).

The turn is driven over the serial ``TURN`` seam, which funnels through the exact
same core as a Telegram message (``orchestrator::handleMessage`` -> ``TurnGuard``
head arc -> ``orchEventSink`` -> the ``g_router`` job table -> ``refreshRing``); the
Telegram *wire* round-trip itself is covered by test_l5_agent::test_telegram_roundtrip
(F18). Provider keys are pushed to the device over serial, never used host-side.

Gated: ``@pytest.mark.agent`` (needs a provider key + network for a real reply); the
backstop-metric leg adds ``@pytest.mark.net``. conftest loud-skips without
``--allow-hardware``; ``require_secret`` turns a missing key into a reasoned skip.
"""

from __future__ import annotations

import re
import time

import pytest

from device import ExpectTimeout

# How long after the reply the ring is allowed to still show a working arc. The
# primary path clears it in ~a poll cycle; this is far below the watchdog cap
# (AttnHoldMs + 60 s ~ 6 min) so a pass proves the PRIMARY edge did the work.
RING_CLEAR_WINDOW_S = 25.0


def _push_key(device, nvs_key: str, value: str) -> None:
    """Push a provider key into device NVS over serial (never logged)."""
    device.send(f"SET {nvs_key}={value}")
    try:
        device.expect(f"SET {nvs_key} ok=", timeout=3.0)
    except ExpectTimeout:
        pass  # not every build acks; the real proof is the reply downstream


def _provision(device, secrets) -> None:
    if secrets.openai_key:
        _push_key(device, "oaiKey", secrets.openai_key)
    if secrets.anthropic_key:
        _push_key(device, "antKey", secrets.anthropic_key)
    device.wifi(secrets.sta_ssid, secrets.sta_pass)
    device.expect_re(r"WIFI_GOT_IP\s+\d+\.\d+\.\d+\.\d+", timeout=25.0)


def _wait_ring_dark(device, window: float) -> "tuple[bool, object]":
    """Poll RENDER? until the composed ring is dark (no head/job arc lit), or the
    window elapses. Returns (dark, last_render)."""
    deadline = time.time() + window
    last = None
    while time.time() < deadline:
        last = device.render()
        if last.ring == "dark":
            return True, last
        time.sleep(1.0)
    return False, last


@pytest.mark.agent
def test_ring_returns_to_dark_after_a_turn(device, secrets, require_secret):
    """After a turn's reply is delivered, the ring collapses to fully dark within the
    window - the direct regression for the 'ring stays on after answering' bug.

    Also reads LEDSTATE (the PHYSICAL ring layer) to prove the driver is not still
    pushing a lit working frame after the composed intent went dark (the audit's
    GAP 5: a stranded ember keeps the raw frame lit)."""
    require_secret(secrets.require_provider_keys)
    device.ensure_mode(1)  # Orchestrator
    _provision(device, secrets)

    device.turn("Reply with exactly the single word: pong")
    try:
        device.expect("ORCH REPLY [serial]:", timeout=45.0)
    except ExpectTimeout:
        pytest.fail("no ORCH REPLY within 45 s - the turn never round-tripped (F17); cannot assess the ring")

    dark, last = _wait_ring_dark(device, RING_CLEAR_WINDOW_S)
    assert dark, (
        f"ring never returned to dark within {RING_CLEAR_WINDOW_S:.0f}s of the reply - "
        f"a working/head arc stranded lit (CUM-221). last RENDER? = {last!r}"
    )

    # Physical layer: the raw frame must not still be driving a lit ring.
    led = device.cmd("LEDSTATE", "LEDSTATE ", timeout=4.0)
    m = re.search(r"rgb=(\d+),(\d+),(\d+)\s+bright=(\d+)", led)
    assert m, f"LEDSTATE gave nothing parseable: {led!r}"
    r, g, b, bright = (int(m.group(i)) for i in range(1, 5))
    assert bright == 0 or (r == 0 and g == 0 and b == 0), (
        f"composed ring is dark but the physical layer is still lit: {led!r} (CUM-221 GAP 5)"
    )


@pytest.mark.agent
@pytest.mark.net
def test_ring_backstop_counter_is_zero_on_a_healthy_turn(device, secrets, require_secret, net):
    """The blue arc must be cleared by the PRIMARY path, never a belt-and-braces
    watchdog. ``ringBackstopFires`` in /api/state counts every backstop fire
    (attention watchdog, stuck-turn reaper, working-ceiling, and the CUM-221 Done
    reaper); a healthy turn keeps it 0 (CUM-11/CUM-221 acceptance metric)."""
    require_secret(secrets.require_provider_keys)
    device.ensure_mode(1)
    _provision(device, secrets)
    ip = net.wait_got_ip(timeout=25.0)

    before = net.get_json("/api/state", ip=ip).get("ringBackstopFires", 0)
    device.turn("Reply with exactly the single word: pong")
    try:
        device.expect("ORCH REPLY [serial]:", timeout=45.0)
    except ExpectTimeout:
        pytest.fail("no ORCH REPLY within 45 s - cannot assess the backstop counter")
    _wait_ring_dark(device, RING_CLEAR_WINDOW_S)

    after = net.get_json("/api/state", ip=ip).get("ringBackstopFires", 0)
    assert after == before, (
        f"ringBackstopFires rose {before}->{after} across a healthy turn - a backstop "
        f"cleared the arc instead of the primary path (CUM-11/CUM-221)"
    )
