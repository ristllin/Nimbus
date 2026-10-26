"""L48 (coalesce window) - a text and the photo sent seconds after it are one turn.

25 Oct 2026: the owner sent "Without the mac and cheese. Just the Sunday roast" and the
plate's photo 5 s later by Telegram's dates (no media group). The device answered the
text alone ("If a photo or message was meant to arrive, it didn't reach me"), then the
photo as a second turn. The settle rule (``lib/core/include/nimbus/net/tg_settle.h``,
host-tested in ``test/test_tg_settle`` and end to end in ``test/test_tg_burst``) now
holds a plain text for 6 s for what follows it (a photo, file or voice note), shows the
chat Telegram's typing indicator meanwhile, never holds a command, and a photo that
misses the window is the next turn, told the earlier answer is already in the chat.

No phone and no user account can message the test bot, so these legs replay the
sequence through TGINJECT pages marked ``settle`` (``{"ok":true,"settle":true,...}``,
test builds): the device holds them by the same settle rule as a polled page, with its
own timing, and each later settle page joins the held one. Replies go out through the
dedicated test bot (the ``bench`` fixture: ``locks/tg-bot`` taken, the token cleared in
a finally). Sound stays at 0. A paused monthly spend ceiling is lifted for the legs and
put back after.

Unit A (SD card, TEST build, the head on ``claude-haiku-5-5``, a few cents a run):

* ``test_l48w_sunday_roast_is_one_reply``: the text, 3 s, the photo (a drawn plate, each
  food labeled, the meat a roast lamb the text never names): one settle dispatch of 2
  messages run as 1 turn, one user row holding the text before ``[image 1/1``, one
  reply, and that reply names the lamb (it read the photo) and does not say anything
  is missing (printed for the PR).
* ``test_l48w_late_photo_knows_the_answer``: the photo sent while the text's turn runs:
  two turns, and the second is told the text's answer is already in the chat (its log
  line); both replies are printed.
* ``test_l48w_plain_text_latency``: a plain text with nothing after it, three times: the
  settle hold (device log) is at least the 6 s window and at most the window plus one
  re-poll, and the typing indicator was shown for it; the hold and the reply time are
  printed (the before/after table in the PR).
* ``test_l48w_text_then_voice_is_one_turn``: a text, 3 s, a voice note (spoken words made
  with macOS ``say`` and ffmpeg): one turn, the text first, then ``(voice note)`` and the
  transcript; one reply (printed).

Run (unit A, ``NIMBUS_BENCH_LOCKS=<program>/locks``)::

    python3 -m pytest tests/hil/test_l48_coalesce_window.py --allow-hardware -s

The pure helpers are host-tested at the bottom (``-m host``).
"""

from __future__ import annotations

import io
import json
import re
import time
from typing import List, Optional, Tuple

import pytest

import tg_archive_bench as tb
from device import ExpectTimeout

MODEL = "claude-haiku-5-5"
TEXT_WINDOW_MS = 6000  # nimbus::net::kSettleQuietTextMs
REPOLL_MS = 1000  # nimbus::net::kSettleRepollMs
STAGE_PROJECT = "l48w"
GAP_S = 3.0  # the replay's text-to-photo gap (the DoD's 3 s)
DISPATCH_RE = re.compile(r"settle dispatch \(injected\) n=(\d+) after (\d+) ms \(([^)\s]+)\)")
RAN_RE = re.compile(r"settled page ran: (\d+) messages? in (\d+) turn")
CROSSED_LOG = "was sent before its chat's last answer went out"
TYPING_OK = "typing shown for a held text (ok)"
SPEND_RE = re.compile(r"fabric-spend: (\S+) in=(\d+) out=(\d+) cR=(\d+) cW=(\d+)")
MISSING_RE = re.compile(
    r"(did ?n.t|did not|never) (reach|arrive|come through|get through|receive)|no (photo|image|picture)", re.I
)
BENCH_SETTINGS = {"orchHost": "anthropic", "orchM_anthropic": MODEL, "sfxVol": "0"}


# ---- pure helpers (host-tested) ---------------------------------------------------


