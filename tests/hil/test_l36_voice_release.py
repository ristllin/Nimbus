"""L36 - hold-to-talk release on real hardware (CUM-456).

The owner held the mic on an all-in-one with no joinable Wi-Fi: on RELEASE the ring
stayed on the listening color for ~30 s (the speech-to-text call ran first and sat
out its connect timeouts), then the screen said "Didn't catch that" although the
real failure was the network. The fix orders the STATE updates around the (still
single-task) network call: processing is shown first, a missing link fails at once.

The automatable class (asserted here over serial)
-------------------------------------------------
``TAP <x> <y> HOLD`` presses the on-screen mic; the release watcher reads the real
touch driver, so the capture ends at its ~0.9 s floor (the "release"). ``VOICE?``
then reports the flow and the release timings the firmware measured itself:

  * OFFLINE (Wi-Fi not joined): the flow ends on the ``no_network`` outcome, the
    speech-to-text call never started (``sttMs=0``), processing was on screen
    within 200 ms of the release, and the line is the network one - never
    "Didn't catch that".
  * ONLINE (Wi-Fi joined, placeholder key): processing is on screen before the
    speech-to-text call starts, and the provider's HTTP 401 for the placeholder
    reads as a named provider error. No paid call: the key is refused.
  * A tap clears an outcome line, and the flow ends idle (no lit cue outlives it).

What stays a human leg
----------------------
That the ring and screen LOOK right (the spinner sweeps, the alert color reads),
and a real spoken turn online with a real key - the manual test at the bottom.

Gated ``@pytest.mark.hil``. The pure ``VOICE?`` parser is host-tested in
test_l36_voice_release_host.py.
"""

from __future__ import annotations

import re
import time

import pytest

# The contract from the issue: release -> the ring leaves the recording state in
# under 200 ms regardless of the network.
RELEASE_TO_PROCESSING_MS = 200

# Mic target per board (320x240 landscape). The all-in-one draws a tall mic button
# right of the on-screen ring; a ring board draws a full-width "Hold to talk" bar.
MIC_XY = {"freenove_s3": (272, 140), "solide_s3": (160, 206)}
DEAD_SPACE_XY = (110, 60)  # header strip left of the gear: taps nothing


def parse_voice_state(raw: str) -> dict:
    """Parse ``VOICE phase=.. outcome=.. transitions=.. shownMs=.. sttMs=.. line="..."``.
    Raises ValueError when the line is missing or unparseable."""
    idx = raw.find("VOICE phase=")
    if idx < 0:
        raise ValueError(f"no VOICE? line in: {raw!r}")
    line = raw[idx:].splitlines()[0]
    m = re.search(
        r"VOICE phase=(?P<phase>\w+) outcome=(?P<outcome>\w+) transitions=(?P<tr>\d+) "
        r"shownMs=(?P<shown>\d+) sttMs=(?P<stt>\d+) line=\"(?P<line>.*)\"$",
        line,
    )
    if not m:
        raise ValueError(f"unparseable VOICE? line: {line!r}")
    return {
        "phase": m.group("phase"),
        "outcome": m.group("outcome"),
        "transitions": int(m.group("tr")),
        "shown_ms": int(m.group("shown")),
        "stt_ms": int(m.group("stt")),
        "line": m.group("line"),
    }


def _board(device) -> str:
    raw = device.cmd("STATUS", "STATUS ", timeout=4.0)
    m = re.search(r"board=(\S+)", raw)
    return m.group(1) if m else "freenove_s3"


def _hold_and_release(device, board: str) -> dict:
    """Press-hold the mic and let the watcher release it; return VOICE? after the
    release path finished. Always lifts the injected finger."""
    x, y = MIC_XY.get(board, MIC_XY["freenove_s3"])
    try:
        device.tap(x, y, hold=True)
        device.expect("VOICE: release path done", timeout=90.0)
    finally:
        device.cmd("TAPUP", "TAPUP<", timeout=5.0)
    return parse_voice_state(device.cmd("VOICE?", "VOICE phase=", timeout=5.0))


@pytest.fixture
def voice_ready(device, wake_home):
    """Orchestrator mode on the home screen, with a speech-to-text key (a RAM-only
    placeholder when the board has none). Yields the board slug; restores the key."""
    device.ensure_mode(1)
    wake_home()
    device.cmd("STTKEY placeholder", "STTKEY<", timeout=5.0)
    yield _board(device)
    device.cmd("STTKEY off", "STTKEY<", timeout=5.0)


@pytest.mark.hil
def test_offline_release_is_instant_and_names_the_network(device, voice_ready):
    st = device.status()
    if st.group("wifi") != "0":
        pytest.skip("Wi-Fi is joined - the offline leg needs a board with no link")
    v = _hold_and_release(device, voice_ready)
    assert v["outcome"] == "no_network", f"offline release should end no_network, got {v}"
    assert v["phase"] == "notice", v
    assert v["stt_ms"] == 0, f"offline must never start the speech-to-text call: {v}"
    assert 0 < v["shown_ms"] < RELEASE_TO_PROCESSING_MS, f"release -> processing took {v['shown_ms']} ms"
    assert v["line"] == "No network. Check Wi-Fi and try again.", v
    assert "catch that" not in v["line"]
    # A tap acknowledges the outcome and hands the ring back.
    device.tap(*DEAD_SPACE_XY)
    time.sleep(0.5)
    after = parse_voice_state(device.cmd("VOICE?", "VOICE phase=", timeout=5.0))
    assert after["phase"] == "idle", after


@pytest.mark.hil
def test_online_release_shows_processing_before_the_network_call(device, voice_ready):
    st = device.status()
    if st.group("wifi") != "1":
        pytest.skip("Wi-Fi is not joined - the online leg needs a board with a link")
    v = _hold_and_release(device, voice_ready)
    assert v["stt_ms"] > 0, f"online release should start the speech-to-text call: {v}"
    assert 0 < v["shown_ms"] <= v["stt_ms"], f"processing must be shown before the network call: {v}"
    assert v["shown_ms"] < RELEASE_TO_PROCESSING_MS, v
    # With the placeholder key the provider refuses (HTTP 401): a NAMED error, never
    # no-speech. With a real key the capture is ~0.9 s of room noise: either a
    # transcript (the turn runs) or an empty one - both are honest outcomes.
    assert v["outcome"] in ("stt_http", "empty_transcript", "none"), v
    if v["outcome"] == "stt_http":
        assert "HTTP" in v["line"] and "catch that" not in v["line"], v


@pytest.mark.hil
@pytest.mark.manual
def test_voice_flow_looks_right(device, require_manual, voice_ready):
    """Owner leg: eyes on the ring and screen, and a real spoken turn online."""
    require_manual.confirm(
        "HOLD-TO-TALK (owner leg):\n"
        "  1. Hold the mic, say 'what time is it', release.\n"
        "     Confirm: 'Listening' while held; the instant you let go the ring becomes a\n"
        "     sweeping spinner with 'Transcribing', then 'Thinking' and what it heard;\n"
        "     the mic reads 'wait'; the reply appears and the ring returns to normal.\n"
        "  2. Turn Wi-Fi off (or leave its range), hold + release again.\n"
        "     Confirm: within a blink 'No network / Check Wi-Fi and try again.' and a\n"
        "     breathing alert ring; a tap clears it.\n"
        "Did every step look and read as described?"
    )
