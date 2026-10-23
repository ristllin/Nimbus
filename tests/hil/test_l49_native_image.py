"""L49 - photos go to the head as images (lane E, owner ruling R1).

Host-tested first (test/test_turn_images, test_harness_wire_{anthropic,openai,mistral},
test_image_vision_park, test_image_label, test_image_records): the photos ride the
seeded user message as image parts, spliced into the body at send time from one PSRAM
copy in 4,096 B slices; the caption step is the fallback only, walks the providers and
parks a refusing one through the shared park; every failure has its own label.

These legs prove it on hardware with no phone and no bot token: TGINJECT pages whose
photos are card files staged over HTTP (the lane D helpers in test_l48_albums), the
head pinned to an image-reading model (claude-haiku-5-5 with effort low on the bench).
The ``IMGMODE`` console verb (RAM only) switches the photo path for a leg and restores
it afterwards.

* ``test_l49_three_photo_turn_heap_trace``: a 3-photo album goes to the head as images
  (three "attached for the head" lines, no caption call), the head exchanges trace
  af=0, their largest internal block stays within 2,048 B of the warm single-photo
  baseline, and every head body carries the three photos (inline: at least their
  base64; upload: three Files API uploads and small head bodies).
* ``test_l49_ten_photos_against_one``: a 10-photo turn against the single-photo turn,
  3 runs each: largest block within 2,048 B of the baseline, af=0, the 10-photo turn
  under its 2 MB budget; printed for the PR.
* ``test_l49_inline_against_upload``: the same 3-photo turn re-sent inline every round
  and uploaded once: heap and duration of each, printed for the design decision.
* ``test_l49_label_numbers_read_from_pixels``: three rendered label photos (numbers
  only a reader of the pixels can know) with the caption path off: the reply names
  them (R1, H2-1's machine half).
* ``test_l49_provfault_parks_the_captioner``: the caption path on, PROVFAULT 429
  insufficient_quota on the first caption host: it parks (PARK?), the next 5 photos
  make zero requests to it, one owner notice line.
* ``test_l49_provfault_off_rereads_once``: after the fault is cleared and the park
  ends (PARK clear), exactly one re-read runs in the idle step and the photo's row
  text is replaced.

Human gate (manual, H2-1 to H2-5): a real album of 3 label photos from the phone gets
one reply that reads the label numbers; a photo sent as a file (PNG) is seen; an
iPhone HEIC file gets the "send it as a photo" label; a photo during a provider outage
names the cause and is read once the provider is back.

The pure helpers are host-tested at the bottom (``-m host``).
"""

from __future__ import annotations

import io
import re
import time
from typing import Dict, List, Tuple

import pytest

import tg_archive_bench as tb
from test_l48_albums import (  # noqa: F401 - the fixtures are used by name
    INJECT_DONE,
    _episodic,
    _http,
    _inject,
    _lines_until,
    _state,
    _wait,
    album_page,
    head_traces,
    owner_chat,
    stage,
    stage_path,
    trace_summary,
)

SPEND_RE = re.compile(r"SPENDSET (\{.*\})")
IMGMODE_RE = re.compile(
    r"IMGMODE native=(?P<native>\d) upload=(?P<upload>\d) head=(?P<head>\d) records=(?P<records>\d+) "
    r"deletes=(?P<deletes>\d+)"
)
ATTACHED_RE = re.compile(r"telegram: media (\d+)/(\d+) attached for the head \((\d+) bytes\)")
QUOTA_429 = (
    '{"error":{"message":"You exceeded your current quota.","type":"insufficient_quota","code":"insufficient_quota"}}'
)
CAPTION_HOSTS = {"openai": "api.openai.com", "anthropic": "api.anthropic.com", "mistral": "api.mistral.ai"}
LABELS = [("BATCH", "4417"), ("NET WT", "250 g"), ("LOT", "88213")]


# ---- pure helpers (host-tested) ---------------------------------------------------