def settle_page(updates: List[dict]) -> str:
    """A settle-marked getUpdates page (compact JSON: it rides one console line). The
    mark must open the body exactly so (telegram.cpp kSettleMark)."""
    body = json.dumps({"ok": True, "settle": True, "result": updates}, separators=(",", ":"))
    assert body.startswith('{"ok":true,"settle":true,')
    return body


def dispatch_of(line: str) -> Optional[Tuple[int, int, str]]:
    """(updates, held ms, why) of a settle dispatch line (logged as its turns start), or
    None."""
    m = DISPATCH_RE.search(line or "")
    return (int(m.group(1)), int(m.group(2)), m.group(3)) if m else None


def ran_of(line: str) -> Optional[Tuple[int, int]]:
    """(messages, turns) of the line logged once a settled page ran, or None."""
    m = RAN_RE.search(line or "")
    return (int(m.group(1)), int(m.group(2))) if m else None


def spend_of(lines: List[str]) -> List[Tuple[str, int, int, int, int]]:
    """(model, in, out, cacheRead, cacheWrite) per provider-reported usage line."""
    return [
        (m.group(1), int(m.group(2)), int(m.group(3)), int(m.group(4)), int(m.group(5)))
        for ln in lines
        for m in [SPEND_RE.search(ln)]
        if m
    ]


def tags_of(row: dict) -> List[str]:
    """A history row's tags, one per item (they are comma-separated)."""
    return [t.strip() for t in (row.get("tags") or "").split(",") if t.strip()]


def is_message(row: dict) -> bool:
    """A chat message row (not a tool trace or a model-response trace row)."""
    return (row.get("kind") or "message") == "message"


def turn_rows(rows: List[dict], src: str) -> Tuple[List[dict], List[dict]]:
    """The user rows of the turn whose source tag is ``src`` (``tg:<newest update
    id>x<messages>``) and the assistant messages answering them (tagged ``turn:<user row
    id>``), oldest first (the store answers newest first). The words carry no marker:
    a number in the text only puzzles the model."""
    users = [r for r in rows if r.get("role") == "user" and is_message(r) and f"src:{src}" in tags_of(r)]
    wanted = {f"turn:{r.get('id')}" for r in users if r.get("id")}
    replies = [r for r in rows if r.get("role") == "assistant" and is_message(r) and wanted & set(tags_of(r))]
    return users[::-1], replies[::-1]


def plate_jpeg(seed: int = 25) -> bytes:
    """A drawn Sunday-roast plate, each food labeled in large print (the head reads the
    labels), the meat a roast lamb (the replayed text never names the meat, so a reply
    that names it read the photo), the mac and cheese in its own dish beside it."""
    from PIL import Image, ImageDraw, ImageFont

    font = ImageFont.load_default(size=34)
    img = Image.new("RGB", (1024, 768), (150, 105, 60))
    d = ImageDraw.Draw(img)
    d.ellipse((120, 90, 740, 690), fill=(245, 245, 240), outline=(200, 200, 200), width=6)
    foods = [
        ((220, 170, 450, 320), (120, 50, 40), "ROAST LAMB"),
        ((470, 200, 650, 350), (220, 170, 80), "POTATOES"),
        ((220, 380, 400, 520), (200, 150, 70), "YORKSHIRE"),
        ((430, 400, 640, 560), (230, 120, 40), "CARROTS"),
    ]
    for box, color, label in foods:
        d.ellipse(box, fill=color)
        d.text((box[0], box[3] + 4), label, fill=(0, 0, 0), font=font)
    d.ellipse((770, 380, 1000, 600), fill=(250, 250, 250), outline=(180, 180, 180), width=4)
    d.ellipse((800, 410, 970, 570), fill=(240, 200, 60))
    d.text((760, 610), "MAC AND", fill=(0, 0, 0), font=font)
    d.text((760, 650), "CHEESE", fill=(0, 0, 0), font=font)
    d.text((260, 30), f"SUNDAY ROAST {seed}", fill=(255, 255, 255), font=font)
    out = io.BytesIO()
    img.save(out, "JPEG", quality=90)
    return out.getvalue()


