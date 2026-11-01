"""L75 - what the device says about speaking and about itself is backed by what happened (lane QW6).

The owner, 22 Oct: the device "said it got a confirmation from the tool but in practice I got
nothing". Lane VOICE made ``reply.telegram`` voice:true honest. The review found the same family
in the end-of-turn ``tts`` device action, which reported "spoken" and wrote history whether the
speaker played, played silence (volume 0), failed, or Telegram refused the fallback, and which cut
text at 400 BYTES, possibly inside a character, without a word. The owner's "What model are you
running?" (25 Oct) was answered from a prompt that named the raw pinned or first-priority host;
since v4.6.1 each request's input opens with "[THIS TURN RUNS ON] <host> / <model>" for the host
that request goes to (plus why the chosen head is not running, when it is not), and the cached
prompt names no model. The I legs check that line on /api/lastturn and the answer built on it.

Legs (the head is the LAN mock unless noted, so the replies are fixed and cost nothing):

  T1  volume 0: a reply that says it read the text aloud, with a ``tts`` action behind it. The
      speaker would play silence, so nothing is synthesized, the log says ``tts(not-spoken: ...)``
      and the delivered reply carries "(Correction: that was not spoken aloud.)".
  T2  a turn whose only output is that ``tts`` delivers the text with the reason, not silence.
  T6  the same at volume 0 with a reply no claim cue knows (reworded English, French): the failed
      speech still reaches the owner as a plain note on the reply, with the reason, in the same turn.
  T3  voice replies switched off: the same reply is corrected, nothing is delivered for a
      tts-only turn is NOT asserted here (the owner's ruling: a silent no-op, host-tested).
  T4  the speaker cannot play (``FAULT speaker``), so the action falls back to Telegram: a text of
      about 1,000 characters whose 400-byte bound falls inside a 2-byte letter, in a long word with
      no space or sentence end to cut at, is cut hard just before that letter (399 bytes), voiced
      as one voice message whose Telegram message id is in the log, Telegram confirms the message
      exists, and the reply says the spoken version was shortened (and is NOT corrected: a voice
      message with a receipt backs "read it aloud").
  T5  the same fallback to a chat Telegram does not know: ``tts(not-spoken: ...)`` with the
      refusal, and the reply is corrected.
  I1  "What model are you running?" with the pinned host lacking a key: the turn's per-turn line
      names the host it really ran on, and the answer names that model (the concrete id the line
      carries, or the alias it was asked for), never the pin's.
  I2  the same question right after a provider failover (the head answered 529 once, then
      recovered): the line and the answer name the host that ran.
  I3  the same question while the head is closed for credit (parked): the line names the open
      host that answers and says the chosen head is out of credit; the answer names that model,
      and the closed head may be mentioned only as the chosen head that is not running.

Conditions: unit L or unit A in Orchestrator mode with the test console image, the LAN mock
(``custom_backend``), the DEDICATED test bot for T4/T5 (the ``bench`` fixture takes the bench's
tg-bot lock around it) and a Mistral key; I1-I3 use the unit's own cheap head models. Sound stays
at 0 except T4/T5, which set ``sfxVol`` 1 with the speaker faulted (nothing can play) and restore
it. Spend: about 400 characters of Mistral speech and a few short head turns.

Run: python3 -m pytest tests/hil/test_l75_voice_honesty.py -m "hil and net" --allow-hardware -s
"""

from __future__ import annotations

import os
import re
import time
from contextlib import contextmanager

import pytest

from mock_llm import CLAIM_TURNS, mock_llm  # noqa: F401  (mock_llm is a fixture)
from tg_archive_bench import BenchBot
from test_claim_check_bench import SETTLE_S, _replies, _tailed, assistant_replies, mock_head, new_replies  # noqa: F401
from test_claim_check_bench import _turn as mock_turn
from test_l12_mock_llm import _opost, custom_backend  # noqa: F401  (fixture + helper)
from test_l67_voice_delivery import _probe

pytestmark = [pytest.mark.hil, pytest.mark.net, pytest.mark.agent]

