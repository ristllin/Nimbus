"""Source guards for the device seams of the internal SRAM fixes. No board needed.

The policies are host-tested (test_refresh_small_blocks, test_turn_memory, test_sfx). What a host
build cannot reach is the wiring in src/: these three seams only compile on the device, and losing
one brings back a failure only a live unit shows.
"""

from __future__ import annotations

import re
from pathlib import Path

import pytest

pytestmark = pytest.mark.host

ROOT = Path(__file__).resolve().parents[2]


def body_of(src: str, signature: str) -> str:
    """The text of a function's body, from its signature to its closing brace."""
    at = src.index(signature)
    open_at = src.index("{", at)
    depth = 0
    for i in range(open_at, len(src)):
        depth += {"{": 1, "}": -1}.get(src[i], 0)
        if depth == 0:
            return src[open_at : i + 1]
    raise AssertionError(f"no end to {signature}")


def test_every_verify_and_list_refresh_runs_psram_first():
    """Both routes (its own worker, the Telegram task) go through runOne, and it opens the scope
    before its first statement: Lumi's 968 B came from a refresh that had none."""
    src = (ROOT / "src/agent/provider_verify.cpp").read_text()
    body = body_of(src, "static void runOne() {")
    code = [ln.strip() for ln in body.splitlines()[1:] if ln.strip() and not ln.strip().startswith("//")]
    assert code[0] == "const PsramFirstScope psramFirst;"
    assert "runOne();" in body_of(src, "static void verifyTask(void*) {")
    assert "runOne();" in body_of(src, "InlineRun runRefreshHere(const String& provider) {")


def test_an_event_sound_asks_the_gate_before_it_opens_the_speaker():
    src = (ROOT / "src/sfx/sound_fx.cpp").read_text()
    body = body_of(src, "void playEvent(const Item& it) {")
    assert body.index("g_cueGate.allow(") < body.index("resolveAndPlay(")
    assert "g_cueGate.finished(" in body
    assert "playEvent(it);" in body_of(src, "void sfxTask(void*) {")
    # The one call of the resolver is the gated one.
    calls = re.findall(r"\bresolveAndPlay\(\(", src)
    assert len(calls) == 1


def test_an_asked_sound_checks_memory_before_it_opens_the_speaker():
    """Every player in music.cpp opens through openSpeaker, and it asks first."""
    src = (ROOT / "src/sfx/music.cpp").read_text()
    body = body_of(src, "static bool openSpeaker(uint32_t rate, OpenHook onOpen) {")
    assert body.index("speakerMemoryOk(") < body.index("solide::audio::spkOpen(")
    outside = src.replace(body, "")
    assert "solide::audio::spkOpen(" not in outside