def speech_ogg(words: str) -> bytes:
    """Spoken words as a Telegram-style voice note (OGG/Opus, mono 16 kHz): macOS ``say``
    renders them, ffmpeg encodes them."""
    import shutil
    import subprocess
    import tempfile

    say, ff = shutil.which("say"), shutil.which("ffmpeg")
    if not (say and ff):
        raise RuntimeError("macOS say and ffmpeg are needed to build the spoken voice note")
    with tempfile.TemporaryDirectory() as d:
        aiff, ogg = f"{d}/v.aiff", f"{d}/v.ogg"
        subprocess.run([say, "-o", aiff, words], check=True)
        subprocess.run(
            [ff, "-loglevel", "error", "-i", aiff, "-ac", "1", "-ar", "16000", "-c:a", "libopus", "-b:a", "24k", ogg],
            check=True,
        )
        with open(ogg, "rb") as f:
            return f.read()


# ---- device helpers -----------------------------------------------------------------


def _collect(device, seconds: float, until: str = "") -> List[str]:
    """Console lines for ``seconds`` (or up to and with the first holding ``until``)."""
    out, end = [], time.time() + seconds
    while time.time() < end:
        try:
            line = device.expect_re(r".", timeout=end - time.time()).string
        except ExpectTimeout:
            break
        out.append(line)
        if until and until in line:
            break
    return out


def _quiet(device, gap_s: float = 8.0, limit_s: float = 90.0) -> None:
    """Wait until the console has been silent for ``gap_s`` (the device finished the
    background work after a turn: a photo's record, a fold), at most ``limit_s``. A text
    injected while the poll task is busy is first seen late, and the date rule then
    answers it at once: the window is not what such a run measures."""
    end = time.time() + limit_s
    while time.time() < end:
        try:
            device.expect_re(r".", timeout=min(gap_s, max(0.1, end - time.time())))
        except ExpectTimeout:
            return


def _inject(device, page: str, tries: int = 60) -> str:
    """TGINJECT one settle page; a busy answer (the held page was dispatched and is
    still running) is retried every 2 s."""
    line = ""
    for _ in range(tries):
        line = device.cmd("TGINJECT " + page, "TGINJECT", timeout=15.0)
        if "busy" not in line:
            break
        time.sleep(2.0)
    assert "queued" in line and "settle=1" in line, f"the settle page was not queued: {line}"
    return line


def _state(net, tries: int = 4) -> dict:
    """/api/state, read again when a busy device answers with a cut or non-JSON body."""
    for t in range(tries):
        try:
            return net.get_json("/api/state", timeout=8.0)
        except ValueError:
            if t + 1 == tries:
                raise
            time.sleep(2.0)
    return {}


def _episodic(net, limit: int = 80) -> List[dict]:
    """The newest history rows; [] while the device answers busy (a turn holds the store)."""
    try:
        return net.get_json(f"/api/mem/episodic?limit={limit}", timeout=8.0).get("messages", [])
    except ValueError:  # "Device busy, try again." is not JSON: the caller polls again
        return []


def _wait(pred, timeout: float, every: float = 2.0):
    end = time.time() + timeout
    while time.time() < end:
        got = pred()
        if got:
            return got
        time.sleep(every)
    return pred()


def _rows(net, src: str, replies: int = 1):
    """(user rows, reply rows) of the turn with source tag ``src`` once at least
    ``replies`` replies landed, else None."""
    users, answers = turn_rows(_episodic(net), src)
    return (users, answers) if users and len(answers) >= replies else None


def _report(tag: str, lines: List[str]) -> None:
    for ln in lines:
        if "telegram:" in ln or "fabric-spend:" in ln:
            print(f"[{tag}] log: {ln[:220]}")
    for model, i, o, cr, cw in spend_of(lines):
        print(f"[{tag}] usage {model} in={i} out={o} cR={cr} cW={cw}")


def _lift_spend_pause(net) -> Optional[str]:
    """Lift a paused monthly ceiling by 10 USD for the legs; returns the ceiling to put
    back (None when nothing was paused). Skips, saying so, when calls stay paused."""
    spend = _state(net).get("spend") or {}
    if not spend.get("frozen"):
        return None
    ceiling = str(spend["limitCents"]) if spend.get("limitCents") else "off"
    net.post("/api/orch", {"spendCeil": str(int(spend.get("spentCents", 0)) + 1000)}, timeout=8.0)
    print("[bench] the monthly spend ceiling had paused paid calls: lifted by 10 USD for the legs")
    if (_state(net).get("spend") or {}).get("frozen"):
        net.post("/api/orch", {"spendCeil": ceiling}, timeout=8.0)
        pytest.skip("paid calls stay paused on this unit after lifting the ceiling")
    return ceiling