SPOKEN_LINE = "(Correction: that was not spoken aloud.)"
VOLUME_ZERO = "tts(not-spoken: the speaker volume is 0, so speech would play as silence)"
BAD_CHAT = "1"  # a chat id Telegram does not know: the upload is refused for real
LIVE_TURN_TIMEOUT_S = 180.0
OWNER_RELOAD_S = 40.0  # one long poll plus slack
# The speech provider T4/T5 use: Mistral by default; NIMBUS_L75_TTS=openai exercises the close-delimited
# raw body (the short-clip probe) on a real OpenAI stream.
TTS_PROVIDER = os.environ.get("NIMBUS_L75_TTS", "mistral")

DEVICE_TTS_RE = re.compile(r"orchestrator: device\[(tts[^\]]*)\]")
TG_FALLBACK_RE = re.compile(r"tts\(telegram message_id (\d+)\)")
SEND_OK_RE = re.compile(r"telegram: sendAudio ok \(message_id (\d+), (\d+) bytes\)")
SHORTENED_RE = re.compile(r"\(Note: the spoken version was shortened to the first (\d+) of its (\d+) characters\.\)")
TURN_END_RE = re.compile(r"turn end: \w+ host=(\w+)")
STARTING_ON_RE = re.compile(r"(?:orchestrator|fabric): (\w+) is parked \((\w+)\)(?: -)? starting on (\w+)")


# ---- pure helpers (host-tested in test_l75_voice_honesty_host.py) -------------------------


def new_lines(before: list, after: list) -> list:
    """The log lines in `after` that `before` did not hold, in order (the ring may have
    dropped the oldest lines of `before`, so this is a set difference, not a suffix)."""
    seen = set(before)
    return [ln for ln in after if ln not in seen]


def normalized(text: str) -> str:
    """Lowercase letters and digits only: 'Claude Haiku 5.5' and 'claude-haiku-5-5' meet."""
    return re.sub(r"[^a-z0-9]", "", text.lower())


def names_model(reply: str, model: str) -> bool:
    """The reply names the model: its id, or its words (the model may say 'GPT 5.6 Luna')."""
    return bool(model) and normalized(model) in normalized(reply)


# The per-turn line a request's input opens with (served_model.h runsOnLine), as /api/lastturn
# shows the input the turn's last request sent. The model id may hold dots ("gpt-5.6-luna"), so its
# closing dot is the one followed by a space or the end of the line.
RUNS_ON_RE = re.compile(
    r"^\[THIS TURN RUNS ON\] (?P<host>[\w.-]+) / (?P<model>\S+?)(?: \(asked for (?P<asked>\S+?)\))?\.(?= |$)"
    r"(?P<rest>.*)$",
    re.M,
)
LASTTURN_HOST_RE = re.compile(r"^host: (\S+)$", re.M)
ALIAS_NOTE = "That is an alias: add no version, size or family from memory."  # served_model.h kRunsOnAliasNote


def runs_on(page: str) -> dict:
    """The per-turn line of a /api/lastturn page: its host, its model (the concrete id when the
    device knows one), the model asked for (the same when they do not differ), whether it carries
    the unresolved-alias note, and the chosen-head fact ("" when the chosen head ran the turn).
    {} when the page holds no line."""
    m = RUNS_ON_RE.search(page)
    if not m:
        return {}
    rest = m.group("rest")
    cut = rest.find(" When asked which model you run")
    fact = (rest[:cut] if cut >= 0 else rest).strip()
    alias = fact.startswith(ALIAS_NOTE)
    return {
        "host": m.group("host"),
        "model": m.group("model"),
        "asked": m.group("asked") or m.group("model"),
        "alias": alias,
        "fact": fact[len(ALIAS_NOTE) :].strip() if alias else fact,
    }


def names_line_model(reply: str, line: dict) -> bool:
    """The reply names the model the line gave: the concrete id or the alias it was asked for."""
    return bool(line) and (names_model(reply, line["model"]) or names_model(reply, line["asked"]))


