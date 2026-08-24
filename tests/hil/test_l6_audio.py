"""§L6 - HIL audio tests (the HIL test spec).

mic/speaker/loopback, semi-automated with LOUD manual asserts. The point (F5/F6) is
to BISECT an aggregate SKIP: ``TEST mic`` (RMS monitor), ``TEST spk`` (audible tone),
``TEST audio`` (acoustic loopback with a decoded LbDiag fault class) distinguish a
dead mic from a silent speaker from bad coupling instead of shrugging at one number.

Grammar (solide/selftest.h + solide/audio.h LbDiag):
  ``TEST mic``   -> ``RESULT mic PASS|SKIP rms=.. peak=..``
  ``TEST spk``   -> ``RESULT spk PASS|SKIP ..``
  ``TEST audio`` -> ``RESULT audio PASS|SKIP toneMag=.. ctrlMag=.. rms=.. peak=..``

``@pytest.mark.audio`` (needs the audio board) + ``@pytest.mark.manual`` where a human
taps/listens. An unconfirmed manual step is a LOUD failure; a device-gated run is a
loud skip; neither is ever a green pass.
"""

from __future__ import annotations

import re

import pytest


def _kv(rest: str) -> dict:
    """Parse ``k=v`` integer tokens out of the RESULT line tail into a dict."""
    return dict(re.findall(r"(\w+)=(-?\d+)", rest))


# ---- mic_alive (F5, F6) - manual -------------------------------------------
@pytest.mark.audio
@pytest.mark.manual
def test_mic_alive(device, require_manual):
    """mic_alive (F5, F6): baseline ``TEST mic`` (quiet), then the operator TAPS the
    mic and we re-run; assert reported ``rms``/``peak`` JUMPS above the quiet baseline.
    Measures the mic instead of guessing about it."""
    device.reset()
    device.wait_ready(timeout=20.0)

    base = device.selftest("mic", timeout=15.0)
    if base.group("verdict") == "SKIP":
        pytest.skip(f"TEST mic SKIP ({base.group('rest').strip()}); audio board absent - cannot measure the mic")
    bkv = _kv(base.group("rest"))
    assert "rms" in bkv and "peak" in bkv, f"TEST mic gave no rms/peak to measure: {base.group('rest')!r}"
    base_rms, base_peak = int(bkv["rms"]), int(bkv["peak"])

    require_manual.confirm(
        "TAP the microphone firmly a few times (or speak loudly right at it). Press y "
        "the MOMENT before you start tapping.",
        timeout=30.0,
    )

    tapped = device.selftest("mic", timeout=15.0)
    tkv = _kv(tapped.group("rest"))
    tap_rms, tap_peak = int(tkv.get("rms", 0)), int(tkv.get("peak", 0))

    assert tap_rms > base_rms + 200 or tap_peak > base_peak + 2000, (
        f"mic level did not rise on tap (baseline rms={base_rms} peak={base_peak}, "
        f"tapped rms={tap_rms} peak={tap_peak}) - mic not responding (F6: the 'mic is "
        "broken' claim, now actually measured)"
    )


# ---- speaker_audible (F5) - manual -----------------------------------------
@pytest.mark.audio
@pytest.mark.manual
def test_speaker_audible(device, require_manual):
    """speaker_audible (F5): ``TEST spk`` plays an audible tone; a human confirms they
    HEARD it. Unconfirmed -> LOUD failure (the confirmation is the whole assertion).
    The selftest verdict must not be FAIL before the listen."""
    device.reset()
    device.wait_ready(timeout=20.0)

    res = device.selftest("spk", timeout=15.0)
    if res.group("verdict") == "SKIP":
        pytest.skip(f"TEST spk SKIP ({res.group('rest').strip()}); audio board absent - no speaker to sound")
    assert res.group("verdict") != "FAIL", f"TEST spk reported FAIL before the listen: {res.group('rest').strip()}"

    require_manual.confirm(
        "Did you HEAR a clear tone from the speaker just now? Press y ONLY if you heard it.", timeout=30.0
    )


