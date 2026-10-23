"""L67 - a Telegram voice reply arrives, with Telegram's receipt, or says why not.

On 22 Oct 2026 (v4.5.16) a routine turn called ``reply.telegram`` with ``voice:true``.
The call played on the device speaker first (Telegram was only the speaker's
fallback), the speaker driver could not start and nobody heard of it, the clip had
been cut at a fixed 25 s download window, the tool answered "sent to Telegram" and
held the memory card lock for 25 s, and the reply told the owner a voice message was
on its way. Nothing reached Telegram.

Now a Telegram voice request goes to Telegram only: the text is cut into voice
messages, each is synthesized to its own flash file, uploaded at once, and checked
against Telegram's answer; the tool's result carries Telegram's message ids or says
why nothing arrived. These legs prove it on a bench unit, with Telegram itself as
the witness:

  V1  delivered: a Telegram turn in the test chat asks for a voice reply. The device
      logs ``telegram: sendAudio ok (message_id N, B bytes)`` and the tool's receipt;
      Telegram confirms message N exists in the test chat (an edit of its caption to
      the same empty caption answers "message is not modified", while a made-up id
      answers "not found", so the probe tells the two apart); the clip went to a
      ``/tgv-`` file, never ``/reply.mp3``, and nothing touched the speaker; the reply
      tool held no card lock (no "held the card lock" line, the loop guard's blocking
      count unchanged); the claim check passed.
  V2  forced upload failure: the same request aimed at an allowlisted chat Telegram
      does not know (chat id 1). ``telegram: sendAudio FAIL (...)``, the tool says
      nothing was delivered, and the delivered reply does not claim a voice message
      (or carries the device's correction line).
  V3  long text: about 1,500 characters become several voice messages, each with its
      own receipt, each confirmed by Telegram.

Conditions: unit L or unit A, Orchestrator mode, an Anthropic key (the head is pinned
to ``NIMBUS_VOICE_MODEL``, default claude-haiku-5-5) and a Mistral key (the incident's
voice provider; the legs pin ``ttsProv=mistral``). The ``bench`` fixture puts the
DEDICATED test bot on the unit and clears it afterwards; the runner holds the
program's ``locks/tg-bot`` around the run (one Telegram leg at a time). Sound stays
at 0 (``sfxVol`` 0): voice replies (``ttsOn``) are switched on for the legs and every
changed setting is restored. Spend: about three head turns on the cheap model and
about 1,700 characters of Mistral speech.

Run: python3 -m pytest tests/hil/test_l67_voice_delivery.py -m "hil and net" --allow-hardware -s
"""

from __future__ import annotations

import os
import re
import time

import pytest

import scenarios_complex as S
from tg_archive_bench import BenchBot
from test_l48_albums import _cmd_paced

pytestmark = [pytest.mark.hil, pytest.mark.net, pytest.mark.agent]

MODEL = os.environ.get("NIMBUS_VOICE_MODEL", "claude-haiku-5-5")
RUN = f"V{int(time.time()) % 100000}"  # unique per run: the duplicate guard never drops a rerun
TURN_TIMEOUT_S = 300.0
BAD_CHAT = "1"  # a chat id Telegram does not know: the upload is refused for real

SEND_OK_RE = re.compile(r"telegram: sendAudio ok \(message_id (\d+), (\d+) bytes\)")
SEND_FAIL_RE = re.compile(r"telegram: sendAudio FAIL \((.+)\)")
RECEIPT_ONE_RE = re.compile(r"voice: delivered to Telegram as a voice message \(message_id (\d+),")
RECEIPT_MANY_RE = re.compile(r"voice: delivered to Telegram as \d+ voice messages \(message_id ([\d, ]+);")
TTS_RE = re.compile(r"tts: (?:mistral|openai) wrote (\d+) bytes -> (\S+)")
SPEND_RE = re.compile(r"fabric-spend: (\S+) in=(\d+) out=(\d+) cR=(\d+) cW=(\d+)")
CORRECTION = "(Correction: no voice message was delivered.)"

