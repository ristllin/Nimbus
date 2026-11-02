"""Static guard: a device seam never spends a parked provider's one re-check on nothing.

Runs with no board. The device's paid seams (``src/agent/adapters``) need Arduino, so no
host test can run them, and this is the only thing that keeps them honest. Each rule was a
shipped or reviewed defect (the lost re-check, and its siblings on the same seams):

  * every return between a seam's ``gate::check`` and its first network step hands the
    re-check back. A seam that claimed the probe and then found the TLS slot busy leaves it
    "in flight" for the whole lease, and the park logs a failure for a request that never
    left;
  * speech to text hands it back when the uploader sent nothing, rather than reporting a
    failed answer;
  * a question that sends nothing (``available``) reads the park with ``peek``;
  * only a seam about to send asks the claiming question;
  * text to speech reads a refusal it may hold only part of through the tolerant reader;
  * the device's only call of the idle re-check waits for the owner, and comes first in the
    pump.
"""

from __future__ import annotations

import re
from pathlib import Path

_REPO = Path(__file__).resolve().parents[2]
_ADAPTERS = _REPO / "src" / "agent" / "adapters"

# A seam's gate::check and the first thing that really goes to the network after it.
_SEAMS = {
    "image_gen.cpp": ("generateToBuffer(", r"\.connect\("),
    "audio_tts.cpp": ("synthesizeToFile(", r"connectTts\("),
    "embeddings.cpp": ("embedWith(", r"\.connect\("),
    "audio_stt.cpp": ("transcribeCommon(", r"httpmp::post\("),
}


def _function_body(text: str, signature: str) -> str:
    """The brace-balanced body of the definition that starts with `signature`."""
    for match in re.finditer(re.escape(signature), text):
        at = match.start()
        if text.rfind("\n", 0, at) + 1 == at or text[text.rfind("\n", 0, at) + 1 : at].strip():
            brace = text.find("{", at)
            semi = text.find(";", at)
            if brace < 0 or (0 <= semi < brace):
                continue  # a call or a declaration
            depth = 0
            for i in range(brace, len(text)):
                depth += {"{": 1, "}": -1}.get(text[i], 0)
                if depth == 0:
                    return text[brace : i + 1]
    raise AssertionError(f"no definition of {signature}")


def _unhanded_returns(body: str, send: str) -> list[str]:
    """Returns after the seam asked the gate and before it sends, with no hand back."""
    asked = re.search(r"gate::check\(|closedByPark\(", body)
    if not asked:
        return []
    found = re.search(send, body[asked.start() :])
    region = body[asked.start() : asked.start() + found.start()] if found else body[asked.start() :]
    lines = region.splitlines()
    bad = []
    for i, line in enumerate(lines):
        if not re.search(r"\breturn\b", line) or line.strip().startswith("//"):
            continue
        start = i  # the statement or block that returns: back to its `if`
        while start > 0 and "if (" not in lines[start]:
            start -= 1
        near = "\n".join(lines[start : i + 1])
        if any(ok in near for ok in ("handBack", "busy()", ".open", "closedByPark", "failFromStatus")):
            continue  # handed back; the closed verdict; an answer the park was told about (the bench's)
        bad.append(line.strip())
    return bad


def test_every_return_before_the_send_hands_the_recheck_back():
    bad = {}
    for name, (signature, send) in _SEAMS.items():
        text = (_ADAPTERS / name).read_text()
        body = _function_body(text, signature)
        if "gate::check(" not in body and "closedByPark(" not in body:
            raise AssertionError(f"{name}: {signature} no longer asks the park")
        found = _unhanded_returns(body, send)
        if found:
            bad[name] = found
    assert not bad, bad


def test_the_guard_flags_a_seam_that_drops_the_recheck():
    body = """{
  const auto gv = gate::check("openai", seam, key);
  if (!gv.open) { errOut = "closed"; return nullptr; }
  arbiter::WorkSlot slot(10000);
  if (!slot) { free(buf); errOut = "device busy"; return nullptr; }
  client.connect(host, 443);
}"""
    assert _unhanded_returns(body, r"\.connect\(") == [
        'if (!slot) { free(buf); errOut = "device busy"; return nullptr; }'
    ]
    handed = body.replace("{ free(buf);", '{ gate::handBack("openai"); free(buf);')
    assert _unhanded_returns(handed, r"\.connect\(") == []


def test_speech_to_text_hands_back_what_it_did_not_send():
    text = (_ADAPTERS / "audio_stt.cpp").read_text()
    body = _function_body(text, "static void noteToPark(")
    sent_nothing = body.find("sttSentNothing(")
    assert sent_nothing >= 0, "the upload's errors that sent nothing must not read as an answer"
    assert body.find("gate::handBack", sent_nothing) >= 0
    assert body.find("gate::handBack", sent_nothing) < body.find("gate::noteAnswer")


def test_a_question_that_sends_nothing_reads_with_peek():
    text = (_ADAPTERS / "image_vision.cpp").read_text()
    body = _function_body(text, "bool available()")
    assert ".peek(" in body
    assert ".available(" not in body


# Who may ask the claiming question: each is about to send, and hands it back if it does not.
_CLAIMERS = {
    "lib/core/src/orch_provider_park.cpp",  # pickOpenProvider: the walk of a seam about to send
    "lib/harness/src/vision_caption.cpp",  # the caption walk, which releases on a skipped send
    "src/agent/provider_gate.cpp",  # gate::check
}


def test_only_a_seam_about_to_send_asks_the_claiming_question():
    claiming = re.compile(r"providerPark\(\)\s*\.\s*available\(")
    found = set()
    for root in ("src", "lib", "nimbusd/src"):
        for path in (_REPO / root).rglob("*"):
            if path.suffix in {".cpp", ".h"} and claiming.search(path.read_text(errors="ignore")):
                found.add(path.relative_to(_REPO).as_posix())
    assert found <= _CLAIMERS, found - _CLAIMERS


def test_text_to_speech_reads_a_cut_refusal():
    text = (_ADAPTERS / "audio_tts.cpp").read_text()
    body = _function_body(text, "void noteRefusal(")
    assert "voiceRefusalWhy(" in body, "the reason must reach the words, from a body it holds only part of"


def test_the_device_recheck_waits_for_the_owner_and_comes_first():
    text = (_REPO / "src" / "agent" / "orchestrator.cpp").read_text()
    body = _function_body(text, "static void compactTick(")
    ask = body.find("recheckParkedHost()")
    gate = body.find("parkRecheckMayStart(")
    assert ask >= 0, "the device's only call of the idle re-check is gone"
    assert 0 <= gate < ask, "the idle re-check blocks the task that reads the owner's messages"
    assert ask < body.find("canFoldNow()"), "the re-check comes before the fold's gates"
    assert body.count("recheckParkedHost()") == 1
    assert text.count("recheckParkedHost()") == 1
    assert "compactTick();" in text[text.find("pollJobs") :]
