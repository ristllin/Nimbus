"""Host tests for the L67 voice delivery legs' parsers (no board).

The legs in test_l67_voice_delivery.py read Telegram's receipts from the device log,
the tool's own receipt, the speaker path and the provider-reported usage; a renamed
log line must fail here, never pass a leg vacuously.
"""

from __future__ import annotations

import json

import pytest

from test_l67_voice_delivery import (
    probe_verdict,
    receipt_ids,
    speaker_touches,
    spend_lift,
    spend_of,
    tg_page,
    tool_receipt_ids,
    turn_window,
)

pytestmark = pytest.mark.host


def test_receipt_and_speaker_parsers():
    lines = [
        "tts: mistral wrote 61234 bytes -> /tgv-0001a2b3-1.mp3",
        "telegram: sendAudio ok (message_id 812, 61234 bytes)",
        "voice: delivered to Telegram as a voice message (message_id 812, 60 KB)",
        "fabric-spend: anthropic/claude-haiku-5-5 in=12 out=80 cR=1000 cW=200 n=2",
    ]
    assert receipt_ids(lines) == [812]
    assert tool_receipt_ids(lines) == [812]
    assert not speaker_touches(lines)
    assert spend_of(lines) == [("anthropic/claude-haiku-5-5", 12, 80, 1000, 200)]
    multi = ["voice: delivered to Telegram as 3 voice messages (message_id 811, 812, 813; 640 KB in all)"]
    assert tool_receipt_ids(multi) == [811, 812, 813]
    assert receipt_ids(["telegram: sendAudio FAIL (Bad Request: chat not found)"]) == []


def test_every_speaker_mark_is_seen():
    for line in (
        "tts: mistral wrote 5 bytes -> /reply.mp3",
        "tts: openai wrote 5 bytes -> /reply.wav",
        "speak: could not play on the speaker: the audio driver could not start",
        "speaker: could not play on the speaker: the audio driver could not start",
        "sfx: /reply.mp3 did not play to its end",
    ):
        assert speaker_touches([line]), line


def test_the_probe_tells_present_from_missing():
    assert probe_verdict("editMessageCaption failed: Bad Request: message is not modified: specified") == "exists"
    assert probe_verdict("editMessageCaption failed: Bad Request: message to edit not found") == "missing"
    assert probe_verdict("editMessageCaption failed: Bad Request: MESSAGE_ID_INVALID") == "missing"
    assert probe_verdict("editMessageCaption failed: Forbidden") == "editMessageCaption failed: Forbidden"


def test_the_injected_page():
    page = json.loads(tg_page("4242", 77, "hi"))
    msg = page["result"][0]["message"]
    assert page["ok"] and msg["chat"]["id"] == 4242 and msg["from"]["id"] == 4242 and msg["text"] == "hi"
    assert page["result"][0]["update_id"] == 77


def test_the_turn_window_starts_at_the_injected_turn():
    lines = [
        "orchestrator: turn host=anthropic (conv=0 heap=1)",  # a queued real message's turn
        "telegram: send ok to 1",
        "turn end: ok host=anthropic",
        "orchestrator: turn host=anthropic (conv=0 heap=2)",  # the injected page's turn
        "telegram: sendAudio ok (message_id 5, 10 bytes)",
        "telegram: injected page: 1 message batched into 1 turn",
        "telegram: send ok to 2",
    ]
    w = turn_window(lines)
    assert w[0].endswith("heap=2)") and receipt_ids(w) == [5] and w[-1] == "telegram: send ok to 2"
    assert turn_window(["no turn line", "telegram: injected page: 0 messages"])[0] == "no turn line"


def test_a_paused_ceiling_is_lifted_and_restored():
    assert spend_lift({"frozen": False, "limitCents": 2000, "spentCents": 10}) is None
    assert spend_lift({"frozen": True, "limitCents": 2000, "spentCents": 2003}) == ("2000", "2503")
    assert spend_lift({"frozen": True, "limitCents": 0, "spentCents": 7}) == ("off", "507")
