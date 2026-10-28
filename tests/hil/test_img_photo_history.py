"""IMG - photos stay real images for the head, before and after the turn they arrive in.

Host-tested first (test/test_turn_images, test_harness_wire_{anthropic,openai,mistral},
test_image_label, test_tg_archive): every photo is kept for the turn and the host that
serves each request decides whether it sees the pixels; the head writes the photo's
record itself (image.note) and no caption call is made for a photo it saw; image.look
puts an earlier photo back in front of the head as an image; on Anthropic a recent
photo rides its own history message as a file reference, so the cached prefix holds.

These legs prove it on hardware with no phone and no bot token (TGINJECT pages whose
photos are card files staged over HTTP, the lane D helpers in test_l48_albums), at
Lumi's memory (FAULT sram_ballast down to a largest internal block of about 7.7 KB and
at most 33 KB free, the release bench recipe), with the head named by
``NIMBUS_IMG_HEAD`` ("anthropic:claude-haiku-5-5" by default, or
"mistral:mistral-large-latest"); ``NIMBUS_IMG_MODE=inline`` turns the upload (and so
the re-attach) off, so an Anthropic head views the photo again through image.look only.
The head, the photo mode, the ballast and the allowlist are restored.

* ``test_img_photo_turn_then_look_again``: a label photo (a number only a reader of the
  pixels can know) goes to the head as an image ("attached for the head"); no caption
  request is made during the turn or in the idle steps after it; the photo's row is
  settled by the turn ("row settled"); the archive item reads ok at turn time. A
  follow-up "look at that photo again" turn views the pixels again ("image.look ...
  goes to the head as an image", still no caption request) and the reply counts the
  dots printed on the label - a count a one-line note does not carry, so the head
  must read the pixels, not its own record. On Anthropic a third turn follows, and
  every turn's per-round cache read sits at or above the first turn's (the history,
  the re-attached photo included, is read from the cache; a turn's total also scales
  with its round count, so the comparison is per round).

The pure helpers are host-tested at the bottom (``-m host``).
"""

from __future__ import annotations

import io
import os
import random
import re
import time
from typing import Dict, List, Optional, Tuple

import pytest

import tg_archive_bench as tb
from test_l49_native_image import spend_state
from test_l48_albums import (  # noqa: F401 - the fixtures are used by name
    _episodic,
    _http,
    _inject,
    _lines_until,
    _wait,
    album_page,
    owner_chat,
)

CAPTION_RE = re.compile(r"vision: (?:TEST override answered \S+|\S+ \S+ read \d+ bytes)")
ATTACHED_RE = re.compile(r"telegram: media (\d+)/(\d+) attached for the head \((\d+) bytes\)")
SETTLED_RE = re.compile(r"vision: photo (\d+)/(\d+) row settled \(([^)]*)\)")
LOOK_RE = re.compile(r"vision: image\.look ([0-9a-f]{16}) goes to the head as an image \((\d+) bytes\)")
SPEND_RE = re.compile(r"turn spend: (ok|FAIL) .*")
SLOT_RE = re.compile(
    r"(?P<host>[a-z]+)/(?P<model>\S+) in=(?P<inp>\d+) out=(?P<out>\d+) cR=(?P<cr>\d+) cW=(?P<cw>\d+) n=(?P<n>\d+)"
)
BALLAST_RE = re.compile(r"held=(\d+) intFree=(\d+) intLargest=(\d+)")
REF_RE = re.compile(r"\[image 1/1 ref=([0-9a-f]{16})")
REATTACH_RE = re.compile(r"anthropic: (\d+) recent photo\(s\) ride their history message")
LUMI_LARGEST = 7700
LUMI_FREE = 33000
IDLE_WATCH_S = 45  # the old record call ran in the idle steps right after the reply
LABEL_VALUE = "58213"
DOTS_MIN, DOTS_MAX = (
    5,
    8,
)  # the countable dots on the label (per-run random): a count a one-line note does not carry, in the range a vision model counts reliably (run7: mistral-large read the pixels and still miscounted 15 small dots as 13 - past ten, counting is not a property a model has)


# ---- pure helpers (host-tested) ---------------------------------------------------


def caption_calls(lines: List[str]) -> List[str]:
    """Every caption request a stretch of console lines shows (a real one or the
    PROVFAULT test override)."""
    return [ln for ln in lines if CAPTION_RE.search(ln)]


def spend_slots(lines: List[str]) -> List[Dict[str, object]]:
    """The provider slots of the last turn-spend line: host, model, in, out, cR, cW."""
    last = None
    for ln in lines:
        if SPEND_RE.search(ln):
            last = ln
    if not last:
        return []
    out = []
    for m in SLOT_RE.finditer(last):
        d: Dict[str, object] = {"host": m.group("host"), "model": m.group("model")}
        for k in ("inp", "out", "cr", "cw", "n"):
            d[k] = int(m.group(k))
        out.append(d)
    return out