LONG_TEXT = (
    "Good evening. Here is the bench summary for tonight, read aloud so you can listen while "
    "you get ready. The weather tomorrow looks dry and mild, with a light breeze from the west "
    "and highs around fourteen degrees, so a light jacket should be enough for the walk in. "
    "Your calendar has two meetings in the morning: the planning review at nine thirty and a "
    "short call with the design team at eleven. The afternoon is clear until the dentist at "
    "three, which is about twenty minutes away by bus. Your step count today was a little "
    "under eight thousand, slightly below your weekly average, and you logged two meals. "
    "There are three reminders for the evening: water the plants on the balcony, call your "
    "sister about the plans for the weekend, and put the recycling out before ten. The "
    "parcel you were expecting is out for delivery and should arrive tomorrow before noon. "
    "Finally, the library books are due on Friday, and the renewal can be done online if you "
    "need more time. That is everything for today. Sleep well, and have a good morning."
)


# ---- pure helpers (host-tested below) ----------------------------------------


def tg_page(chat: str, update_id: int, text: str) -> str:
    """One raw getUpdates page with a text message from `chat` in `chat`."""
    import json

    return json.dumps(
        {
            "ok": True,
            "result": [
                {
                    "update_id": update_id,
                    "message": {
                        "message_id": update_id,
                        "date": int(time.time()),
                        "chat": {"id": int(chat), "type": "private"},
                        "from": {"id": int(chat), "first_name": "Bench"},
                        "text": text,
                    },
                }
            ],
        },
        separators=(",", ":"),
    )


def receipt_ids(lines: list) -> list:
    """Telegram's message ids from the device's ``telegram: sendAudio ok`` lines, in order."""
    return [int(m.group(1)) for ln in lines for m in [SEND_OK_RE.search(ln)] if m]


def tool_receipt_ids(lines: list) -> list:
    """The message ids the tool's own result named (the ``voice: delivered ...`` line)."""
    for ln in lines:
        one = RECEIPT_ONE_RE.search(ln)
        if one:
            return [int(one.group(1))]
        many = RECEIPT_MANY_RE.search(ln)
        if many:
            return [int(x) for x in many.group(1).replace(" ", "").split(",") if x]
    return []


def speaker_touches(lines: list) -> list:
    """Lines that show the speaker path ran: a clip for the speaker, a queued reply,
    a speaker open or its failure."""
    marks = ("-> /reply.mp3", "-> /reply.wav", "speak: ", "speaker: ", "sfx: /reply")
    return [ln for ln in lines if any(m in ln for m in marks)]


def probe_verdict(error_text: str) -> str:
    """What an editMessageCaption error says about the message: 'exists' when Telegram
    found it and nothing changed, 'missing' when there is no such message, else the text."""
    low = error_text.lower()
    if "message is not modified" in low:
        return "exists"
    if "message to edit not found" in low or "message_id_invalid" in low or "message not found" in low:
        return "missing"
    return error_text


def spend_of(lines: list) -> list:
    """(model, in, out, cacheRead, cacheWrite) per provider-reported usage line."""
    return [
        (m.group(1), int(m.group(2)), int(m.group(3)), int(m.group(4)), int(m.group(5)))
        for ln in lines
        for m in [SPEND_RE.search(ln)]
        if m
    ]


# ---- device helpers ----------------------------------------------------------


def _probe(bot: BenchBot, message_id: int) -> str:
    """Ask Telegram whether `message_id` exists in the test chat, without changing it: an
    edit to what it already holds (the same empty caption, else no keyboard). Returns
    'exists', 'missing' or Telegram's text."""
    verdict = ""
    for method, extra in (("editMessageCaption", {"caption": ""}), ("editMessageReplyMarkup", {})):
        try:
            bot._call(method, {"chat_id": bot.chat, "message_id": message_id, **extra})
            return "exists"  # Telegram accepted an edit that changes nothing: the message is there
        except RuntimeError as exc:
            verdict = probe_verdict(str(exc))
        if verdict in ("exists", "missing"):
            return verdict
    return verdict