DETOUR_RE = re.compile(
    r"credit|paused|closed|unavailable|returns|chosen|instead|fell back|failed over|fallback|for now|temporar", re.I
)


def closed_head_named_honestly(reply: str, head_model: str, real_model: str) -> bool:
    """The reply may name the closed head only as the chosen head that is NOT running (the prompt
    now says so in one clause); naming it as the answer would be the bug. Naming no head is fine."""
    if not names_model(reply, head_model) or normalized(real_model) == normalized(head_model):
        return True
    return bool(DETOUR_RE.search(reply))


def valid_utf8_prefix(prefix: str, whole: str) -> bool:
    """`prefix` is a prefix of `whole` within the device's 400-byte bound. (A decoded Python
    string is whole characters by construction; hard_cut_inside_a_letter is what shows the device
    did not cut inside one.)"""
    return whole.startswith(prefix) and len(prefix.encode("utf-8")) <= 400


def hard_cut_inside_a_letter(prefix: str, whole: str) -> bool:
    """The voiced prefix stopped at a multi-byte letter that straddles the 400-byte bound: it is
    under 400 bytes, the next character of the whole text is multi-byte and runs past byte 400.
    A byte cut would have kept a dangling lead byte (the decoded row would not be a prefix)."""
    if not whole.startswith(prefix) or len(prefix) >= len(whole):
        return False
    size = len(prefix.encode("utf-8"))
    nxt = len(whole[len(prefix)].encode("utf-8"))
    return size < 400 and nxt > 1 and size + nxt > 400


def ran_host(lines: list) -> str:
    """The host the last turn in `lines` ended on (the device's 'turn end: ok host=<h>' line)."""
    hosts = [m.group(1) for ln in lines for m in [TURN_END_RE.search(ln)] if m]
    return hosts[-1] if hosts else ""


def parked_start(lines: list) -> str:
    """The host a parked head's turn started on, from the device's own log line."""
    for ln in lines:
        m = STARTING_ON_RE.search(ln)
        if m:
            return m.group(3)
    return ""


# ---- device helpers ------------------------------------------------------------------------


def _log(be) -> list:
    return be.net.get("/api/log", ip=be.ip, timeout=10.0).text.splitlines()


def _post(be, **fields) -> None:
    r = _opost(be.net, be.ip, be.tok, {k: str(v) for k, v in fields.items()})
    assert r.status_code == 200, f"/api/orch {sorted(fields)} -> {r.status_code} {r.text[:160]}"


def _web_turn(be, model: str, text: str, timeout: float = 180.0) -> tuple:
    """One head turn on the mock scenario `model` through the web chat (chat "web": an owner turn
    whose voice fallback goes to the OWNER chat). Returns (the reply as the owner received it,
    every new assistant row the turn wrote to the web chat's history). A tts action that voiced
    its text also records that text to history, so a voiced turn writes two rows."""
    be.set_model(model)
    rows_url = f"/api/mem/episodic?session=web&kind=message&limit=20&t={be.tok}"
    before = assistant_replies(be.net.get_json(rows_url, ip=be.ip, timeout=8.0))
    r = be.net.post("/api/chat", {"text": text}, ip=be.ip, timeout=20.0)
    assert r.status_code == 200, f"/api/chat -> {r.status_code} {r.text[:160]}"
    turn = r.json().get("turn")
    end = time.time() + timeout
    reply = ""
    while time.time() < end and not reply:
        g = be.net.get_json(f"/api/chat?turn={turn}", ip=be.ip, timeout=10.0)
        reply = "" if g.get("pending") else str(g.get("reply") or "")
        if not reply:
            time.sleep(2.0)
    assert reply, f"no reply to {model} within {timeout:.0f} s"
    time.sleep(SETTLE_S)
    rows = new_replies(before, assistant_replies(be.net.get_json(rows_url, ip=be.ip, timeout=8.0)))
    return reply, rows


def split_voiced(rows: dict, voiced_start: str) -> tuple:
    """(reply texts, voiced texts) of one turn's new rows: a voiced row starts with the text."""
    voiced = [t for t in rows.values() if t.startswith(voiced_start)]
    return [t for t in rows.values() if not t.startswith(voiced_start)], voiced


