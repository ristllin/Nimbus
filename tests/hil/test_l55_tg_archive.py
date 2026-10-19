"""L55 - the Telegram archive on the SD card (lane R, v4.5.14).

Owner ask: everything exchanged with the device over Telegram is saved on the
device, raw photos included, at full size. These legs prove it on unit L (SD card,
test build):

  1. the archive routes need the sign-in token;
  2. a page with a 2 MB photo (sent as a file), a photo, an album of 3, a voice
     note, a document, a sticker and a command lands on the card, every file full
     size and named <update_id>.<ext> under /mem/tg/<chat>/<day>/;
  3. a replay of the same page adds nothing;
  4. a forced retention pass (EPIPRUNE) deletes nothing under /mem/tg;
  5. the device's own replies are saved, whole, before splitting;
  6. a 2 MB streamed download leaves the internal heap low-water within 512 B of
     the baseline and pollStackMin unchanged;
  7. with the card faulted the archive reports unavailable and nothing crashes.

Telegram holds the files: the bench bot uploads synthetic media to the bench chat
(tg_archive_bench.py) and the sent messages become the injected page. Needs the
console verbs TGINJECT and EPIPRUNE (test build), NIMBUS_TEST_TG_TOKEN and
NIMBUS_TEST_TG_CHAT, and the unit's LAN address and token (NIMBUS_TEST_IP /
NIMBUS_TEST_TOKEN). Leg 5 runs one short real turn on the unit's configured
provider (pin claude-haiku-5-5, effort low).
"""

from __future__ import annotations

import hashlib
import os
import time

import pytest

import tg_archive_bench as tb
from test_l4_network import lan_ip_or_skip
from test_l9_resilience import _webtok

try:  # keep `pytest --collect-only` clean on a host without requests
    import requests
except ImportError:  # pragma: no cover
    requests = None

MB = 1024 * 1024


# ---- helpers ---------------------------------------------------------------------------


def _bench_env():
    token, chat = os.environ.get("NIMBUS_TEST_TG_TOKEN"), os.environ.get("NIMBUS_TEST_TG_CHAT")
    if not token or not chat:
        pytest.skip("NIMBUS_TEST_TG_TOKEN and NIMBUS_TEST_TG_CHAT are required for the archive legs")
    if requests is None:
        pytest.skip("requests not installed")
    return token, int(chat)


def _get(ip, tok, path, **params):
    return requests.get(f"http://{ip}{path}", params=params, headers={"X-Nimbus-Token": tok}, timeout=20)


def _rows(ip, tok, chat, day):
    rows, start = [], 0
    while True:
        r = _get(ip, tok, "/api/tg/archive", chat=chat, day=day, limit=200, **{"from": start})
        r.raise_for_status()
        page = r.json()
        rows += page["rows"]
        if not page["more"]:
            return rows
        start = page["next"]


def _today_utc() -> str:
    return time.strftime("%Y-%m-%d", time.gmtime())


def _inject(device, page_json: str):
    # TGINJECT feeds one raw getUpdates body into the receive path (lane N's verb).
    device.cmd("TGINJECT " + page_json, "TGINJECT", timeout=10.0)


def _wait_for(pred, timeout=180.0, every=3.0):
    deadline = time.time() + timeout
    while time.time() < deadline:
        got = pred()
        if got:
            return got
        time.sleep(every)
    return pred()


def _media_status(rows, base):
    """The last status of an item's media: its own row, then its event rows."""
    st = None
    for r in rows:
        if r.get("dir") == "in" and str(r.get("u")) == base:
            st = r.get("st")
        elif r.get("ev") == "media" and r.get("ref") == base:
            st = r.get("st")
    return st


def _item_sha(ip, tok, chat, day, name):
    r = _get(ip, tok, "/api/tg/archive/item", chat=chat, day=day, name=name)
    r.raise_for_status()
    return hashlib.sha256(r.content).hexdigest(), len(r.content)


def _state(ip, tok):
    return _get(ip, tok, "/api/state").json()


# ---- legs ------------------------------------------------------------------------------