INJECT_DONE = "telegram: injected page:"
TURN_START = "orchestrator: turn host="


def turn_window(lines: list) -> list:
    """The injected page's own turn: from the last turn start before the page's
    "injected page" line to the end (the text reply is sent after that line). Turns run
    one at a time, so an earlier turn (a real message the test bot still had queued)
    ends before this one starts and is cut off here."""
    done = next((i for i, ln in enumerate(lines) if INJECT_DONE in ln), len(lines))
    start = max((i for i, ln in enumerate(lines[:done]) if TURN_START in ln), default=0)
    return lines[start:]


def _turn(device, chat: str, text: str, update_id: int) -> list:
    """Inject one Telegram message from the test chat and return every console line from
    the inject through the text reply sent after the page (callers take turn_window for
    the page's own turn; the speaker and lock checks read all of it, stricter)."""
    for _ in range(30):
        line = _cmd_paced(device, "TGINJECT " + tg_page(chat, update_id, text), "TGINJECT", timeout=20.0)
        if "busy" not in line:
            break
        time.sleep(3.0)
    assert "queued" in line or "injected=1" in line, f"the page was not taken: {line}"
    out, end = [], time.time() + TURN_TIMEOUT_S
    while time.time() < end:
        got = device.expect_re(r".", timeout=max(1.0, end - time.time())).string
        out.append(got)
        if INJECT_DONE in got:
            settle = time.time() + 10.0  # the text reply goes out after the page
            while time.time() < settle:
                try:
                    out.append(device.expect_re(r".", timeout=max(0.5, settle - time.time())).string)
                except Exception:  # noqa: BLE001 - a quiet console ends the window early
                    break
            return out
    pytest.fail(f"the injected page did not finish within {TURN_TIMEOUT_S:.0f} s; tail: {out[-20:]}")


def _loopguard(device) -> int:
    m = device.cmd_re("LOOPGUARD?", r"LOOPGUARD blocking=(\d+)", timeout=6.0)
    return int(m.group(1))


def _reply_text(lines: list) -> str:
    """The delivered reply as the device logged it (the turn's JSON reply line)."""
    for ln in lines:
        if "orchestrator: turn -> " in ln:
            return ln.split("orchestrator: turn -> ", 1)[1]
    return ""


BENCH_SETTINGS = {
    "orchHost": "anthropic",
    "orchLoop": "1",
    "orchM_anthropic": MODEL,
    "ttsProv": "mistral",
    "ttsOn": "1",
    "sfxVol": "0",
}


def bench_prior(orch: dict) -> dict:
    """The unit's settings the legs change (BENCH_SETTINGS), read from /api/orch, to
    restore afterwards; skips, saying so, when the head or the voice provider has no key."""
    providers = orch.get("providers") or {}
    anth = providers.get("anthropic") or {}
    if not anth.get("hasKey"):
        pytest.skip("no Anthropic key on the unit")
    if not (providers.get("mistral") or {}).get("hasKey"):
        pytest.skip("no Mistral key on the unit (the incident's voice provider)")
    return {
        "orchHost": str(orch.get("orchHost") or ""),
        "orchLoop": "1" if orch.get("orchLoop") else "0",
        "orchM_anthropic": str(anth.get("orchModel") or ""),
        "ttsProv": str(orch.get("ttsProv") or ""),
        "ttsOn": "1" if orch.get("ttsOn") else "0",
        "sfxVol": str(orch.get("sfxVol", 0)),
    }


def _restore(net, bench, prior: dict, ceiling) -> None:
    net.post("/api/orch", {k: v for k, v in prior.items() if v != ""}, timeout=8.0)
    net.post("/api/orch", {"tgAllow": str(bench.chat)}, timeout=8.0)
    if ceiling:
        net.post("/api/orch", {"spendCeil": ceiling}, timeout=8.0)