def _owners(be) -> list:
    tg = be.net.get_json("/api/telegram", ip=be.ip, timeout=8.0)
    return [str(a["id"]) for a in tg.get("allow", []) if a.get("owner")]


def _role(be, chat: str, owner: bool) -> None:
    r = be.net.post("/api/telegram/role", {"id": chat, "owner": "1" if owner else "0"}, ip=be.ip, timeout=8.0)
    assert r.status_code == 200, f"/api/telegram/role -> {r.status_code} {r.text[:120]}"


@contextmanager
def refused_owner_chat(be, chat: str):
    """The device sends a fallback voice message to the OWNER chat (a web, serial or voice turn
    has no Telegram chat of its own). Make that chat one Telegram does not know, so the upload
    is refused for real, and put the owners back afterwards."""
    owners = _owners(be)
    _post(be, tgAllow=f"{BAD_CHAT},{chat}")
    try:
        _role(be, BAD_CHAT, True)
        for o in owners:
            _role(be, o, False)
        assert _owners(be) == [BAD_CHAT], f"the owner chat did not change: {_owners(be)}"
        time.sleep(OWNER_RELOAD_S)  # the Telegram poll task re-reads the owners between long polls
        yield
    finally:
        for o in owners:
            _role(be, o, True)
        _role(be, BAD_CHAT, False)
        _post(be, tgAllow=chat)
        assert _owners(be) == owners, f"owners not restored: {_owners(be)} != {owners}"
        time.sleep(OWNER_RELOAD_S)


def _prior(be, keys) -> dict:
    o = be.orch()
    out = {}
    for k in keys:
        v = o.get(k)
        out[k] = ("1" if v else "0") if isinstance(v, bool) else str(v if v is not None else "")
    return out


@pytest.fixture
def voice_settings(mock_head):  # noqa: F811 (the imported fixture)
    """Voice replies on, the speaker at volume 0, Mistral voicing; restored afterwards."""
    be = mock_head
    keys = ("ttsOn", "sfxVol", "ttsProv")
    prior = _prior(be, keys)
    _post(be, ttsOn=1, sfxVol=0, ttsProv="mistral")
    try:
        yield be
    finally:
        _post(be, **{k: v for k, v in prior.items() if v != ""})


# ---- T1-T3: the speaker would play silence ---------------------------------------------------


def test_t1_a_spoken_claim_at_volume_zero_is_corrected(voice_settings):
    be = voice_settings
    before, t0 = _log(be), _tailed(be)
    reply = mock_turn(be, "mock-claim-spoken", "say the bench line out loud")
    log = new_lines(before, _log(be))
    assert any(VOLUME_ZERO in ln for ln in log), (
        f"no refusal line; device[...] lines: {[ln for ln in log if 'device[' in ln]}"
    )
    assert not [ln for ln in log if re.search(r"tts: (mistral|openai) wrote", ln)], "text was synthesized at volume 0"
    assert reply == "I read it aloud for you.\n\n" + SPOKEN_LINE, reply
    assert _tailed(be) == t0 + 1
    print(f"[T1] PASS {reply!r}")


def test_t2_a_tts_only_turn_that_was_not_spoken_delivers_the_text(voice_settings):
    be = voice_settings
    before = _log(be)
    try:
        reply = mock_turn(be, "mock-tts-only", "tell me when the kettle is ready")
    except BaseException:
        for ln in new_lines(before, _log(be))[-40:]:
            print(f"[T2 debug] {ln[:200]}")
        raise
    assert "The bench kettle is ready." in reply, reply
    assert "could not be spoken aloud: the speaker volume is 0" in reply, reply
    print(f"[T2] PASS {reply!r}")