@pytest.mark.host
def test_page_builder_host():
    """No board: the injected page has the shape the device parser reads."""
    sent = {
        "message_id": 5,
        "chat": {"id": 42, "type": "private"},
        "date": 1792330000,
        "photo": [
            {"file_id": "s", "width": 90, "height": 60, "file_size": 900},
            {"file_id": "L", "width": 2560, "height": 1707, "file_size": 400000},
        ],
        "media_group_id": "77",
        "from": {"id": 1, "is_bot": True},
    }
    u = tb.as_inbound(sent, 42, 1001)
    assert u["update_id"] == 1001 and u["message"]["from"]["is_bot"] is False
    assert u["message"]["media_group_id"] == "77"
    assert tb.largest_photo(sent)["file_id"] == "L"
    cmd = tb.text_update(1002, 42, "/help")
    assert cmd["message"]["entities"][0]["length"] == 5
    body = tb.page([u, cmd])
    assert body.startswith('{"ok":true,"result":[') and " " not in body[:40]


@pytest.mark.net
def test_archive_routes_need_the_token(device, net, secrets, require_secret):
    ip = lan_ip_or_skip(device, net, secrets, require_secret)
    if requests is None:
        pytest.skip("requests not installed")
    assert requests.get(f"http://{ip}/api/tg/archive", timeout=10).status_code == 401
    assert (
        requests.get(f"http://{ip}/api/tg/archive/item?chat=1&day=2026-10-18&name=1.json", timeout=10).status_code
        == 401
    )
    tok = _webtok(device)
    body = _get(ip, tok, "/api/tg/archive").json()
    assert body["stats"]["available"] is True, "unit L must have its SD card seated"
    assert body["stats"]["root"] == "/mem/tg"
    assert _get(ip, tok, "/api/tg/archive/item", chat="..", day="2026-10-18", name="x").status_code == 400


def _send_fixtures(bot):
    """Upload the media set to the bench chat; returns the sent messages and the big file."""
    big = tb.jpeg_near(2 * MB, seed=11)
    sent = {
        "big_doc": bot.send_file("sendDocument", "document", "big.jpg", big),
        "photo": bot.send_file("sendPhoto", "photo", "p.jpg", tb.jpeg_near(400000, seed=12)),
        "voice": bot.send_file("sendVoice", "voice", "v.ogg", tb.voice_ogg()),
        "doc": bot.send_file("sendDocument", "document", "notes.txt", b"archive bench document\n"),
        "sticker": bot.send_file("sendSticker", "sticker", "s.webp", tb.sticker_webp()),
    }
    album = bot.send_album([tb.jpeg_near(150000, seed=20 + i) for i in range(3)], "an album of three")
    return sent, album, big


def _media_page(sent, album, chat, uid):
    """The injected page: every sent message as an owner update, plus a command."""
    ups, expect = [], []
    for i, msg in enumerate(sent.values()):
        ups.append(tb.as_inbound(msg, chat, uid + i))
        expect.append(str(uid + i))
    for j, msg in enumerate(album):
        ups.append(tb.as_inbound(msg, chat, uid + 10 + j))
        expect.append(str(uid + 10 + j))
    ups.append(tb.text_update(uid + 20, chat, "/help"))
    return ups, expect


def _check_full_size(at, uid, sent, big, rows):
    """at = (ip, token, chat, day)."""
    ip, tok, chat, day = at
    bases = {str(r.get("u")) for r in rows if r.get("dir") == "in"}
    assert str(uid + 20) in bases  # the command is a raw-only item
    # The 2 MB file is byte-identical; the photo is the largest rendition Telegram has.
    sha, n = _item_sha(ip, tok, chat, day, f"{uid}.jpg")
    assert n == len(big) and sha == hashlib.sha256(big).hexdigest()
    _, n = _item_sha(ip, tok, chat, day, f"{uid + 1}.jpg")
    assert n == tb.largest_photo(sent["photo"])["file_size"]
    # The album keeps its group on every item.
    album_ids = {str(uid + 10 + j) for j in range(3)}
    groups = {r.get("mg") for r in rows if r.get("dir") == "in" and str(r.get("u")) in album_ids}
    assert len(groups) == 1 and None not in groups