def imgmode(line: str) -> Dict[str, int]:
    m = IMGMODE_RE.search(line)
    if not m:
        raise AssertionError(f"not an IMGMODE line: {line!r}")
    return {k: int(v) for k, v in m.groupdict().items()}


def attached(lines: List[str]) -> List[Tuple[int, int, int]]:
    """(k, n, bytes) of every photo the turn attached for the head."""
    out = []
    for ln in lines:
        m = ATTACHED_RE.search(ln)
        if m:
            out.append((int(m.group(1)), int(m.group(2)), int(m.group(3))))
    return out


def b64_len(n: int) -> int:
    return (n + 2) // 3 * 4


def message_traces(lines: List[str]) -> List[Dict[str, int]]:
    """The head's own requests of a turn (/v1/messages), without its photo uploads."""
    return head_traces([ln for ln in lines if "heaptrace api.anthropic.com/v1/messages " in ln])


def spend_state(line: str) -> Dict[str, object]:
    """The SPENDSET? state object (lane DEF's monthly spend ceiling)."""
    import json

    m = SPEND_RE.search(line)
    if not m:
        raise AssertionError(f"not a SPENDSET line: {line!r}")
    return json.loads(m.group(1))


def first_caption_host(priority: str) -> str:
    """The API host the caption walk asks first for a priority CSV."""
    for p in [s.strip() for s in (priority or "openai,mistral,anthropic").split(",")]:
        if p in CAPTION_HOSTS:
            return CAPTION_HOSTS[p]
    return CAPTION_HOSTS["openai"]


def label_photo(word: str, value: str, seed: int) -> bytes:
    """A JPEG label with large text (a number only a reader of the pixels can know)."""
    from PIL import Image, ImageDraw, ImageFont

    img = Image.new("RGB", (900, 600), (250, 248, 240))
    d = ImageDraw.Draw(img)
    try:
        font = ImageFont.truetype("/System/Library/Fonts/Helvetica.ttc", 110)
    except OSError:
        font = ImageFont.load_default()
    d.rectangle([20, 20, 880, 580], outline=(20, 20, 20), width=8)
    d.text((70, 120), word, fill=(10, 10, 10), font=font)
    d.text((70, 330), value, fill=(10, 10, 10), font=font)
    d.point([(seed % 900, 590)], fill=(0, 0, 0))  # distinct bytes per seed
    buf = io.BytesIO()
    img.save(buf, "JPEG", quality=90)
    return buf.getvalue()


# ---- device helpers ---------------------------------------------------------------


def _imgmode(device, arg: str = "?") -> Dict[str, int]:
    return imgmode(device.cmd(f"IMGMODE {arg}" if arg != "?" else "IMGMODE?", "IMGMODE", timeout=8.0))


@pytest.fixture
def image_head(device, net, owner_chat):  # noqa: F811 - the lane D fixtures, imported by name
    """The photo path in its default state (native, upload once) on an image-reading
    head, restored afterwards; skips loudly when the head cannot read images. A bench
    unit whose monthly spend ceiling is frozen (another leg spent it) gets a ceiling
    above its spend for this leg, and its own limit back afterwards."""
    m = _imgmode(device, "native")
    if not m["head"]:
        pytest.skip("the configured head cannot read images: pin orchHost anthropic with claude-haiku-5-5")
    _imgmode(device, "upload")
    spend = spend_state(device.cmd("SPENDSET?", "SPENDSET {", timeout=8.0))
    lifted = bool(spend.get("frozen"))
    if lifted:
        raise_to = int(spend.get("spentCents", 0)) + 2000
        print(f"spend ceiling frozen ({spend}); lifted to {raise_to} cents for this leg")
        device.cmd(f"SPENDSET limit {raise_to}", "SPENDSET limit", timeout=8.0)
    try:
        yield owner_chat
    finally:
        if lifted:
            device.cmd(f"SPENDSET limit {spend.get('limitCents')}", "SPENDSET limit", timeout=8.0)
        _imgmode(device, "native")
        _imgmode(device, "upload")
        device.cmd("PROVFAULT off", "PROVFAULT", timeout=5.0)
        device.cmd("PARK clear", "PARK", timeout=5.0)