@pytest.fixture
def voice_bench(bench, net):
    """The bench unit with the test bot, the head on the cheap model, Mistral voicing,
    voice replies on and sound at 0; every changed setting restored."""
    prior = bench_prior(net.get_json("/api/orch", timeout=8.0))
    r = net.post("/api/orch", BENCH_SETTINGS, timeout=8.0)
    assert r.status_code == 200, f"bench settings -> {r.status_code} {r.text[:200]}"
    ceiling = _lift_spend_freeze(net)
    # The token swap resets the offset, so a message still queued for the test bot runs
    # as a turn first: let it finish before the legs inject theirs.
    time.sleep(20.0)
    try:
        yield bench
    finally:
        _restore(net, bench, prior, ceiling)


def spend_lift(spend: dict):
    """(the ceiling to restore, the ceiling to set for the legs) when the unit's monthly
    spend ceiling has paused paid calls (a bench unit's month fills up over many runs);
    None when nothing is paused. The legs need about 5 cents."""
    if not spend.get("frozen"):
        return None
    prior = str(spend["limitCents"]) if spend.get("limitCents") else "off"
    return prior, str(int(spend.get("spentCents", 0)) + 500)


def _lift_spend_freeze(net):
    """Lift a paused monthly ceiling for the legs (restored afterwards); skip, saying so,
    when an owner's per-provider limit is what pauses Anthropic or Mistral."""
    lift = spend_lift(net.get_json("/api/state", timeout=8.0).get("spend") or {})
    if lift:
        r = net.post("/api/orch", {"spendCeil": lift[1]}, timeout=8.0)
        assert r.status_code == 200, f"spendCeil -> {r.status_code} {r.text[:200]}"
        print(f"[bench] the monthly spend ceiling had paused paid calls: raised to {lift[1]} cents for the legs")
    spend = net.get_json("/api/state", timeout=8.0).get("spend") or {}
    capped = [
        p.get("p") for p in spend.get("providers", []) if p.get("frozen") and p.get("p") in ("anthropic", "mistral")
    ]
    if spend.get("frozen") or capped:
        if lift:
            net.post("/api/orch", {"spendCeil": lift[0]}, timeout=8.0)
        pytest.skip(f"paid calls stay paused on this unit (provider limits: {capped or 'none'})")
    return lift[0] if lift else None


EVIDENCE_MARKS = (
    "tts: ",
    "telegram: send",
    "voice: ",
    "claims: ",
    "tg_poll stack",
    "held the card lock",
    "speak: ",
    "speaker: ",
    "heap: alloc FAILED",
    "turn end:",
    "fabric-spend:",
)


def _report(tag: str, lines: list) -> None:
    for ln in lines:
        if any(m in ln for m in EVIDENCE_MARKS):
            print(f"[{tag}] log: {ln[:200]}")
    for model, i, o, cr, cw in spend_of(lines):
        print(f"[{tag}] usage {model} in={i} out={o} cR={cr} cW={cw}")
    chars = [int(m.group(1)) for ln in lines for m in [TTS_RE.search(ln)] if m]
    print(f"[{tag}] tts clips bytes={chars}")