@pytest.mark.net
@pytest.mark.hil
def test_inbound_media_full_size_then_replay_and_retention(device, net, secrets, require_secret):
    ip = lan_ip_or_skip(device, net, secrets, require_secret)
    tok = _webtok(device)
    token, chat = _bench_env()
    day = _today_utc()
    sent, album, big = _send_fixtures(tb.BenchBot(token, chat))
    uid = tb.first_update_id()
    ups, expect = _media_page(sent, album, chat, uid)
    body = tb.page(ups)

    _inject(device, body)
    done = _wait_for(lambda: all(_media_status(_rows(ip, tok, chat, day), b) == "ok" for b in expect), timeout=300)
    rows = _rows(ip, tok, chat, day)
    assert done, {b: _media_status(rows, b) for b in expect}
    _check_full_size((ip, tok, chat, day), uid, sent, big, rows)

    # The replay adds nothing.
    before = _get(ip, tok, "/api/tg/archive").json()["stats"]
    _inject(device, body)
    time.sleep(8)
    after = _get(ip, tok, "/api/tg/archive").json()["stats"]
    assert len(_rows(ip, tok, chat, day)) == len(rows)
    assert after["dupes"] >= before["dupes"] + len(ups) and after["in"] == before["in"]

    # A forced retention pass deletes nothing under /mem/tg. It must really run: a
    # pass that meets a turn in flight or a busy card waits and removes nothing.
    listing = sorted(r["file"] for r in rows if r.get("file"))
    status = ""
    for _ in range(20):
        status = device.cmd_re("EPIPRUNE", r"EPIPRUNE days=\d+ status=(\S+)", timeout=60.0).group(1)
        if not status.startswith("deferred"):
            break
        time.sleep(3)
    assert status == "ran", status
    for name in listing:
        assert _get(ip, tok, "/api/tg/archive/item", chat=chat, day=day, name=name).status_code == 200, name
    assert len(_rows(ip, tok, chat, day)) == len(rows)


@pytest.mark.net
@pytest.mark.hil
def test_local_inject_media_takes_both_paths(device, net, secrets, require_secret):
    """No phone and no Telegram upload: TGINJECT media whose bytes come from card files
    (FSPUT) lands in the archive byte-identical and runs the normal attachment path
    (the photo is stored in Memory & Files under chat-<id>, like a real one)."""
    ip = lan_ip_or_skip(device, net, secrets, require_secret)
    tok = _webtok(device)
    chat = int(os.environ.get("NIMBUS_TEST_TG_CHAT", "0")) or pytest.skip("NIMBUS_TEST_TG_CHAT is required")
    day = _today_utc()
    photo = tb.jpeg_near(300000, seed=31)  # under the 512 KB turn budget: both paths run
    album = [tb.jpeg_near(120000, seed=40 + i) for i in range(3)]
    voice = tb.voice_ogg()
    doc = b"local inject document\n" * 50
    files = {"/mem/inject/p.jpg": photo, "/mem/inject/v.ogg": voice, "/mem/inject/d.txt": doc}
    files.update({f"/mem/inject/a{i}.jpg": b for i, b in enumerate(album)})
    for path, data in files.items():
        tb.fsput(device, data, path)
    uid = tb.first_update_id() + 90
    ups = [tb.local_photo_update(uid, chat, "/mem/inject/p.jpg", len(photo))]
    ups += [
        tb.local_photo_update(uid + 1 + i, chat, f"/mem/inject/a{i}.jpg", len(album[i]), group="555") for i in range(3)
    ]
    ups.append(
        tb.local_file_update(uid + 4, chat, "voice", "/mem/inject/v.ogg", len(voice), {"mime_type": "audio/ogg"})
    )
    ups.append(tb.local_file_update(uid + 5, chat, "document", "/mem/inject/d.txt", len(doc), {"file_name": "d.txt"}))
    line = device.cmd("TGINJECT " + tb.page(ups), "TGINJECT", timeout=10.0)
    assert "queued" in line and "media=6" in line, line
    bases = [str(uid + k) for k in range(6)]
    assert _wait_for(lambda: all(_media_status(_rows(ip, tok, chat, day), b) == "ok" for b in bases), timeout=300)
    expect = [(f"{uid}.jpg", photo)] + [(f"{uid + 1 + i}.jpg", album[i]) for i in range(3)]
    expect += [(f"{uid + 4}.ogg", voice), (f"{uid + 5}.txt", doc)]
    for name, data in expect:
        sha, n = _item_sha(ip, tok, chat, day, name)
        assert n == len(data) and sha == hashlib.sha256(data).hexdigest(), name
    # The attachment path ran too: the photo was stored through the file store.
    listing = _get(ip, tok, "/api/files/list", project=f"chat-{chat}").json()
    assert any(f["name"].startswith("photo-") for f in listing.get("files", [])), listing