# ---- loopback_explains (F5) ------------------------------------------------
@pytest.mark.audio
def test_loopback_explains(device):
    """loopback_explains (F5): ``TEST audio`` (speaker->mic acoustic loopback) either
    PASSES (backed by a real tone, not luck), or SKIPs with a DECODED fault class -
    never a bare/undecodable SKIP. The class is derived from the LbDiag numbers per
    audio.h: toneMag>1000 & toneMag>2*ctrlMag => tone reproduced; rms~0/peak~0 =>
    MIC-DEAD; rms high but toneMag~=ctrlMag => SPEAKER/COUPLING."""
    device.reset()
    device.wait_ready(timeout=20.0)

    res = device.selftest("audio", timeout=25.0)
    verdict = res.group("verdict")
    rest = res.group("rest").strip()
    kv = _kv(rest)

    if verdict == "PASS":
        tone = int(kv.get("toneMag", 0))
        ctrl = int(kv.get("ctrlMag", 1))
        assert tone > 1000 and tone > 2 * ctrl, (
            f"audio PASS but toneMag={tone} ctrlMag={ctrl} do not show a real tone "
            "(F5: PASS must be earned, not an aggregate fluke)"
        )
        return

    # A genuine hardware-absent SKIP with a reason is legitimate (loud skip).
    if verdict == "SKIP" and ("absent" in rest or "no audio" in rest.lower()):
        pytest.skip(f"audio board absent: {rest}")

    # Otherwise the contract is: the result must be EXPLAINED by a decoded fault class,
    # not left as a bare aggregate. That decoded explanation is the assertion (F5).
    fault = _classify(kv)
    assert fault is not None, (
        f"audio {verdict} with no decodable fault class from {rest!r} - the exact F5 "
        "gap (an aggregate SKIP/FAIL with no isolation of mic vs speaker vs coupling)"
    )
    print(f"[loopback_explains] {verdict} decoded fault: {fault} ({rest})")


# ---- stt_transcribes - the audio-input regression, on hardware --------------
@pytest.mark.audio
@pytest.mark.agent
@pytest.mark.manual
def test_stt_micrec_transcribes(device, require_manual):
    """MICREC drives the EXACT path the audio-input regression lived on:
    recordToFile -> transcribePcm -> httpmp::post -> readHttpBody -> parseTranscription.
    Asserts the mic captured audio AND STT returned a NON-EMPTY transcript - the
    silent-empty transcript (bad-JSON / "HTTP 0") is precisely what the fixed-limit
    response read used to produce for longer captures.

    Scope note: the deterministic >2048-byte truncation case (the root cause) is locked
    by the host test ``test/test_audio_stt`` - hardware can't produce a ~2 KB transcript
    on demand, so this proves the round-trip is ALIVE with a short spoken phrase. The
    injection->handleMessage->reply machinery is already covered by test_l12 (TURN /
    custom), so it isn't re-driven here.

    Requires a mic + an STT key already provisioned on the device (Mistral/Voxtral or
    OpenAI). A missing key surfaces as an empty transcript -> LOUD fail with the
    /api/log pointer (never a silent pass); an absent mic -> 0 bytes -> LOUD fail."""
    device.reset()
    device.wait_ready(timeout=20.0)

    require_manual.confirm(
        "MICREC records 4 s. Speak a clear short phrase (e.g. 'the quick brown fox') immediately after you press y.",
        timeout=30.0,
    )

    # Timeout covers the 4 s record + the STT round-trip (response deadline is now 45 s).
    m = device.cmd_re("MICREC", r'MICREC bytes=(\d+) transcript="(.*)"', timeout=60.0)
    nbytes = int(m.group(1))
    transcript = m.group(2)
    assert nbytes > 0, "mic captured 0 bytes - mic not delivering audio (a mic fault, not the STT bug)"
    assert transcript.strip(), (
        "STT returned an EMPTY transcript despite captured audio - the audio-input "
        "regression shape. Read GET /api/log [stt] lines to classify: respLen~2048 + "
        "'bad JSON' = the old body cap; err='HTTP 0' = the response deadline; "
        "'no key' = provider key not provisioned."
    )
    print(f"[stt_micrec] bytes={nbytes} transcript={transcript!r}")


