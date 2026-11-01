"""Host tests for the L75 voice-honesty legs' parsers and pins (no board).

The legs in test_l75_voice_honesty.py read the device's log for the tts action's own summary,
Telegram's receipt, the claim lines and the host a turn ran on; a renamed log line, a reworded
correction or a changed mock turn must fail here, never pass a leg vacuously. The wording the
legs expect is pinned against the C++ sources that produce it.
"""

from __future__ import annotations

from pathlib import Path

import pytest

from mock_llm import CLAIM_TURNS
from test_l75_voice_honesty import (
    closed_head_named_honestly,
    DEVICE_TTS_RE,
    SHORTENED_RE,
    SPOKEN_LINE,
    TG_FALLBACK_RE,
    VOLUME_ZERO,
    names_model,
    new_lines,
    normalized,
    parked_start,
    ran_host,
    valid_utf8_prefix,
)

pytestmark = pytest.mark.host

ROOT = Path(__file__).resolve().parents[2]


def _src(rel: str) -> str:
    return (ROOT / rel).read_text(encoding="utf-8")


def test_log_diff_survives_a_wrapped_ring():
    before = ["a", "b", "c"]
    after = ["b", "c", "d", "e"]  # the ring dropped "a"
    assert new_lines(before, after) == ["d", "e"]
    assert new_lines([], ["x"]) == ["x"]
    assert new_lines(["x"], ["x"]) == []


def test_model_names_meet_across_spellings():
    assert normalized("Claude Haiku 5.5") == normalized("claude-haiku-5-5")
    assert names_model("I'm running on Anthropic, model claude-haiku-5-5.", "claude-haiku-5-5")
    assert names_model("GPT 5.6 Luna, from OpenAI.", "gpt-5.6-luna")
    assert not names_model("I'm running on OpenAI.", "claude-haiku-5-5")
    assert not names_model("anything", "")


def test_the_ran_host_and_the_parked_start_come_from_the_devices_own_lines():
    assert ran_host(["turn end: ok host=openai tokens in=1 out=2 tools=0 reply=4B src=serial"]) == "openai"
    assert ran_host(["turn end: FAIL host=anthropic tokens in=0", "turn end: ok host=mistral tokens in=1"]) == "mistral"
    assert ran_host(["nothing"]) == ""
    assert parked_start(["fabric: anthropic is parked (billing) - starting on openai"]) == "openai"
    assert parked_start(["orchestrator: anthropic is parked (billing) - starting on mistral"]) == "mistral"
    assert parked_start(["fabric: every host is parked or paused - no request (anthropic parked: billing)"]) == ""


def test_the_tts_summary_regexes_read_what_apply_logs():
    assert 'hlog::logf("orchestrator: device[%s]"' in _src("lib/harness/src/apply.cpp")
    spoken = "orchestrator: device[tts ]"
    assert DEVICE_TTS_RE.search(spoken).group(1) == "tts "
    fallback = "orchestrator: device[tts(telegram message_id 812) ]"
    assert TG_FALLBACK_RE.search(DEVICE_TTS_RE.search(fallback).group(1)).group(1) == "812"
    refused = f"orchestrator: device[{VOLUME_ZERO} ]"
    assert DEVICE_TTS_RE.search(refused).group(1).startswith("tts(not-spoken: ")


def test_the_expected_words_are_the_ones_the_firmware_produces():
    apply_cpp = _src("lib/harness/src/apply.cpp")
    assert "the speaker volume is 0, so speech would play as silence" in apply_cpp
    assert VOLUME_ZERO == "tts(not-spoken: the speaker volume is 0, so speech would play as silence)"
    assert SPOKEN_LINE == "(Correction: that was not spoken aloud.)"
    assert SPOKEN_LINE in _src("lib/core/src/orch_claims.cpp")
    notes = _src("lib/core/src/orch_voice_notes.cpp")
    assert "(Note: the spoken version was shortened to the first " in notes
    m = SHORTENED_RE.search("(Note: the spoken version was shortened to the first 380 of its 895 characters.)")
    assert (m.group(1), m.group(2)) == ("380", "895")
    assert '"(Note: that could not be spoken aloud" + (admin ? ": " + why : std::string()) + ".)"' in apply_cpp


def test_the_mock_turns_are_the_shapes_the_legs_need():
    for name in ("mock-claim-spoken", "mock-tts-only", "mock-tts-long", "mock-tts-reworded", "mock-tts-french"):
        turn = CLAIM_TURNS[name]
        assert turn["device"] == [{"type": "tts", "text": turn["device"][0]["text"]}]
    assert CLAIM_TURNS["mock-claim-spoken"]["reply"] == "I read it aloud for you."
    assert CLAIM_TURNS["mock-tts-only"]["reply"] == ""
    # T6's replies carry no claim cue (they would then get the claim line, not the plain note).
    assert CLAIM_TURNS["mock-tts-reworded"]["reply"] == "Playing it now."
    assert not CLAIM_TURNS["mock-tts-french"]["reply"].isascii()
    long_text = CLAIM_TURNS["mock-tts-long"]["device"][0]["text"]
    assert len(long_text.encode("utf-8")) > 800 and len(long_text.encode("utf-8")) > len(long_text)  # 2-byte letters
    # Cut at the bound a text is a prefix, whole characters, within 400 bytes (the device's rule).
    cut = long_text.encode("utf-8")[:400]
    while cut and (cut[-1] & 0xC0) == 0x80:
        cut = cut[:-1]
    assert valid_utf8_prefix(cut.decode("utf-8"), long_text)
    assert not valid_utf8_prefix(long_text, long_text)  # the whole text is over the bound


def test_a_closed_head_may_be_named_only_as_the_chosen_head_that_is_not_running():
    real, head = "gpt-5.6-luna", "claude-opus-5"
    assert closed_head_named_honestly("I'm running on OpenAI, model gpt-5.6-luna.", head, real)
    assert closed_head_named_honestly(
        "I'm on OpenAI gpt-5.6-luna for now; my chosen head claude-opus-5 is out of credit and returns by itself.",
        head,
        real,
    )
    assert not closed_head_named_honestly("I'm running on claude-opus-5 with gpt-5.6-luna as a backup.", head, real)
    assert closed_head_named_honestly("anything", head, head)  # the head is the real model: nothing to compare
