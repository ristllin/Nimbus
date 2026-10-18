"""L57 - online hold-to-talk with no mic (program-2026-10-18 amendment, OA-030).

The online voice leg was never run: hold-to-talk needs a finger on the mic
button and a spoken clip, so no automated leg ever exercised release ->
speech-to-text -> turn with a real provider. Lane N's ``AUDIOINJECT <path>``
console verb feeds a recorded clip from the card through the SAME release path a
real hold takes: the clip's samples replace the microphone capture in
``/voice.pcm``, then ``nimbus::voice::afterRelease()`` shows processing, runs the
real speech-to-text call and sends the transcript as the voice turn. Spoken
replies stay off for the turn (speaker fault + the console's speech mute) until
``AUDIOINJECT off``.

What this leg proves: the injected clip REACHES the speech-to-text step. The
release path prints ``VOICE: stt start`` when the STT call begins, and the verb's
own line reports ``stt_ms=<ms after release>`` (0 = the call never started).

Bench prerequisites (the orchestrator, see PR_BODY): a 16 kHz mono 16-bit WAV of
a short spoken sentence on the card at ``/voice_bench.wav`` (FSPUT it), and a
speech-to-text key on the board. A wrong-format clip is refused with a reason.

Markers: ``hil`` (needs the board) and ``agent`` (a real STT call, cents).
"""

from __future__ import annotations

import re

import pytest

from device import Device, ExpectTimeout

pytestmark = [pytest.mark.hil, pytest.mark.agent]

CLIP_PATH = "/voice_bench.wav"


@pytest.fixture(scope="module")
def clip_ready(device: Device) -> Device:
    """Skip with a reason (never pass) when the bench prerequisite is missing."""
    try:
        st = device.cmd("FSTAT " + CLIP_PATH, "FSTAT", timeout=10.0)
    except ExpectTimeout:
        pytest.skip("FSTAT did not answer")
    if "ERR" in st:
        pytest.skip(f"{CLIP_PATH} is not on the card - FSPUT a 16 kHz mono 16-bit WAV first")
    return device


def test_injected_clip_reaches_speech_to_text(clip_ready: Device):
    dev = clip_ready
    dev.drain(quiet=0.3)
    dev.send("AUDIOINJECT " + CLIP_PATH)
    # The release path shows processing BEFORE the network call, then starts STT.
    dev.expect_re(r"VOICE: injected clip .* bytes=\d+", timeout=15.0)
    dev.expect_re(r"VOICE: stt start \+\d+ms after release", timeout=20.0)
    line = dev.expect_re(
        r"AUDIOINJECT \S+ pcm=(?P<pcm>\d+) stt_ms=(?P<stt>\d+) outcome=(?P<out>\S+) tts=off", timeout=60.0
    )
    assert int(line.group("pcm")) > 12000, "clip shorter than about 0.4 s of audio - not a real sentence"
    assert int(line.group("stt")) > 0, "the speech-to-text call never started"
    # "none" = the transcript was read and sent as the voice turn; any other outcome
    # names the failure (no_network, stt_http, stt_refused, empty_transcript, ...).
    assert line.group("out") == "none", f"release path ended {line.group('out')}"
    dev.cmd("AUDIOINJECT off", "AUDIOINJECT off", timeout=5.0)


def test_wrong_format_clip_is_refused_with_a_reason(clip_ready: Device):
    # A path that is not a WAV (the card's own directory listing target) must be refused
    # before any STT call is made.
    line = clip_ready.cmd("AUDIOINJECT /no_such_clip.wav", "AUDIOINJECT", timeout=10.0)
    assert re.search(r"refused|cannot open", line), line
    assert "stt_ms" not in line