@pytest.mark.parametrize("turn", ["mock-tts-reworded", "mock-tts-french"])
def test_t6_a_failed_tts_is_reported_whatever_the_reply_says(voice_settings, turn):
    """The reply claims nothing the claim check knows (a reworded English reply, a French one), yet the
    owner must hear in the same turn that the speech did not happen: a plain note with the reason."""
    be = voice_settings
    reply = mock_turn(be, turn, "say the bench line out loud")
    spoken = CLAIM_TURNS[turn]["reply"]
    # (No log assertion: the ring is ~20 lines and "new" lines are a set difference, so the second
    # parametrization's identical device[...] line would hide behind the first's. The note itself
    # carries the device's reason, which only the volume-0 refusal produces.)
    assert (
        reply
        == f"{spoken}\n\n(Note: that could not be spoken aloud: the speaker volume is 0, so speech would play as silence.)"
    ), reply
    assert SPOKEN_LINE not in reply, f"a claim line where the claim check had no claim: {reply!r}"
    print(f"[T6] PASS {turn}: {reply!r}")


def test_t3_voice_replies_off_still_corrects_a_spoken_claim(voice_settings):
    be = voice_settings
    _post(be, ttsOn=0)
    reply = mock_turn(be, "mock-claim-spoken", "say the bench line out loud")
    assert reply == "I read it aloud for you.\n\n" + SPOKEN_LINE, reply
    print(f"[T3] PASS {reply!r}")


# ---- T4/T5: the speaker is faulted, the action falls back to Telegram -----------------------------


SETTLE_UPTIME_S = 150.0  # the cloud relay and the boot backfill take the radio and the heap for a while


def _settled(be, timeout: float = 240.0) -> None:
    """Wait out the boot: a speech upload during the relay dial and the episodic backfill met a
    TLS allocation failure on a unit that had just restarted (the fixture restarts it)."""
    end = time.time() + timeout
    while time.time() < end:
        try:
            if float(be.state().get("uptimeMs", 0)) / 1000.0 >= SETTLE_UPTIME_S:
                return
        except Exception:  # noqa: BLE001 - the unit may still be rejoining
            pass
        time.sleep(5.0)


@pytest.fixture
def telegram_fallback(mock_head, bench):  # noqa: F811 (the imported fixture)
    """The test bot on the unit with ONLY the test chat allowlisted, Mistral voicing, the speaker
    faulted (nothing can play, so sfxVol 1 is silent) and voice replies on."""
    be = mock_head
    _settled(be)
    keys = ("ttsOn", "sfxVol", "ttsProv", "tgAllow")
    prior = _prior(be, keys)
    chat = str(bench.chat)
    _post(be, ttsOn=1, sfxVol=1, ttsProv=TTS_PROVIDER, tgAllow=chat)
    first = (be.orch().get("tgAllow") or "").split(",")[0].strip()
    assert first == chat, "the first allowlisted chat is not the test chat: refusing to send a voice message"
    out = be.device.cmd("FAULT speaker on", expect="FAULT", timeout=6.0)
    assert "mask=" in out or "speaker" in out.lower(), out
    try:
        yield be, bench
    finally:
        be.device.cmd("FAULT speaker off", expect="FAULT", timeout=6.0)
        _post(be, **{k: v for k, v in prior.items() if v != "" and k != "tgAllow"})
        _post(be, tgAllow=chat)


ENV_FAILURES = ("alloc FAILED", "could not be saved", "HTTP 0", "low memory")


def environment_failed(lines: list) -> bool:
    """The turn met the unit's memory floor, not a voice fault: a failed allocation, a clip that
    could not be written, a Telegram answer that never came."""
    return any(any(m in ln for m in ENV_FAILURES) for ln in lines)


def _tts_lines(log: list) -> list:
    return [m.group(1) for ln in log for m in [DEVICE_TTS_RE.search(ln)] if m]