@pytest.mark.net
@pytest.mark.hil
@pytest.mark.agent
def test_device_replies_are_saved_whole(device, net, secrets, require_secret):
    ip = lan_ip_or_skip(device, net, secrets, require_secret)
    tok = _webtok(device)
    token, chat = _bench_env()
    day = _today_utc()
    before = [r for r in _rows(ip, tok, chat, day) if r.get("dir") == "out"]
    uid = tb.first_update_id() + 50
    _inject(device, tb.page([tb.text_update(uid, chat, "Reply with exactly the word: archived")]))
    got = _wait_for(
        lambda: len([r for r in _rows(ip, tok, chat, day) if r.get("dir") == "out"]) > len(before), timeout=180
    )
    assert got, "no outbound row after the turn"
    out = [r for r in _rows(ip, tok, chat, day) if r.get("dir") == "out"][-1]
    assert out["st"] == "ok" and out["origin"] in ("send", "direct")
    r = _get(ip, tok, "/api/tg/archive/item", chat=chat, day=day, name=out["raw"])
    assert r.status_code == 200 and r.json()["out"] is True and r.json()["text"]


@pytest.mark.net
@pytest.mark.hil
def test_streamed_download_heap_low_water(device, net, secrets, require_secret):
    ip = lan_ip_or_skip(device, net, secrets, require_secret)
    tok = _webtok(device)
    token, chat = _bench_env()
    bot = tb.BenchBot(token, chat)
    day = _today_utc()
    mp3 = tb.audio_mp3_near(2 * MB)
    sent = bot.send_file("sendAudio", "audio", "bench.mp3", mp3)  # no caption: no turn runs
    s0 = _state(ip, tok)
    base_low, base_stack = s0["heapMin"], s0["mem"]["pollStackMin"]
    uid = tb.first_update_id() + 70
    _inject(device, tb.page([tb.as_inbound(sent, chat, uid)]))
    assert _wait_for(lambda: _media_status(_rows(ip, tok, chat, day), str(uid)) == "ok", timeout=300)
    s1 = _state(ip, tok)
    print(f"heapMin {base_low} -> {s1['heapMin']}, pollStackMin {base_stack} -> {s1['mem']['pollStackMin']}")
    assert s1["heapMin"] >= base_low - 512
    assert s1["mem"]["pollStackMin"] == base_stack
    _, n = _item_sha(ip, tok, chat, day, f"{uid}.mp3")
    assert n == len(mp3)


@pytest.mark.net
@pytest.mark.hil
def test_card_fault_reports_unavailable(device, net, secrets, require_secret):
    ip = lan_ip_or_skip(device, net, secrets, require_secret)
    tok = _webtok(device)
    try:
        device.cmd("FAULT sd on", "FAULT", timeout=5.0)
        body = _get(ip, tok, "/api/tg/archive").json()
        assert body["stats"]["available"] is False and body["chats"] == []
        assert _get(ip, tok, "/api/tg/archive/item", chat="1", day="2026-10-18", name="1.json").status_code == 503
    finally:
        device.cmd("FAULT sd off", "FAULT", timeout=5.0)
    assert _get(ip, tok, "/api/tg/archive").json()["stats"]["available"] is True