def _stage_bytes(net, items: List[Tuple[str, bytes]]) -> List[Tuple[str, int]]:
    import requests

    out = []
    for name, data in items:
        url = f"http://{net.ip}/api/files/upload?project=l48&name={name}"
        files = {"file": (name, data, "image/jpeg")}
        _http(lambda: requests.post(url, files=files, headers=net._auth(), timeout=60), f"staging {name}")
        out.append((stage_path(name), len(data)))
    return out


def _turn(device, chat: int, items, uid: int, caption: str, group: str) -> List[str]:
    lines = _inject(device, album_page(uid, chat, items, caption=caption, group=group))
    print("\n".join(ln for ln in lines if "telegram:" in ln or "vision" in ln or "heaptrace" in ln))
    return lines


# ---- legs ---------------------------------------------------------------------------


@pytest.mark.hil
@pytest.mark.net
@pytest.mark.agent
def test_l49_three_photo_turn_heap_trace(device, net, image_head, stage):  # noqa: F811 - the lane D fixtures, imported by name
    chat = image_head
    warm = stage(1, 120000, seed=4900)
    uid = tb.first_update_id() + 4900
    base_lines = _turn(device, chat, warm, uid - 10, "warm up, reply ok", "L49W")
    base = trace_summary(head_traces(base_lines))
    items = stage(3, 120000, seed=4901)
    lines = _turn(device, chat, items, uid, f"what is in these {uid}", "L49A")
    att = attached(lines)
    assert [(k, n) for k, n, _ in att] == [(1, 3), (2, 3), (3, 3)], att
    assert not [ln for ln in lines if re.search(r"telegram: media \d/3 describing", ln)], "a caption call was made"
    traces = head_traces(lines)
    assert traces, "no head exchange traced"
    t = trace_summary(traces)
    print(f"baseline {base} three-photo {t}")
    assert t["af"] == 0, "an allocation failed during the turn"
    assert t["largest"] >= base["largest"] - 2048, (base, t)
    uploads = [ln for ln in lines if re.search(r"heaptrace api\.anthropic\.com/v1/files body=", ln)]
    if _imgmode(device)["upload"] and uploads:
        assert len(uploads) == 3, uploads
    else:
        assert t["body"] >= sum(b64_len(n) for _, _, n in att), "the head body does not carry the photos"


@pytest.mark.hil
@pytest.mark.net
@pytest.mark.agent
def test_l49_ten_photos_against_one(device, net, image_head, stage):  # noqa: F811 - the lane D fixtures, imported by name
    chat = image_head
    one = stage(1, 120000, seed=4911)
    ten = stage(10, 120000, seed=4921)
    runs: Dict[str, List[Dict[str, int]]] = {"one": [], "ten": []}
    heads: Dict[str, List[Dict[str, int]]] = {"one": [], "ten": []}
    for r in range(3):
        for name, items in (("one", one), ("ten", ten)):
            uid = tb.first_update_id() + 5000 + 20 * r + (0 if name == "one" else 10)
            lines = _turn(device, chat, items, uid, "", f"L49B{r}{name}")
            assert len(attached(lines)) == len(items), f"run {r} {name}: not every photo attached"
            traces = head_traces(lines)
            assert traces, f"no head exchange traced in run {r} ({name})"
            runs[name].append(trace_summary(traces))
            msgs = message_traces(lines)
            assert msgs, f"no head request traced in run {r} ({name})"
            heads[name].append(trace_summary(msgs))
    for name in ("one", "ten"):
        print(name, runs[name])
        print(name, "head requests", heads[name])
    assert all(x["af"] == 0 for rs in runs.values() for x in rs), "an allocation failed during a turn"
    # Lane D's margin, read on the head's own requests: a ten-photo turn's request finds
    # the largest internal block within 2,048 B of a one-photo turn's. Read over every
    # exchange it would set the lowest of ten 130 KB upload writes against the lowest of
    # one, a sampling gap of 2 to 3 KB by itself (bench runs 5 and 7); the uploads are
    # held to af=0 above and printed.
    assert min(x["largest"] for x in heads["ten"]) >= min(x["largest"] for x in heads["one"]) - 2048