def head_cache_read(lines: List[str], host: str) -> Optional[int]:
    """The cache-read tokens the head's host billed in the turn (None: no such slot)."""
    for s in spend_slots(lines):
        if s["host"] == host:
            return int(s["cr"])
    return None


def head_spec(value: str) -> Tuple[str, str]:
    host, _, model = (value or "anthropic:claude-haiku-5-5").partition(":")
    return host, model or "claude-haiku-5-5"


def label_photo(value: str, seed: int, dots: int = 0) -> bytes:
    """A JPEG label with a large number (only a reader of the pixels can know it)
    and, when `dots` > 0, a row of that many filled dots along the bottom (a count
    one note line does not carry), kept under the turn's 80 KB pick budget."""
    from PIL import Image, ImageDraw, ImageFont

    img = Image.new("RGB", (640, 420), (250, 248, 240))
    d = ImageDraw.Draw(img)
    try:
        font = ImageFont.truetype("/System/Library/Fonts/Helvetica.ttc", 120)
    except OSError:
        font = ImageFont.load_default()
    d.rectangle([14, 14, 626, 406], outline=(20, 20, 20), width=6)
    d.text((40, 60), "LOT", fill=(10, 10, 10), font=font)
    d.text((40, 220), value, fill=(10, 10, 10), font=font)
    for i in range(dots):  # large and well separated (see evidence/hil/dots-preview-8.jpg)
        d.ellipse([30 + i * 52, 344, 62 + i * 52, 376], fill=(30, 30, 30))
    d.point([(seed % 640, 412)], fill=(0, 0, 0))
    buf = io.BytesIO()
    img.save(buf, "JPEG", quality=80)
    return buf.getvalue()


# ---- device helpers ---------------------------------------------------------------


def _orch(net) -> dict:
    return _http(lambda: net.get("/api/orch", timeout=8.0), "reading /api/orch").json()


def _mem(net) -> Dict[str, int]:
    st = net.get_json("/api/state", timeout=8.0)
    return {
        "free": int((st.get("mem") or {}).get("intFree") or st.get("heap") or 0),
        "largest": int((st.get("mem") or {}).get("intLargest") or 0),
    }


@pytest.fixture
def lumi_heap(device, net):
    """FAULT sram_ballast until the largest internal block is at most LUMI_LARGEST with
    at most LUMI_FREE free (Lumi's resting heap); released afterwards."""
    device.cmd("FAULT sram_ballast 0", "FAULT sram_ballast", timeout=10.0)
    time.sleep(1.0)
    hold = _mem(net)["largest"] + 500
    out = ""
    for _ in range(60):
        out = device.cmd(f"FAULT sram_ballast {hold}", "FAULT sram_ballast", timeout=10.0)
        m = BALLAST_RE.search(out or "")
        if m and int(m.group(3)) <= LUMI_LARGEST and int(m.group(2)) <= LUMI_FREE:
            break
        hold += 500
    m = BALLAST_RE.search(out or "")
    print(f"ballast: {out}")
    if not m or int(m.group(3)) > LUMI_LARGEST or int(m.group(2)) > LUMI_FREE:
        device.cmd("FAULT sram_ballast 0", "FAULT sram_ballast", timeout=10.0)
        pytest.fail(f"Lumi's heap not reached (largest <= {LUMI_LARGEST}, free <= {LUMI_FREE}): {out}")
    try:
        yield {"free": int(m.group(2)), "largest": int(m.group(3)), "held": int(m.group(1))}
    finally:
        device.cmd("FAULT sram_ballast 0", "FAULT sram_ballast", timeout=10.0)


@pytest.fixture
def img_head(device, net, owner_chat):  # noqa: F811 - the lane D fixture, imported by name
    """The head named by NIMBUS_IMG_HEAD, the tool loop on, the photo path in its
    default state (native, upload once); everything restored afterwards."""
    host, model = head_spec(os.environ.get("NIMBUS_IMG_HEAD", ""))
    before = _orch(net)
    prev_model = ((before.get("providers") or {}).get(host) or {}).get("orchModel", "")
    _http(
        lambda: net.post("/api/orch", {"orchHost": host, "orchLoop": "1", f"orchM_{host}": model}, timeout=8.0),
        "pinning the head",
    )
    device.cmd("IMGMODE native", "IMGMODE native=", timeout=8.0)
    # NIMBUS_IMG_MODE=inline: no upload, so no re-attach either; an Anthropic head then
    # views the photo again through image.look only.
    mode = os.environ.get("NIMBUS_IMG_MODE", "upload")
    device.cmd(f"IMGMODE {'inline' if mode == 'inline' else 'upload'}", "IMGMODE native=", timeout=8.0)
    spend = spend_state(device.cmd("SPENDSET?", "SPENDSET {", timeout=8.0))
    lifted = bool(spend.get("frozen"))
    if lifted:  # another leg spent the bench ceiling: room for this leg, the limit back after
        device.cmd(f"SPENDSET limit {int(spend.get('spentCents', 0)) + 2000}", "SPENDSET limit", timeout=8.0)
    try:
        yield {"chat": owner_chat, "host": host, "model": model}
    finally:
        if lifted:
            device.cmd(f"SPENDSET limit {spend.get('limitCents')}", "SPENDSET limit", timeout=8.0)
        device.cmd("IMGMODE upload", "IMGMODE native=", timeout=8.0)
        restore = {"orchHost": before.get("orchHost", ""), "orchLoop": str(int(before.get("orchLoop", 1)))}
        if prev_model:
            restore[f"orchM_{host}"] = prev_model
        _http(lambda: net.post("/api/orch", restore, timeout=8.0), "restoring the head")


