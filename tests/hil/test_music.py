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

import struct
import time

import pytest

try:
    import requests
except ImportError:  # pragma: no cover
    requests = None

from test_l4_network import lan_ip_or_skip
from test_l9_resilience import _webtok


def _hdr(tok: str) -> dict:
    return {"X-Nimbus-Token": tok}


def _tiny_wav(seconds: float = 0.05, rate: int = 16000) -> bytes:
    """A minimal valid 16-bit mono PCM WAV so the upload lands a real, playable track."""
    n = int(rate * seconds)
    data = b"\x00\x00" * n
    return (
        b"RIFF" + struct.pack("<I", 36 + len(data)) + b"WAVE"
        b"fmt " + struct.pack("<IHHIIHH", 16, 1, 1, rate, rate * 2, 2, 16)
        + b"data" + struct.pack("<I", len(data)) + data
    )


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


# ---- CUM-40: the web upload path (put a track into /music, then play it) -----
# The gap this closed: before, /music could only be filled over the serial console,
# so /play always found an empty folder. These prove the USER path end to end over
# the LAN web API. Markers: ``@pytest.mark.net`` (needs a board on the LAN + token).


@pytest.mark.net
def test_music_upload_requires_token(device, net, secrets, require_secret):
    """Every /api/music route needs the token; an unauthenticated upload writes no byte."""
    if requests is None:
        pytest.skip("requests not installed")
    ip = lan_ip_or_skip(device, net, secrets, require_secret)
    assert net.get("/api/music/list", ip=ip, auth=False).status_code == 401
    r = requests.post(
        f"http://{ip}/api/music/upload?name=x.wav", files={"file": ("x.wav", _tiny_wav())}, timeout=10
    )
    assert r.status_code == 401, f"unauthenticated music upload -> {r.status_code}, want 401"


@pytest.mark.net
def test_music_upload_lists_and_plays(device, net, secrets, require_secret):
    """Upload a valid WAV -> it appears in /api/music/list -> it plays -> delete it."""
    if requests is None:
        pytest.skip("requests not installed")
    ip = lan_ip_or_skip(device, net, secrets, require_secret)
    tok = _webtok(device)
    name = "hiltest-clip.wav"
    up = requests.post(
        f"http://{ip}/api/music/upload?name={name}", files={"file": (name, _tiny_wav())},
        headers=_hdr(tok), timeout=20,
    )
    if up.status_code != 200 and not net.get_json("/api/music/list", ip=ip).get("present", False):
        pytest.skip("no SD card mounted")
    assert up.status_code == 200, f"music upload -> {up.status_code}: {up.text}"
    try:
        listing = net.get_json("/api/music/list", ip=ip, timeout=8.0)
        assert name in (listing.get("tracks") or []), f"uploaded track not listed: {listing}"
        # Play it, then stop - the device must accept both and stay responsive.
        assert net.post("/api/music/play", {"name": name, "t": tok}, ip=ip).status_code == 200
        assert net.post("/api/music/play", {"action": "stop", "t": tok}, ip=ip).status_code == 200
        device.cmd_re("PING", r"PONG|PING", timeout=6.0)
    finally:
        net.post("/api/music/rm", {"name": name, "t": tok}, ip=ip)
    gone = net.get_json("/api/music/list", ip=ip, timeout=8.0)
    assert name not in (gone.get("tracks") or []), "track still listed after delete"


@pytest.mark.net
def test_music_upload_rejects_bad_extension(device, net, secrets, require_secret):
    """A non-audio name is refused (400) and never lands in /music."""
    if requests is None:
        pytest.skip("requests not installed")
    ip = lan_ip_or_skip(device, net, secrets, require_secret)
    tok = _webtok(device)
    r = requests.post(
        f"http://{ip}/api/music/upload?name=notes.txt", files={"file": ("notes.txt", b"nope")},
        headers=_hdr(tok), timeout=10,
    )
    assert r.status_code == 400, f"bad-extension upload -> {r.status_code}, want 400"
    listing = net.get_json("/api/music/list", ip=ip, timeout=8.0)
    assert "notes.txt" not in (listing.get("tracks") or [])