def _long_turn_with_retry(be, voiced_start: str) -> tuple:
    """Run the long-text turn; retry while the unit's memory floor (not a voice fault) stops the
    upload. Returns (log, tts_lines, reply, voiced texts, tail_before)."""
    for attempt in range(1, 4):
        before, t0 = _log(be), _tailed(be)
        reply, rows = _web_turn(be, "mock-tts-long", "read me the long bench text")
        log = new_lines(before, _log(be))
        lines = _tts_lines(log)
        _, voiced = split_voiced(rows, voiced_start)
        if lines and TG_FALLBACK_RE.search(lines[-1]):
            return log, lines, reply, voiced, t0
        print(f"[T4] attempt {attempt}: {lines[-1:] or 'no tts line'}")
        if not environment_failed(log):
            return log, lines, reply, voiced, t0  # a real failure: the asserts say which
        time.sleep(90.0)  # the unit's memory floor: let the cloud relay and the backfill settle
    pytest.skip("BLOCKED: the unit's internal heap never left its floor for a speech upload (see the log lines)")


def test_t4_the_fallback_delivers_a_cut_text_with_a_receipt(telegram_fallback):
    be, bench = telegram_fallback
    whole = CLAIM_TURNS["mock-tts-long"]["device"][0]["text"]
    assert len(whole.encode("utf-8")) > 800
    log, tts_lines, reply, voiced, t0 = _long_turn_with_retry(be, whole[:24])
    assert tts_lines, f"no device[tts...] line; tail: {log[-15:]}"
    # history holds what was voiced (the cut text), as its own row: a whole character prefix of the text,
    # cut hard just before the 2-byte letter that straddles the 400-byte bound (the text has no space or
    # sentence end to cut at there), so a byte cut would have split that letter
    assert len(voiced) == 1 and valid_utf8_prefix(voiced[0], whole), (
        f"the voiced row is not a cut of the text: {voiced!r}"
    )
    assert hard_cut_inside_a_letter(voiced[0], whole), (
        f"the cut is not the hard one before the straddling letter: {len(voiced[0].encode('utf-8'))} B, "
        f"next {whole[len(voiced[0]) : len(voiced[0]) + 1]!r}"
    )
    m = TG_FALLBACK_RE.search(tts_lines[-1])
    assert m, f"the action did not report a Telegram delivery: {tts_lines}"
    mid = int(m.group(1))
    sent = [int(x.group(1)) for ln in log for x in [SEND_OK_RE.search(ln)] if x]
    assert sent == [mid], f"the upload's receipt {sent} != the action's {mid}"
    assert _probe(BenchBot(str(bench.token), int(str(bench.chat))), mid) == "exists", f"Telegram has no message {mid}"
    assert not [ln for ln in log if "-> /reply.mp3" in ln or "-> /reply.wav" in ln], "the speaker path ran"
    assert any(f"tts: {TTS_PROVIDER} wrote" in ln for ln in log), (
        f"the speech did not come from {TTS_PROVIDER}: {log[-12:]}"
    )
    note = SHORTENED_RE.search(reply)
    assert note, f"no 'shortened' note in the reply: {reply!r}"
    spoken, total = int(note.group(1)), int(note.group(2))
    assert 0 < spoken < total and total == len(whole), (spoken, total, len(whole))
    assert SPOKEN_LINE not in reply, f"a delivered voice message must back 'read it aloud': {reply!r}"
    assert _tailed(be) == t0, "the claim check corrected a reply the receipt backs"
    print(f"[T4] PASS message_id {mid} confirmed by Telegram; {spoken} of {total} characters voiced; reply {reply!r}")


def test_t5_a_refused_fallback_is_reported_and_the_claim_corrected(telegram_fallback):
    be, bench = telegram_fallback
    chat = str(bench.chat)
    with refused_owner_chat(be, chat):  # the fallback goes to the owner chat, which Telegram refuses
        before = _log(be)
        reply, _ = _web_turn(be, "mock-claim-spoken", "say the bench line out loud")
        log = new_lines(before, _log(be))
    tts_lines = [m.group(1) for ln in log for m in [DEVICE_TTS_RE.search(ln)] if m]
    assert tts_lines and tts_lines[-1].startswith("tts(not-spoken: "), f"{tts_lines}"
    assert "the Telegram voice message did not arrive" in tts_lines[-1], tts_lines[-1]
    assert any("telegram: sendAudio FAIL" in ln for ln in log), "no refused upload in the log"
    assert reply == "I read it aloud for you.\n\n" + SPOKEN_LINE, reply
    print(f"[T5] PASS {tts_lines[-1][:140]!r}")