def test_v1_a_voice_reply_reaches_telegram_with_a_receipt(device, voice_bench):
    bot = BenchBot(str(voice_bench.token), int(str(voice_bench.chat)))
    chat = str(voice_bench.chat)
    blocking0 = _loopguard(device)
    lines = _turn(
        device,
        chat,
        f"{RUN} bench: send me this as a Telegram voice message, using reply.telegram with voice set "
        "to true and no chat_id: 'This is the bench voice check. The weather tomorrow is dry and "
        "mild.' Then reply with one short sentence saying what the tool returned.",
        int(time.time()) % 1000000 + 5000000,
    )
    win = turn_window(lines)
    _report("V1", win)
    ids = receipt_ids(win)
    assert ids, f"no 'telegram: sendAudio ok (message_id N, B bytes)' line; tail: {win[-30:]}"
    assert tool_receipt_ids(win) == ids, f"the tool's receipt {tool_receipt_ids(win)} != the upload's {ids}"
    for mid in ids:
        assert _probe(bot, mid) == "exists", f"Telegram does not know message {mid} in the test chat"
    assert _probe(bot, ids[-1] + 100000) == "missing", "the probe cannot tell a missing message apart"
    clips = [m.group(2) for ln in win for m in [TTS_RE.search(ln)] if m]
    assert clips and all(c.startswith("/tgv-") for c in clips), f"the voice clip was not a Telegram temp file: {clips}"
    assert not speaker_touches(lines), f"the speaker path ran for a Telegram request: {speaker_touches(lines)}"
    assert not [ln for ln in lines if "held the card lock" in ln], "a tool held the card lock through the voice reply"
    assert _loopguard(device) == blocking0, "the main loop waited for the card lock during the voice reply"
    assert any("claims: ok" in ln for ln in win), "the claim check did not pass the receipted reply"
    print(f"[V1] PASS message_id(s) {ids} confirmed by Telegram")


def test_v2_a_refused_upload_is_reported_honestly(device, voice_bench, net):
    chat = str(voice_bench.chat)
    r = net.post("/api/orch", {"tgAllow": f"{chat},{BAD_CHAT}"}, timeout=8.0)
    assert r.status_code == 200, f"allowlist -> {r.status_code}"
    lines = _turn(
        device,
        chat,
        f"{RUN} bench: use reply.telegram with chat_id \"{BAD_CHAT}\" and voice set to true to send the "
        "words 'bench failure check' as a voice message. Then tell me in one sentence exactly what "
        "the tool returned.",
        int(time.time()) % 1000000 + 6000000,
    )
    win = turn_window(lines)
    _report("V2", win)
    fails = [m.group(1) for ln in win for m in [SEND_FAIL_RE.search(ln)] if m]
    assert fails, f"no 'telegram: sendAudio FAIL (...)' line; tail: {win[-30:]}"
    assert any("reached Telegram; stopped" in ln or "voice: not sent" in ln for ln in win), "no voice failure line"
    assert not receipt_ids(win), "an upload to an unknown chat reported a receipt"
    assert not speaker_touches(lines), f"the speaker path ran for a Telegram request: {speaker_touches(lines)}"
    reply = _reply_text(win)
    b = type("B", (), {"tool_rows": []})()
    lies = S._voice_lies(reply.lower(), b)
    assert not lies or CORRECTION in reply, (
        f"the reply claims a voice message that never arrived: {lies}: {reply[:300]}"
    )
    print(f"[V2] PASS refusal {fails[0]!r}; reply: {reply[:200]}")


def test_v3_long_text_becomes_several_receipted_voice_messages(device, voice_bench):
    bot = BenchBot(str(voice_bench.token), int(str(voice_bench.chat)))
    chat = str(voice_bench.chat)
    lines = _turn(
        device,
        chat,
        f"{RUN} bench: send the following text to me as a Telegram voice message with reply.telegram "
        "(voice true, no chat_id), passing the text exactly as written, without shortening or "
        f"summarizing it: {LONG_TEXT}",
        int(time.time()) % 1000000 + 7000000,
    )
    win = turn_window(lines)
    _report("V3", win)
    ids = receipt_ids(win)
    if len(ids) == 1:
        pytest.skip("the model shortened the text into one voice message, so the split was not exercised")
    assert len(ids) >= 2, f"no voice messages arrived for the long text; tail: {win[-30:]}"
    assert tool_receipt_ids(win) == ids, f"the tool's receipts {tool_receipt_ids(win)} != the uploads' {ids}"
    for mid in ids:
        assert _probe(bot, mid) == "exists", f"Telegram does not know message {mid} in the test chat"
    assert not speaker_touches(lines), f"the speaker path ran for a Telegram request: {speaker_touches(lines)}"
    print(f"[V3] PASS {len(ids)} voice messages, message_id(s) {ids} confirmed by Telegram")