@pytest.fixture
def coalesce_bench(bench, net):
    """The bench unit with the test bot as owner, the head on the cheap model, sound at
    0; a paused spend ceiling lifted; every changed setting restored, each on its own."""
    orch = net.get_json("/api/orch", timeout=8.0)
    anth = (orch.get("providers") or {}).get("anthropic") or {}
    if not anth.get("hasKey"):
        pytest.skip("no Anthropic key on the unit")
    prior = {
        "orchHost": str(orch.get("orchHost") or ""),
        "orchM_anthropic": str(anth.get("orchModel") or ""),
        "sfxVol": str(orch.get("sfxVol", 0)),
    }
    r = net.post("/api/orch", BENCH_SETTINGS, timeout=8.0)
    assert r.status_code == 200, f"bench settings -> {r.status_code} {r.text[:200]}"
    ceiling = _lift_spend_pause(net)
    time.sleep(20.0)  # a message still queued for the test bot runs first (the token swap reset the offset)
    try:
        yield bench
    finally:
        try:
            net.post("/api/orch", {k: v for k, v in prior.items() if v != ""}, timeout=8.0)
        finally:
            if ceiling:
                net.post("/api/orch", {"spendCeil": ceiling}, timeout=8.0)


@pytest.fixture
def plate(net):
    """The drawn plate staged in the Files store over HTTP: (card path, size). The
    project is removed afterwards."""
    import requests

    data = plate_jpeg()
    name = "plate.jpg"
    url = f"http://{net.ip}/api/files/upload?project={STAGE_PROJECT}&name={name}"
    r = requests.post(url, files={"file": (name, data, "image/jpeg")}, headers=net._auth(), timeout=60)
    assert r.status_code < 400, f"staging the plate: HTTP {r.status_code} {r.text[:120]}"
    try:
        yield f"/mem/files/{STAGE_PROJECT}/{name}", len(data)
    finally:
        net.post(f"/api/files/rmproject?project={STAGE_PROJECT}&confirm=DELETE", {}, timeout=20.0)


# ---- unit A ---------------------------------------------------------------------------


@pytest.mark.hil
@pytest.mark.net
@pytest.mark.agent
def test_l48w_sunday_roast_is_one_reply(device, net, coalesce_bench, plate):
    chat = int(str(coalesce_bench.chat))
    uid = tb.first_update_id() + 4900
    marker = "Without the mac and cheese. Just the Sunday roast"  # the 25 Oct words
    _quiet(device)
    _inject(device, settle_page([tb.text_update(uid, chat, marker)]))
    lines = _collect(device, GAP_S)
    _inject(device, settle_page([tb.local_photo_update(uid + 1, chat, plate[0], plate[1])]))
    lines += _collect(device, 120.0, until="settle dispatch (injected)")
    lines += _collect(device, 300.0, until="settled page ran")
    lines += _collect(device, 8.0)
    _report("roast", lines)
    got = [d for d in map(dispatch_of, lines) if d]
    ran = [r for r in map(ran_of, lines) if r]
    assert got and got[0][0] == 2, f"the text and the photo did not dispatch together: {got}"
    assert ran and ran[0] == (2, 1), f"the text and the photo did not run as one turn: {ran}"
    assert got[0][1] >= GAP_S * 1000, f"held {got[0][1]} ms: the photo joined the held text"
    rows = _wait(lambda: _rows(net, f"tg:{uid + 1}x2"), 120)
    assert rows, "no user row and reply for the replay (one turn of both updates)"
    users, replies = rows
    assert len(users) == 1, f"expected one turn holding the text, got {len(users)}"
    text = users[0]["text"]
    at_text, at_photo = text.find(marker), text.find("[image 1/1")
    assert 0 <= at_text < at_photo, f"the text must come first, then the photo: {text[:300]}"
    assert len(replies) == 1, f"expected one reply, got {len(replies)}"
    reply = replies[0]["text"]
    print(f"[roast] reply: {reply}")
    assert not MISSING_RE.search(reply), f"the reply says something is missing: {reply}"
    assert re.search(r"\blamb\b", reply, re.I), f"the reply did not use the photo (no lamb): {reply}"