# ---- I1-I3: "What model are you running?" ---------------------------------------------------------

ASK_MODEL = "What model are you running right now? Reply in one short sentence naming the provider and the model."
BILLING_400 = (
    '{"type":"error","error":{"type":"invalid_request_error","message":"Your credit balance is too low to '
    'access the API. Please go to Plans & Billing to upgrade or purchase credits."}}'
)
OVERLOADED_529 = '{"type":"error","error":{"type":"overloaded_error","message":"Overloaded"}}'


def _live_turn(be, text: str) -> tuple:
    """One head turn on the unit's real provider (serial chat); returns (reply, new log lines).
    A turn that met the unit's memory floor ("ran out of working memory") is asked again after a
    pause, up to three times: the answer to judge is the one a healthy turn gives."""
    for attempt in range(3):
        before_log = _log(be)
        before = _replies(be)
        try:
            be.device.cmd(f"TURN {text}", expect="TURN <-", timeout=6.0)
        except Exception:  # noqa: BLE001 - the echo is advisory; the turn still fires
            pass
        end = time.time() + LIVE_TURN_TIMEOUT_S
        while time.time() < end and not new_replies(before, _replies(be)):
            time.sleep(3.0)
        time.sleep(SETTLE_S)
        got = new_replies(before, _replies(be))
        assert got, f"no reply to {text!r} within {LIVE_TURN_TIMEOUT_S:.0f} s"
        log = new_lines(before_log, _log(be))
        for ln in log:
            if any(k in ln for k in ("turn host=", "turn end:", "fabric:", "failover", "parked", "PROVFAULT")):
                print(f"[live log] {ln[:200]}")
        reply = next(iter(got.values()))
        if "working memory" not in reply and "low memory" not in reply:
            return reply, log
        print(f"[live] attempt {attempt + 1} met the memory floor: {reply[:100]!r}")
        time.sleep(60.0)
    pytest.skip("BLOCKED: the unit's heap floor failed three turns in a row")


def _providers(be) -> dict:
    return be.orch().get("providers") or {}


def _ran_line(be, ran: str) -> dict:
    """The per-turn line of the turn that just ran on `ran`, from /api/lastturn (a single global
    slot: its header must name the same host, or another turn wrote it since)."""
    page = be.net.get("/api/lastturn", ip=be.ip, timeout=10.0).text
    head = LASTTURN_HOST_RE.search(page)
    assert head and head.group(1) == ran, f"/api/lastturn is another turn's (host {head and head.group(1)!r})"
    line = runs_on(page)
    print(f"[live line] {line}")
    assert line, "the turn's input carries no [THIS TURN RUNS ON] line"
    assert line["host"] == ran, f"the line names {line['host']!r}, the turn ran on {ran!r}"
    return line


@pytest.fixture
def live_head(custom_backend):  # noqa: F811
    """The unit's own head (not the mock): the pin and the cheap models restored afterwards."""
    be = custom_backend
    keys = ("orchHost", "provPrio")
    prior = _prior(be, keys)
    prior_models = {f"orchM_{p}": str(v.get("orchModel") or "") for p, v in _providers(be).items()}
    # A failover needs somewhere to fail over to: the provider priority names the alternates
    # (a bench unit may list only its head). The heads' own cheap models are what each runs.
    _post(be, provPrio="anthropic,openai,mistral")
    try:
        yield be
    finally:
        be.device.cmd("PROVFAULT off", expect="PROVFAULT", timeout=5.0)
        fields = {k: v for k, v in {**prior, **prior_models}.items() if v != ""}
        _post(be, **fields)