def _stage_label(net, value: str, seed: int, dots: int = 0) -> Tuple[str, int]:
    import requests

    data = label_photo(value, seed, dots)
    name = f"img{seed}.jpg"
    url = f"http://{net.ip}/api/files/upload?project=l48&name={name}"
    files = {"file": (name, data, "image/jpeg")}
    _http(lambda: requests.post(url, files=files, headers=net._auth(), timeout=60), f"staging {name}")
    return f"/mem/files/l48/{name}", len(data)


def _idle_lines(device, seconds: float) -> List[str]:
    """Console lines for `seconds` after a turn (the idle steps run between cycles)."""
    out, end = [], time.time() + seconds
    while time.time() < end:
        try:
            out.append(device.expect_re(r".", timeout=max(0.5, end - time.time())).string)
        except Exception:  # noqa: BLE001 - quiet console: the window just ends
            break
    return out


def _reply(net, needle: str, timeout: float = 120.0) -> Optional[str]:
    def got():
        for row in _episodic(net, 40):
            if row.get("role") == "assistant" and needle in row.get("text", ""):
                return row["text"]
        return None

    return _wait(got, timeout)


def _archive_status(net, chat: int, uid: int) -> Optional[str]:
    """The archive item status of update `uid` (its status lines folded in by the web
    route), searched in the chat's newest days."""
    days = net.get_json(f"/api/tg/archive?chat={chat}", timeout=10.0).get("days", [])
    names = sorted((d.get("day") if isinstance(d, dict) else d for d in days), reverse=True)[:3]
    for day in names or [time.strftime("%Y-%m-%d", time.gmtime())]:
        frm = 0
        for _ in range(40):  # a page holds what fits one response: follow `next`
            data = net.get_json(f"/api/tg/archive?chat={chat}&day={day}&limit=200&from={frm}", timeout=10.0)
            for row in data.get("rows", []):
                if int(row.get("u") or 0) == uid:
                    return row.get("st")
            if not data.get("more"):
                break
            frm = int(data.get("next") or 0)
    return None


# ---- legs ---------------------------------------------------------------------------


def _photo_turn(device, net, chat: int, uid: int) -> Tuple[List[str], str, int]:
    """Turn 1: a label photo the head sees itself. Returns the console lines, the ref and
    the number of dots printed on it (turn 2's truth: a count a one-line note of the
    label does not carry, so the head must view the photo again)."""
    dots = random.randint(DOTS_MIN, DOTS_MAX)
    item = _stage_label(net, LABEL_VALUE, seed=int(time.time()) % 100000, dots=dots)
    lines = _inject(device, album_page(uid, chat, [item], caption=f"what is the big number on this label? ({uid})"))
    lines += _idle_lines(device, IDLE_WATCH_S)
    keep1 = ("telegram:", "vision", "turn spend", "orchestrator", "refused", "memory:", "heap")
    print("\n".join(ln for ln in lines if any(k in ln for k in keep1)))
    assert ATTACHED_RE.search("\n".join(lines)), "the photo was not attached for the head"
    assert not caption_calls(lines), f"a caption request was made for a photo the head saw: {caption_calls(lines)}"
    settled = [m.groups() for m in map(SETTLED_RE.search, lines) if m]
    assert settled, "the photo's row was not settled by its turn"
    assert settled[0][2] in ("the head's note", "seen"), settled
    st = _archive_status(net, chat, uid)
    assert st == "ok", f"the archive item reads {st!r} at turn time"
    rows = [r for r in _episodic(net, 40) if r.get("kind") == "image" and REF_RE.search(r.get("text", ""))]
    assert rows, "no stored row for the photo"
    print("stored row:", rows[0].get("text", "")[:200])
    return lines, REF_RE.search(rows[0]["text"]).group(1), dots