@pytest.mark.hil
@pytest.mark.net
@pytest.mark.agent
def test_l48w_late_photo_knows_the_answer(device, net, coalesce_bench, plate):
    chat = int(str(coalesce_bench.chat))
    uid = tb.first_update_id() + 4920
    marker = "Just the Sunday roast tonight"
    _quiet(device)
    _inject(device, settle_page([tb.text_update(uid, chat, marker)]))
    lines = _collect(device, 60.0, until="settle dispatch (injected)")  # logged as the text's turn starts
    lines += _collect(device, 1.0)
    photo = tb.local_photo_update(uid + 1, chat, plate[0], plate[1])  # sent now, before the answer
    _inject(device, settle_page([photo]))  # busy until the text's turn returned
    lines += _collect(device, 150.0, until=CROSSED_LOG)
    lines += _collect(device, 180.0, until="settled page ran")
    lines += _collect(device, 8.0)
    _report("late", lines)
    assert any(CROSSED_LOG in ln for ln in lines), "the late photo's turn was not told the answer exists"
    first = _wait(lambda: _rows(net, f"tg:{uid}x1"), 60)
    second = _wait(lambda: _rows(net, f"tg:{uid + 1}x1"), 120)
    assert first and second, f"two turns, each answered: text {bool(first)}, photo {bool(second)}"
    print(f"[late] reply to the text: {first[1][0]['text'][:400]}")
    print(f"[late] reply to the photo: {second[1][0]['text'][:400]}")
    assert "sent while you were" not in second[0][0]["text"], "the history row keeps what was sent, not the note"


@pytest.mark.hil
@pytest.mark.net
@pytest.mark.agent
def test_l48w_plain_text_latency(device, net, coalesce_bench):
    chat = int(str(coalesce_bench.chat))
    holds = []
    for k in range(3):
        uid = tb.first_update_id() + 4940 + 2 * k
        marker = f"What is {k + 2} plus {k + 3}?"
        _quiet(device)
        sent = time.time()
        _inject(device, settle_page([tb.text_update(uid, chat, marker)]))
        lines = _collect(device, 60.0, until="settle dispatch (injected)")
        dispatched = time.time()
        rows = _wait(lambda: _rows(net, f"tg:{uid}x1"), 120, every=0.5)
        replied = time.time()
        lines += _collect(device, 5.0)
        _report(f"plain{k}", lines)
        got = [d for d in map(dispatch_of, lines) if d]
        assert got and got[0][0] == 1 and got[0][2] == "quiet", f"the plain text dispatch: {got}"
        assert rows, "no reply to the plain text"
        held = got[0][1]
        holds.append(held)
        print(
            f"[plain{k}] hold {held} ms (device), inject->dispatch {1000 * (dispatched - sent):.0f} ms, "
            f"inject->reply row {1000 * (replied - sent):.0f} ms"
        )
        assert TEXT_WINDOW_MS <= held <= TEXT_WINDOW_MS + REPOLL_MS, f"a plain text held {held} ms"
        assert any(TYPING_OK in ln for ln in lines), "no typing indicator during the hold"
        time.sleep(5.0)
    print(f"[plain] holds {holds}")


@pytest.fixture
def voice(net):
    """A spoken voice note staged in the Files store over HTTP: (card path, size). The
    project is removed afterwards."""
    import requests

    data = speech_ogg("Make that the chicken salad instead of the roast, please.")
    name = "voice.ogg"
    url = f"http://{net.ip}/api/files/upload?project={STAGE_PROJECT}&name={name}"
    r = requests.post(url, files={"file": (name, data, "audio/ogg")}, headers=net._auth(), timeout=60)
    assert r.status_code < 400, f"staging the voice note: HTTP {r.status_code} {r.text[:120]}"
    try:
        yield f"/mem/files/{STAGE_PROJECT}/{name}", len(data)
    finally:
        net.post(f"/api/files/rmproject?project={STAGE_PROJECT}&confirm=DELETE", {}, timeout=20.0)