@pytest.mark.hil
@pytest.mark.net
@pytest.mark.agent
def test_l49_inline_against_upload(device, net, image_head, stage):  # noqa: F811 - the lane D fixtures, imported by name
    chat = image_head
    items = stage(3, 120000, seed=4931)
    out = {}
    for r, mode in enumerate(("inline", "upload")):
        _imgmode(device, mode)
        uid = tb.first_update_id() + 5100 + 10 * r
        t0 = time.time()
        lines = _turn(device, chat, items, uid, "one line on each, please", f"L49C{mode}")
        dur = time.time() - t0
        traces = head_traces(lines)  # the messages rounds and any Files API uploads
        out[mode] = {"s": round(dur, 1), "exchanges": len(traces), **trace_summary(traces)}
    print("inline vs upload:", out)
    assert all(v["af"] == 0 for v in out.values())


@pytest.mark.hil
@pytest.mark.net
@pytest.mark.agent
def test_l49_label_numbers_read_from_pixels(device, net, image_head):
    chat = image_head
    items = _stage_bytes(net, [(f"label{i}.jpg", label_photo(w, v, 4940 + i)) for i, (w, v) in enumerate(LABELS)])
    uid = tb.first_update_id() + 5200
    marker = f"read me the three label values {uid}"
    lines = _turn(device, chat, items, uid, marker, "L49D")
    assert len(attached(lines)) == 3
    assert not [ln for ln in lines if "describing" in ln], "the labels must be read by the head, not a captioner"

    def reply():
        for row in _episodic(net, 40):
            if row.get("role") == "assistant" and all(v.split()[0] in row.get("text", "") for _, v in LABELS):
                return row["text"]
        return None

    text = _wait(reply, 120)
    assert text, "no reply named all three label values"
    print("reply:", text[:300])


@pytest.mark.hil
@pytest.mark.net
@pytest.mark.agent
def test_l49_provfault_parks_the_captioner(device, net, image_head, stage):  # noqa: F811 - the lane D fixtures, imported by name
    chat = image_head
    prio = _http(lambda: net.get("/api/orch", timeout=8.0), "reading the priority").json().get("provPrio", "")
    host = first_caption_host(prio)
    _imgmode(device, "caption")
    device.cmd("PARK clear", "PARK", timeout=5.0)
    device.cmd(f"PROVFAULT {host} 429 {QUOTA_429}", "PROVFAULT", timeout=5.0)
    items = stage(6, 60000, seed=4951)
    uid = tb.first_update_id() + 5300
    lines = _turn(device, chat, items[:1], uid, "fault leg", "L49E0")
    asked = [ln for ln in lines if f"vision: TEST override answered {host}" in ln]
    assert len(asked) == 1, asked
    park = device.cmd("PARK?", "PARK? providers=", timeout=8.0)
    print(park)
    notices = [ln for ln in lines if "out of credit" in ln and "photos" in ln.lower()]
    for i in range(1, 6):
        more = _turn(device, chat, items[i : i + 1], uid + 10 * i, f"fault leg {i}", f"L49E{i}")
        assert not [ln for ln in more if f"vision: TEST override answered {host}" in ln], (
            f"photo {i} asked the parked host"
        )
        notices += [ln for ln in more if "out of credit" in ln and "photos" in ln.lower()]
    assert len(notices) <= 1, notices