def _look_turn(device, net, chat: int, uid: int, ref: str, dots: int) -> List[str]:
    """Turn 2: the head views the photo again (image.look returns the pixels, or on
    Anthropic the kept file rides its history message) and counts what only a reader
    of the pixels can count; no caption request."""
    truth = f"dots: {dots}"
    t2 = (
        "Look at that photo again yourself with image.look (do not answer from memory or your note) and "
        "count exactly how many filled dots are printed in its bottom row. "
        f"Start your reply with '{truth}'. ({uid})"
    )
    lines = _inject(device, tb.page([tb.text_update(uid, chat, t2)]))
    lines += _idle_lines(device, 10)
    keep = ("vision", "turn spend", "anthropic:", "headloop", "tool", "orchestrator", "refused", "memory:", "heap")
    print("\n".join(ln for ln in lines if any(k in ln for k in keep)))
    looks = [m.groups() for m in map(LOOK_RE.search, lines) if m]
    reattached = [ln for ln in lines if REATTACH_RE.search(ln)]
    print(f"viewed again: image.look {looks}, re-attached {reattached[:1]}")
    assert looks or reattached, "the photo was not viewed again (no image.look image, no re-attached file)"
    if os.environ.get("NIMBUS_IMG_MODE") == "inline":
        assert looks, "with nothing re-attached the photo must come back through image.look"
    assert not looks or looks[0][0] == ref, (looks, ref)
    assert not caption_calls(lines), f"a caption request was made on a seeing head: {caption_calls(lines)}"
    text = _reply(net, truth)
    if not text:
        print("replies:", [r.get("text", "")[:160] for r in _episodic(net, 6) if r.get("role") == "assistant"])
    assert text, f"no reply named the dot count ({dots})"
    return lines


@pytest.mark.hil
@pytest.mark.net
@pytest.mark.agent
def test_img_photo_turn_then_look_again(device, net, img_head, lumi_heap):
    chat, host = img_head["chat"], img_head["host"]
    print(f"head {host}/{img_head['model']} at Lumi's heap {lumi_heap}")
    uid = tb.first_update_id() + 7100
    lines, ref, dots = _photo_turn(device, net, chat, uid)
    lines2 = _look_turn(device, net, chat, uid + 1, ref, dots)
    if host != "anthropic":
        print(f"cache reads (informational on {host}): {[head_cache_read(l, host) for l in (lines, lines2)]}")
        return
    # Turn 3: on Anthropic the per-round cached prefix never shrinks. A turn's total
    # cache read scales with its round count, so the fair comparison is the per-round
    # average: every later turn's must sit at or above the first turn's.
    lines3 = _inject(device, tb.page([tb.text_update(uid + 2, chat, f"thanks, and the first word on it? ({uid + 2})")]))
    assert not caption_calls(lines3)
    per = []
    for l in (lines, lines2, lines3):
        mine = [s for s in spend_slots(l) if s["host"] == host]
        assert mine, "no turn-spend slot for the head"
        per.append((mine[0]["cr"], mine[0]["n"]))
    print(f"cache reads (total, rounds) over three turns: {per}")
    first = per[0][0] / max(per[0][1], 1)
    for i in (1, 2):
        assert per[i][0] / max(per[i][1], 1) >= first, (
            f"turn {i + 1}'s per-round cache read fell below the first turn's: {per}"
        )


# ---- host tests ---------------------------------------------------------------------


@pytest.mark.host
def test_host_caption_and_spend_parse():
    lines = [
        "telegram: media 1/1 attached for the head (61234 bytes)",
        "vision: mistral mistral-small-latest read 92938 bytes -> 749 chars",
        "vision: TEST override answered api.openai.com",
        "turn spend: ok telegram | anthropic/claude-haiku-5-5 in=812 out=95 cR=10450 cW=1200 n=2 | "
        "mistral/mistral-small-latest in=300 out=50 cR=0 cW=0 n=1",
    ]
    assert len(caption_calls(lines)) == 2
    assert head_cache_read(lines, "anthropic") == 10450
    assert head_cache_read(lines, "mistral") == 0
    assert head_cache_read(lines, "openai") is None
    assert not caption_calls(["vision: photo 1/1 row settled (seen)", "vision: image.look ab goes to the head"])
    m = LOOK_RE.search("[agent] vision: image.look 0123456789abcdef goes to the head as an image (61234 bytes)")
    assert m and m.group(1) == "0123456789abcdef"
    s = SETTLED_RE.search("vision: photo 1/1 row settled (the head's note)")
    assert s and s.group(3) == "the head's note"
    assert REATTACH_RE.search("[agent] anthropic: 1 recent photo(s) ride their history message (kept files)")
    assert head_spec("") == ("anthropic", "claude-haiku-5-5")
    assert head_spec("mistral:mistral-large-latest") == ("mistral", "mistral-large-latest")