@pytest.mark.hil
@pytest.mark.net
@pytest.mark.agent
def test_l48w_text_then_voice_is_one_turn(device, net, coalesce_bench, voice):
    chat = int(str(coalesce_bench.chat))
    uid = tb.first_update_id() + 4960
    words = "For dinner tonight"
    _quiet(device)
    _inject(device, settle_page([tb.text_update(uid, chat, words)]))
    lines = _collect(device, GAP_S)
    note = tb.local_file_update(uid + 1, chat, "voice", voice[0], voice[1], {"mime_type": "audio/ogg", "duration": 3})
    _inject(device, settle_page([note]))
    lines += _collect(device, 120.0, until="settle dispatch (injected)")
    lines += _collect(device, 300.0, until="settled page ran")
    lines += _collect(device, 8.0)
    _report("voice", lines)
    if any("Voice messages aren't available" in ln for ln in lines):
        pytest.skip("no speech-to-text provider on the unit")
    got = [d for d in map(dispatch_of, lines) if d]
    ran = [r for r in map(ran_of, lines) if r]
    assert got and got[0][0] == 2, f"the text and the voice note did not dispatch together: {got}"
    assert ran and ran[0] == (2, 1), f"the text and the voice note did not run as one turn: {ran}"
    rows = _wait(lambda: _rows(net, f"tg:{uid + 1}x2"), 120)
    assert rows, "no user row and reply for the text and the voice note (one turn of both)"
    users, replies = rows
    text = users[0]["text"]
    at_text, at_voice = text.find(words), text.find("(voice note)")
    assert 0 <= at_text < at_voice, f"the text must come first, then the voice note: {text[:300]}"
    print(f"[voice] turn: {text[:300]}")
    print(f"[voice] reply: {replies[0]['text'][:400]}")


# ---- host tests ---------------------------------------------------------------------


@pytest.mark.host
def test_settle_page_mark_and_parse_helpers():
    body = settle_page([tb.text_update(7, 9, "hi")])
    assert body.startswith('{"ok":true,"settle":true,"result":[')
    assert json.loads(body)["result"][0]["message"]["text"] == "hi"
    assert dispatch_of("[agent] telegram: settle dispatch (injected) n=2 after 7412 ms (quiet)") == (2, 7412, "quiet")
    assert dispatch_of("telegram: settle hold (injected, re-poll 3)") is None
    assert ran_of("telegram: settled page ran: 2 messages in 1 turn") == (2, 1)
    assert ran_of("telegram: settled page ran: 1 message in 1 turn") == (1, 1)
    assert spend_of(["fabric-spend: claude-haiku-5-5 in=10 out=2 cR=0 cW=5"]) == [("claude-haiku-5-5", 10, 2, 0, 5)]


@pytest.mark.host
def test_turn_rows_pairs_replies_by_turn_tag():
    rows = [
        {"id": "m5", "role": "assistant", "kind": "message", "text": "other", "tags": "out:reply,turn:m23"},
        {"id": "m4", "role": "assistant", "kind": "message", "text": "logged", "tags": "out:reply,turn:m2"},
        {"id": "m3", "role": "assistant", "kind": "llm_response", "text": "trace", "tags": "turn:m2"},
        {"id": "m2", "role": "user", "kind": "message", "text": "Sunday roast", "tags": "from:Bench,src:tg:9x2"},
        {"id": "m1", "role": "assistant", "text": "older", "tags": "turn:m0"},
    ]
    users, replies = turn_rows(rows, "tg:9x2")
    assert [u["id"] for u in users] == ["m2"]
    assert [r["id"] for r in replies] == ["m4"]  # not the trace row, not turn:m23
    assert turn_rows(rows, "tg:9x1") == ([], [])
    assert MISSING_RE.search("If a photo or message was meant to arrive, it didn't reach me")
    assert not MISSING_RE.search("Sunday roast logged, mac and cheese excluded: ~755 kcal")


@pytest.mark.host
def test_plate_is_a_jpeg():
    data = plate_jpeg()
    assert data[:2] == b"\xff\xd8" and 20000 < len(data) < 600000