# ---- spoken_reply_audible (N12 / CUM-134 #1) - manual ----------------------
@pytest.mark.audio
@pytest.mark.agent
@pytest.mark.manual
def test_spoken_reply_audible(device, require_manual):
    """N12: the on-device spoken REPLY path, end to end on hardware - the bug that
    shipped SILENT. ``SPKSAY <text>`` drives the exact seam reply.speak/the tts action
    use: synthesize with the CONFIGURED provider's format, write it to LittleFS, and
    play it on the speaker (OpenAI WAV via playWavFile, Mistral MP3 via the vendored
    minimp3 decoder). On the shipped default (Mistral) this exercises the MP3-to-speaker
    branch that never existed before - the reason a Mistral-only device could never talk.

    Asserts synthesis produced bytes AND playback returned played=1, then a human
    confirms they HEARD it (the confirmation is the real assertion - an I2S write into a
    dead speaker still 'plays'). The reported fmt should be ``mp3`` on a Mistral device
    and ``wav`` on an OpenAI one; either proves the routing.

    Requires a TTS key + network already provisioned. No key / no network -> bytes=0 ->
    LOUD skip with the /api/log pointer, never a silent green. An absent speaker is caught
    by test_speaker_audible; this test is about the reply pipeline, not the driver.

    STACK CAVEAT (bench): SPKSAY decodes on the console/loop task, NOT the sfx task that
    the real reply.speak/tts action path uses. minimp3's frame decoder puts a ~16 KB
    scratch on the stack, and the sfx task stack was raised for it (see sound_fx.cpp
    kSfxStackBytes). To validate the REAL path, also drive an actual voice reply (send a
    turn that makes the model call reply.speak) and check `STACK?`/the boot stack-HWM log
    for the sfx task keeps healthy headroom while an MP3 reply plays."""
    device.reset()
    device.wait_ready(timeout=20.0)

    # Synthesis is a network round-trip (bounded ~25 s) plus a short playback; give it room.
    m = device.cmd_re(
        "SPKSAY Nimbus voice reply check. One, two, three.",
        r"SPKSAY bytes=(\d+) fmt=(wav|mp3) played=(\d)",
        timeout=60.0,
    )
    nbytes, fmt, played = int(m.group(1)), m.group(2), int(m.group(3))
    if nbytes == 0:
        pytest.skip(
            "SPKSAY synthesized 0 bytes - no TTS key/network provisioned "
            "(check GET /api/log [tts] lines); nothing to sound"
        )
    assert played == 1, (
        f"TTS synthesized {nbytes} bytes (fmt={fmt}) but playback returned played=0 - "
        "the reply reached the speaker path and failed there (decode or I2S), NOT the "
        "old silent 0-byte synthesis failure"
    )
    require_manual.confirm(
        f"Did you HEAR the spoken phrase from the speaker just now? (synth fmt={fmt}) "
        "Press y ONLY if you heard the words.",
        timeout=30.0,
    )
    print(f"[spoken_reply] bytes={nbytes} fmt={fmt} played={played}")


def _classify(kv: dict) -> "str | None":
    """Map LbDiag numbers to a fault class (audio.h interpretation), or None if the
    numbers are insufficient to decide."""
    if not kv:
        return None
    rms = int(kv.get("rms", -1))
    peak = int(kv.get("peak", -1))
    tone = int(kv.get("toneMag", -1))
    ctrl = int(kv.get("ctrlMag", -1))
    if rms < 0 and peak < 0 and tone < 0:
        return None
    if (0 <= rms <= 5) or (0 <= peak <= 50):
        return "MIC-DEAD (mic delivering no data)"
    if tone >= 0 and ctrl >= 0:
        if tone > 1000 and tone > 2 * max(ctrl, 1):
            return None  # actually a good tone; caller treats as pass-worthy
        if rms > 100 and tone <= 2 * max(ctrl, 1):
            return "SPEAKER/COUPLING (mic hears noise, not the tone)"
    return "AMBIGUOUS (has data but no clear tone; check amp/5V + coupling)"
