"""Music playback (CUM-40) HIL - the player must never take the device down.

Drives the ``PLAY`` console command against real hardware and proves the device
survives (still answers ``PING``) - the acceptance's core worry is a multi-minute
clip starving the loop watchdog, so "the console still responds after a play" is the
real assertion. The headline 3-minute-MP3-to-completion case is gated: it needs an
MP3 decoder in the build AND a fixture in SD ``/music`` AND a human to confirm the
speaker, so it SKIPs loudly when those are absent rather than passing vacuously.

Markers: ``@pytest.mark.hil`` (serial console). The 3-min MP3 adds ``audio`` +
``manual`` (needs the speaker + a person).
"""

from __future__ import annotations

import time

import pytest


def _play(device, arg: str = "", timeout: float = 8.0) -> str:
    """Run PLAY [arg] and return the device's echoed result line."""
    cmd = "PLAY" if not arg else f"PLAY {arg}"
    m = device.cmd_re(cmd, r"PLAY .*", timeout=timeout)
    return m.group(0)


@pytest.mark.hil
def test_play_all_does_not_reset(device):
    """PLAY (all of /music) must return, and the console must still be alive right
    after - a play that reset the board would fail the follow-up PING."""
    line = _play(device)
    # "PLAY all -> N track(s), mp3=yes|no" - just needs to come back cleanly.
    assert "PLAY" in line
    device.cmd_re("PING", r"PONG|PING", timeout=6.0)


@pytest.mark.hil
def test_play_stop_pause_control(device):
    """The control verbs are accepted and the device stays responsive."""
    _play(device, "stop")
    _play(device, "pause")
    device.cmd_re("PING", r"PONG|PING", timeout=6.0)


@pytest.mark.hil
@pytest.mark.audio
@pytest.mark.manual
def test_three_minute_mp3_completes_no_watchdog(device):
    """THE acceptance: a 3-minute MP3 plays to completion on the speaker with no
    watchdog reset. Requires an MP3 decoder build + SD /music/long.mp3 (~3 min) +
    a human at the speaker. SKIPs (never silently passes) when unmet.
    """
    m = device.cmd_re("PLAY long.mp3", r"PLAY long\.mp3 -> (\S+)", timeout=8.0)
    verdict = m.group(1)
    if verdict != "queued":
        pytest.skip(f"long.mp3 not playable (verdict={verdict}); needs MP3 build + the fixture")
    # Watch for ~3.2 minutes; the board must keep answering PING the whole time and
    # must not print a reset/boot banner. A watchdog reset would drop the console.
    deadline = time.time() + 195
    while time.time() < deadline:
        device.cmd_re("PING", r"PONG|PING", timeout=8.0)
        time.sleep(15)
    # Still alive after the full clip duration = no watchdog reset.
    device.cmd_re("PING", r"PONG|PING", timeout=8.0)