def test_i1_the_answer_names_the_host_the_turn_ran_on_not_the_pin(live_head):
    be = live_head
    provs = _providers(be)
    keyless = [p for p, v in provs.items() if not v.get("hasKey")]
    if not keyless:
        pytest.skip("every provider has a key on this unit: no keyless pin to test")
    pin = keyless[0]
    _post(be, orchHost=pin)
    reply, log = _live_turn(be, ASK_MODEL)
    ran = ran_host(log)
    assert ran and ran != pin, f"the turn did not run on a keyed host: host={ran!r} pin={pin!r}; {log[-8:]}"
    line = _ran_line(be, ran)
    real = str(provs[ran].get("orchModel") or "")
    assert line["asked"] == real, f"the line names {line['asked']!r}, {ran} is set to {real!r}"
    pinned_model = str(provs[pin].get("orchModel") or "")
    assert names_line_model(reply, line), f"the reply does not name the model that ran ({line}): {reply!r}"
    assert not names_model(reply, pinned_model) or normalized(pinned_model) in normalized(real), (
        f"the reply names the keyless pin's model {pinned_model!r}: {reply!r}"
    )
    print(f"[I1] PASS pin={pin} ran={ran}/{real}; reply {reply!r}")


def test_i2_the_answer_after_a_failover_is_exact(live_head):
    be = live_head
    provs = _providers(be)
    head = "anthropic"
    if not provs.get(head, {}).get("hasKey"):
        pytest.skip("no Anthropic key on this unit")
    _post(be, orchHost=head)
    be.device.cmd(f"PROVFAULT api.anthropic.com 529 {OVERLOADED_529}", expect="PROVFAULT", timeout=5.0)
    try:
        first, log1 = _live_turn(be, "Say hello in one short sentence.")
    finally:
        be.device.cmd("PROVFAULT off", expect="PROVFAULT", timeout=5.0)
    assert any("failover" in ln or "switching" in ln or "fabric: " in ln for ln in log1), (
        f"the 529 did not cause a switch; log: {log1[-12:]}"
    )
    reply, log = _live_turn(be, ASK_MODEL)
    ran = ran_host(log)
    assert ran, f"no turn end line; log: {log[-8:]}"
    line = _ran_line(be, ran)
    assert names_line_model(reply, line), (
        f"after a failover the reply does not name the model that ran ({line}): {reply!r}"
    )
    print(f"[I2] PASS failover turn: {first!r}; then ran={ran} line={line}; reply {reply!r}")


def test_i3_the_answer_names_the_open_host_while_the_head_is_closed(live_head):
    be = live_head
    provs = _providers(be)
    if not provs.get("anthropic", {}).get("hasKey"):
        pytest.skip("no Anthropic key on this unit")
    _post(be, orchHost="anthropic")
    be.device.cmd(f"PROVFAULT api.anthropic.com 400 {BILLING_400}", expect="PROVFAULT", timeout=5.0)
    try:
        _live_turn(be, "Say hello in one short sentence.")  # refused for credit: anthropic is parked
        be.device.cmd("PROVFAULT off", expect="PROVFAULT", timeout=5.0)
        reply, log = _live_turn(be, ASK_MODEL)
        alt = parked_start(log)
        assert alt, f"the turn did not start past the parked head; log: {log[-12:]}"
        line = _ran_line(be, ran_host(log) or alt)
        assert line["host"] == alt, f"the line names {line['host']!r}, the turn started on {alt!r}"
        assert "the chosen head" in line["fact"] and "out of credit" in line["fact"], (
            f"the line does not say why the chosen head is not running: {line}"
        )
        assert names_line_model(reply, line), f"the reply does not name the open host's model ({line}): {reply!r}"
        head_model = str(provs["anthropic"].get("orchModel") or "")
        assert closed_head_named_honestly(reply, head_model, line["model"]), (
            f"the reply names the closed head as running: {reply!r}"
        )
        print(f"[I3] PASS head closed, ran on {alt} line={line}; reply {reply!r}")
    finally:
        be.device.cmd("PROVFAULT off", expect="PROVFAULT", timeout=5.0)
        # a park is kept on flash (it survives a restart): end it, or the next leg finds the head closed
        be.device.cmd("PARK clear", expect="PARK cleared", timeout=8.0)
