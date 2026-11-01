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


def test_an_event_sound_asks_the_gate_on_its_turn_before_it_opens_the_speaker():
    src = (ROOT / "src/sfx/sound_fx.cpp").read_text()
    body = body_of(src, "void playEvent(const Item& it) {")
    assert body.index("turn.take(0)") < body.index("dmaFreeNow()") < body.index("g_cueGate.allow(") < body.index("resolveAndPlay(")
    assert "g_cueGate.finished(" in body
    assert "playEvent(it);" in body_of(src, "void sfxTask(void*) {")
    # The one call of the resolver is the gated one.
    calls = re.findall(r"\bresolveAndPlay\(\(", src)
    assert len(calls) == 1


def test_an_asked_sound_checks_memory_before_it_opens_the_speaker():
    """Every player in music.cpp opens through openSpeaker, and it asks first."""
    src = (ROOT / "src/sfx/music.cpp").read_text()
    body = body_of(src, "static bool openSpeaker(uint32_t rate, OpenHook onOpen) {")
    assert body.index("roomToOpen(") < body.index("solide::audio::spkOpen(")
    assert "speakerMemoryOk(" in body_of(src, "static bool roomToOpen(uint32_t& dmaFree) {")
    outside = src.replace(body, "")
    assert "solide::audio::spkOpen(" not in outside


def test_the_memory_gate_uses_this_boards_speaker():
    """The need comes from the board's audio hardware, in every reader: a codec board's open is twice a raw one's."""
    turn = (ROOT / "src/sfx/speaker_turn.h").read_text()
    assert "solide::activeBoard().audioKind == solide::AudioKind::Es8311Codec" in turn
    fx = (ROOT / "src/sfx/sound_fx.cpp").read_text()
    music = (ROOT / "src/sfx/music.cpp").read_text()
    assert "g_cueGate(speakerProfileHere())" in fx
    for src in (fx, music):
        for call in re.findall(r"speakerMemoryOk\(([^,]+),", src):
            assert "speakerProfileHere()" in call


def test_a_reply_reads_memory_only_after_the_track_it_stopped_has_closed():
    """stopForSpeech only asks the track to leave. A reading before it closes counts the track's own buffers."""
    fx = (ROOT / "src/sfx/sound_fx.cpp").read_text()
    body = body_of(fx, "void playReply(const Item& it) {")
    assert body.index("music::stopForSpeech()") < body.index("turn.take(kReplyTurnWaitMs)") < body.index("streamMp3File(")


def test_a_track_plays_only_on_its_turn_and_a_refusal_stops_the_queue():
    """A cue or reply in front of it holds the buffers, and a refused track must not skip to the next in a loop."""
    music = (ROOT / "src/sfx/music.cpp").read_text()
    play = body_of(music, "static void playTrack(const std::string& name, uint32_t gen) {")
    assert play.index("takeSpeakerForTrack(turn, gen)") < play.index("streamWav(")
    take = body_of(music, "static bool takeSpeakerForTrack(sfx::SpeakerTurn& turn, uint32_t gen) {")
    assert take.index("turn.take(") < take.index("roomToOpen(") < take.index("g_q.stop()")


def test_one_cue_walks_the_tiers_through_the_stop_on_failure():
    """The walk that ends at the first failed allocation is the only way the tiers are played."""
    fx = (ROOT / "src/sfx/sound_fx.cpp").read_text()
    body = body_of(fx, "nimbus::sfx::ClipResult resolveAndPlay(Ev e) {")
    assert "playFirstTier(" in body and "heapwatch::allocFails()" in body
    for player in ("playFromPool(", "playEmbedded("):
        assert player not in body


def test_a_refused_asked_sound_leaves_a_sign():
    """The owner's test sound, a menu tone: refused or failed, the log says so, and the web answer is not ok."""
    fx = (ROOT / "src/sfx/sound_fx.cpp").read_text()
    body = body_of(fx, "void playEvent(const Item& it) {")
    assert body.count("askedNotPlayed(") == 3
    web = (ROOT / "src/net/webui.cpp").read_text()
    at = web.index('s_server.on("/api/audio/sfx"')
    handler = web[at : web.index("});", at)]
    assert "::sfx::play(slug.c_str(), &lowMemory)" in handler
    assert handler.index("lowMemory.length()") < handler.index("503")