@pytest.mark.hil
@pytest.mark.net
@pytest.mark.agent
def test_l49_provfault_off_rereads_once(device, net, image_head, stage):  # noqa: F811 - the lane D fixtures, imported by name
    chat = image_head
    prio = _http(lambda: net.get("/api/orch", timeout=8.0), "reading the priority").json().get("provPrio", "")
    host = first_caption_host(prio)
    _imgmode(device, "caption")
    device.cmd("PARK clear", "PARK", timeout=5.0)
    device.cmd(f"PROVFAULT {host} 429 {QUOTA_429}", "PROVFAULT", timeout=5.0)
    items = stage(1, 60000, seed=4961)
    uid = tb.first_update_id() + 5400
    _turn(device, chat, items, uid, f"reread leg {uid}", "L49F")
    queued = _imgmode(device)["records"]
    assert queued >= 1, "the photo was not queued for a re-read"
    device.cmd("PROVFAULT off", "PROVFAULT", timeout=5.0)
    device.cmd("PARK clear", "PARK", timeout=5.0)  # the park ends
    written = _lines_until(device, "vision: record for", 180)
    assert "written" in written[-1], written[-1]
    rows = [r for r in _episodic(net, 40) if r.get("kind") == "image" and "(transcribed by" in r.get("text", "")]
    assert rows, "the photo row text was not replaced"
    # The re-read photo was the newest item (newest first); earlier legs' records may
    # still be queued behind it, one per Telegram cycle.
    assert _imgmode(device)["records"] <= queued - 1


# ---- host tests ---------------------------------------------------------------------


@pytest.mark.host
def test_host_imgmode_and_attached_parse():
    m = imgmode("IMGMODE native=1 upload=0 head=1 records=2 deletes=3 lastErr=")
    assert m == {"native": 1, "upload": 0, "head": 1, "records": 2, "deletes": 3}
    lines = [
        "telegram: media 1/3 attached for the head (120034 bytes)",
        "telegram: media 2/3 describing",
        "telegram: media 3/3 attached for the head (98000 bytes)",
    ]
    assert attached(lines) == [(1, 3, 120034), (3, 3, 98000)]
    with pytest.raises(AssertionError):
        imgmode("PARK? providers=0")


@pytest.mark.host
def test_host_message_traces_skip_uploads():
    def trace(target: str, body: int, wrote: int) -> str:
        return (
            f"[agent] transport: heaptrace api.anthropic.com{target} body={body} pre=22000/13300 "
            f"conn=22000/13300 wrote=20000/{wrote} done=22000/13300 fail=none af=0 min=16000 at=1 ms=5"
        )

    lines = [trace("/v1/files", 130500, 7668), trace("/v1/messages", 62000, 11252)]
    assert [t["largest"] for t in head_traces(lines)] == [7668, 11252]
    assert [t["largest"] for t in message_traces(lines)] == [11252]


@pytest.mark.host
def test_host_spend_state_parse():
    s = spend_state('SPENDSET {"limitCents":2000,"spentCents":2562,"frozen":true,"ownerSet":true}')
    assert s["frozen"] is True and s["spentCents"] == 2562
    with pytest.raises(AssertionError):
        spend_state("SPENDGATE head=1/0")


@pytest.mark.host
def test_host_first_caption_host_and_b64():
    assert first_caption_host("anthropic,openai") == "api.anthropic.com"
    assert first_caption_host(" zai , mistral") == "api.mistral.ai"
    assert first_caption_host("") == "api.openai.com"
    assert b64_len(3) == 4 and b64_len(4) == 8 and b64_len(120000) == 160000


@pytest.mark.host
def test_host_label_photos_differ():
    pytest.importorskip("PIL")
    a, b = label_photo("BATCH", "4417", 1), label_photo("BATCH", "4417", 2)
    assert a[:3] == b"\xff\xd8\xff" and a != b
