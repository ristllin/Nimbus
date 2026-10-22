"""L48 - albums and bursts as one turn, and the inbound storm guards (lane D, wave 2).

Host-tested first (test/test_tg_burst, test_tg_settle, test_tg_dup_guard,
test_orch_msg_batch, test_media_label, test_tg_archive): a page is held until an
album has fully arrived, every photo is fetched and described inside its turn, the
turn runs with every item, the page is acked only after its turns returned, a
duplicate costs no turn, the hourly ceiling bounds inbound turns, and a page that
crashes the device twice is acked unprocessed. These legs prove it on hardware with
no phone and no bot token: TGINJECT pages whose photos are card files, so neither the
dedicated test bot nor the test chat is needed (the ``locks/tg-bot`` lock is not taken).
The photos are staged over HTTP into the Files store (project ``l48``, file id
``LOCAL_/mem/files/l48/<name>``, the project removed afterwards), not over the console:
the console's FSPUT stalled at its acks on unit L while the cloud relay reconnected, and
a stalled FSPUT leaves the console in its data mode, so every later command timed out.
Each leg first waits until the device has been up for 30 s (the boot-time relay connect
and first renders are done) and every guard read retries until the guard has booted.

Unit A (SD card, TEST build, head provider pinned to claude-haiku-5-5 with effort low,
the current captioner for the descriptions):

* ``test_l48_album_of_three_is_one_turn``: 3 photos with one caption: 3 downloads and
  3 descriptions, then exactly one turn holding ``[image 1/3]`` to ``[image 3/3]``;
  ``pollStackMin`` at or above the baseline minus 512 B; the internal largest block and
  free PSRAM back to the baseline once the turn is done.
* ``test_l48_album_and_text_is_one_turn``: the album and a follow-up text in one page:
  one turn, the text after the third photo.
* ``test_l48_eleven_photos_are_two_turns``: a 10-photo album plus one more photo: the
  first turn holds 10, the 11th runs on the next cycle; nothing is lost.
* ``test_l48_ten_photos_against_one``: the head request body, the largest internal block
  and the minimum free heap of a 10-photo turn against a single-photo turn, 3 runs each
  (``transport: heaptrace`` lines); printed for the PR, bounded here.

Unit L (the fill unit, TEST build):

* ``test_l48_storm_one_turn_for_fifty_pages``: the settings store filled (the offset
  cannot be stored), then the same page injected 50 times with a restart after the
  25th: exactly one paid turn, one owner notice, a duplicate count of 49 (``TGGUARD?``);
  the restart keeps the count (the guard record survives it). The store is freed in a
  ``finally``.
* ``test_l48_guard_is_fresh_after_a_flash``: the first boot of a newly flashed image
  starts a fresh guard record and counts nothing (a fresh device); read from the durable
  log over HTTP (that boot is over before a console could watch it).

Human gate (manual, H2-1 to H2-4): a real 3-photo album from the phone gets one reply
covering all three; a text 2 s after an album joins it; a text 20 s after gets its own
reply, in order; a reset while photo 2 is described processes the album once after the
boot (injected pages do not survive a restart, so this leg needs a real album).

The pure helpers are host-tested at the bottom (``-m host``).
"""

from __future__ import annotations

import json
import os
import re
import time
from typing import Dict, Iterable, List, Optional, Tuple

import pytest

import tg_archive_bench as tb
from device import command_line_problem

CHAT_DEFAULT = 4848484848  # a synthetic owner chat for the injected pages
INJECT_DONE = "telegram: injected page:"
LABEL_RE = re.compile(r"\[image (\d+)/(\d+)(?: ref=([0-9a-f]{16}))?")
GUARD_RE = re.compile(
    r"TGGUARD dups=(\d+) ceiling=(\d+) poison=(\d+) tries=(\d+) holding=(\d)(?: booted=(\d))?(?: kept=(\d))?"
)
STAGE_PROJECT = "l48"  # the Files store project the photos are staged in (removed afterwards)
PACE_BYTES = 64  # a long console line is written in pieces this size...
PACE_GAP_S = 0.01  # ...this far apart (6.4 KB/s: the 1.5 KB receive buffer covers a 240 ms loop pass)
SETTLE_UPTIME_MS = 30000
HEAPTRACE_RE = re.compile(
    r"transport: heaptrace (?P<target>\S+) body=(?P<body>\d+) pre=(?P<pre_f>\d+)/(?P<pre_l>\d+) "
    r"conn=\d+/(?P<conn_l>\d+) wrote=\d+/(?P<wrote_l>\d+) done=\d+/(?P<done_l>\d+) "
    r"fail=(?P<fail>\S+) af=(?P<af>\d+) min=(?P<min>\d+|-)"
)


# ---- pure helpers (host-tested) ---------------------------------------------------


def labels_in(text: str) -> List[Tuple[int, int, Optional[str]]]:
    """Every ``[image k/N ref=...]`` label of a turn text, in order."""
    return [(int(m.group(1)), int(m.group(2)), m.group(3)) for m in LABEL_RE.finditer(text or "")]


def album_turn_rows(rows: Iterable[dict], n: int) -> List[dict]:
    """User message rows (the turn text, not the per-photo image rows) that hold an
    ``[image k/n]`` label: one per turn that carried that album."""
    out = []
    for r in rows:
        if r.get("role") != "user" or (r.get("kind") or "message") != "message":
            continue
        if any(total == n for _, total, _ in labels_in(r.get("text") or "")):
            out.append(r)
    return out


def guard_counts(line: str) -> Dict[str, int]:
    """The counters of a ``TGGUARD`` reply; ``booted`` and ``kept`` are -1 when the build
    predates them."""
    m = GUARD_RE.search(line or "")
    if not m:
        raise ValueError(f"not a TGGUARD line: {line!r}")
    keys = ("dups", "ceiling", "poison", "tries", "holding", "booted", "kept")
    return {k: -1 if v is None else int(v) for k, v in zip(keys, m.groups())}


BOOT_RE = re.compile(r"\bboot: n=\d+ .*?\bbuild=(\S+)")


def log_files_oldest_first(names: Iterable[str]) -> List[str]:
    """The durable log's files, oldest first: ``nimbus.log.<n>`` from the highest n
    down, then the active ``nimbus.log``."""
    rotated = sorted(
        (n for n in names if re.fullmatch(r"nimbus\.log\.\d+", n)), key=lambda n: -int(n.rsplit(".", 1)[1])
    )
    return rotated + [n for n in names if n == "nimbus.log"]


def first_boot_guard_line(text: str, build: str) -> Optional[str]:
    """The ``storm guard`` line of the first boot of ``build`` in a durable log, or None
    (no boot of that build, or its first boot logged no guard line)."""
    in_first = False
    for line in text.splitlines():
        m = BOOT_RE.search(line)
        if m:
            if in_first:
                return None
            in_first = m.group(1) == build
            continue
        if in_first and "telegram: storm guard" in line:
            return line
    return None


def photo_near(target_bytes: int, seed: int) -> bytes:
    """A noise JPEG of at least ``target_bytes``, grown from a small frame (the bench
    helper starts at 600 px, about 320 KB, whatever the target)."""
    side = 160
    while True:
        data = tb.noise_jpeg(side, side * 3 // 4, seed)
        if len(data) >= target_bytes:
            return data
        side = int(side * 1.1) + 1


def stage_path(name: str) -> str:
    """The card path of a photo staged in the Files store (what ``LOCAL_`` names)."""
    return f"/mem/files/{STAGE_PROJECT}/{name}"


def head_traces(lines: Iterable[str], host: str = "api.anthropic.com") -> List[Dict[str, int]]:
    """The head exchanges of a turn: body size, the smallest largest-block seen across
    the exchange's stages, and the minimum free heap (None when not sampled)."""
    out = []
    for line in lines:
        m = HEAPTRACE_RE.search(line)
        if not m or not m.group("target").startswith(host):
            continue
        largest = min(int(m.group(k)) for k in ("pre_l", "conn_l", "wrote_l", "done_l") if int(m.group(k)))
        out.append(
            {
                "body": int(m.group("body")),
                "largest": largest,
                "min": None if m.group("min") == "-" else int(m.group("min")),
                "af": int(m.group("af")),
            }
        )
    return out


def album_updates(
    uid: int, chat: int, items: List[Tuple[str, int]], caption: str = "", group: str = "", text: str = ""
) -> List[dict]:
    """The updates of an album of card files (path, size), the caption on the first
    item, then an optional follow-up text."""
    ups = []
    for i, (path, size) in enumerate(items):
        u = tb.local_photo_update(uid + i, chat, path, size, group=group or None)
        if i == 0 and caption:
            u["message"]["caption"] = caption
        ups.append(u)
    if text:
        ups.append(tb.text_update(uid + len(items), chat, text))
    return ups


def album_page(
    uid: int, chat: int, items: List[Tuple[str, int]], caption: str = "", group: str = "", text: str = ""
) -> str:
    """One getUpdates page (compact JSON: it rides one console line)."""
    return tb.page(album_updates(uid, chat, items, caption, group, text))


def eleven_page(uid: int, chat: int, items: List[Tuple[str, int]]) -> str:
    """A 10-photo album plus one lone photo in one page, each captioned with the page's
    first update id (``eleven <uid>``, ``lone <uid>``) so its turns are found among
    earlier runs'. Each photo carries only the rendition the device keeps (no
    thumbnail): with the thumbnails the page is longer than one console line."""
    lone = tb.local_photo_update(uid + 10, chat, items[10][0], items[10][1])
    lone["message"]["caption"] = f"lone {uid}"
    ups = album_updates(uid, chat, items[:10], caption=f"eleven {uid}", group="L48C") + [lone]
    for u in ups:
        u["message"]["photo"] = u["message"]["photo"][-1:]
    return tb.page(ups)


# ---- device helpers -----------------------------------------------------------------


def _chat() -> int:
    return int(os.environ.get("NIMBUS_TEST_TG_CHAT") or CHAT_DEFAULT)


def _state(net) -> dict:
    return net.get_json("/api/state", timeout=8.0)


def _episodic(net, limit: int = 60) -> List[dict]:
    return net.get_json(f"/api/mem/episodic?limit={limit}", timeout=8.0).get("messages", [])


def _wait(pred, timeout: float, every: float = 2.0):
    end = time.time() + timeout
    while time.time() < end:
        got = pred()
        if got:
            return got
        time.sleep(every)
    return pred()


def _lines_until(device, done: str, timeout: float) -> List[str]:
    """Every console line up to (and with) the first one containing ``done``."""
    out, end = [], time.time() + timeout
    while True:
        line = device.expect_re(r".", timeout=max(1.0, end - time.time())).string
        out.append(line)
        if done in line:
            return out


def paced_chunks(line: str, size: int = PACE_BYTES) -> List[bytes]:
    """One console line (with its newline) cut into the pieces it is written in."""
    data = (line + "\n").encode("utf-8")
    return [data[i : i + size] for i in range(0, len(data), size)]


def _cmd_paced(device, line: str, expect: str, timeout: float) -> str:
    """``device.cmd`` for a long line, written in small paced pieces. The console's
    receive buffer holds 1.5 KB and its loop drains 256 B a pass: a 3.3 KB page
    written at USB speed overflowed it on unit L (22 Oct), lost bytes with its newline
    among them, and was never answered."""
    problem = command_line_problem(line)
    if problem:
        raise AssertionError(problem)
    device.drain(quiet=0.2)
    ser = device._require_open()
    for piece in paced_chunks(line):
        ser.write(piece)
        ser.flush()
        time.sleep(PACE_GAP_S)
    return device.expect(expect, timeout=timeout)


def _inject(device, page: str, timeout: float = 900.0) -> List[str]:
    """TGINJECT one page and return the console lines until it has run (the album's
    downloads, descriptions and turn all happen before the summary line)."""
    for _ in range(30):
        line = _cmd_paced(device, "TGINJECT " + page, "TGINJECT", timeout=15.0)
        if "busy" not in line:
            break
        time.sleep(3.0)
    assert "queued" in line, f"the page was not queued: {line}"
    return _lines_until(device, INJECT_DONE, timeout)


def _guard(device, timeout: float = 20.0) -> Dict[str, int]:
    """The guard's counters once it has booted (a read before that is all zeros, which
    is what the 22 Oct bench mistook for a lost count). Retries a garbled or early read."""
    end, last = time.time() + timeout, None
    while True:
        try:
            g = guard_counts(device.cmd("TGGUARD?", "TGGUARD", timeout=5.0))
            if g["booted"] != 0:
                return g
            last = g
        except Exception as e:  # noqa: BLE001 - console chatter or a cut line: read again
            last = e
        if time.time() >= end:
            raise AssertionError(f"no booted guard read within {timeout:.0f} s: {last}")
        time.sleep(1.0)


def _settle(net, uptime_ms: int = SETTLE_UPTIME_MS, timeout: float = 90.0) -> None:
    """Wait (over HTTP) until the device has been up for ``uptime_ms``."""

    def up():
        try:
            return _state(net).get("uptimeMs", 0) >= uptime_ms
        except Exception:  # noqa: BLE001 - still booting or busy
            return False

    assert _wait(up, timeout, every=3.0), f"the device did not settle ({uptime_ms} ms up) within {timeout:.0f} s"


def _http(call, what: str, tries: int = 4):
    """A request retried on a transport error or a busy answer (429/5xx)."""
    last = None
    for t in range(tries):
        try:
            r = call()
            if r.status_code < 400:
                return r
            last = f"HTTP {r.status_code} {r.text[:160]}"
            if r.status_code != 429 and r.status_code < 500:
                break
        except Exception as e:  # noqa: BLE001 - a dropped connection: retry
            last = repr(e)
        time.sleep(3.0 * (t + 1))
    raise AssertionError(f"{what} failed: {last}")


@pytest.fixture
def owner_chat(device, net):
    """The injected chat on the allowlist (it is the owner: the first entry), restored
    afterwards. No bot token is set: replies and notices are logged, not sent."""
    device.ensure_mode(1)
    _settle(net)
    before = _http(lambda: net.get("/api/orch", timeout=8.0), "reading the allowlist").json().get("tgAllow", "")
    chat = _chat()
    _clear_chat_files(net, chat)
    _http(lambda: net.post("/api/orch", {"tgAllow": str(chat)}, timeout=8.0), "setting the allowlist")
    try:
        yield chat
    finally:
        _http(lambda: net.post("/api/orch", {"tgAllow": before}, timeout=8.0), "restoring the allowlist")
        _clear_chat_files(net, chat)


def _clear_chat_files(net, chat: int) -> None:
    """Remove the photos earlier injected turns stored for the synthetic chat (project
    ``chat-<id>``): they count against that sender's 8 MB limit, and on 22 Oct the 26th
    staged photo of a bench run was refused for it ("over the sender's storage limit").
    Only the synthetic chat: a real test chat's files are left alone."""
    if chat != CHAT_DEFAULT:
        return
    r = net.post(f"/api/files/rmproject?project=chat-{chat}&confirm=DELETE", {}, timeout=20.0)
    print(f"cleared chat-{chat}: HTTP {r.status_code} {r.text[:60]}")


@pytest.fixture
def stage(net):
    """``stage(n, size, seed)`` puts n noise JPEGs into the Files store over HTTP and
    returns their (card path, size); the whole project is removed afterwards."""
    import requests

    def put(n: int, size: int, seed: int) -> List[Tuple[str, int]]:
        items = []
        for i in range(n):
            data = photo_near(size, seed=seed + i)
            name = f"p{seed}_{i}.jpg"
            url = f"http://{net.ip}/api/files/upload?project={STAGE_PROJECT}&name={name}"
            files = {"file": (name, data, "image/jpeg")}
            _http(lambda: requests.post(url, files=files, headers=net._auth(), timeout=60), f"staging {name}")
            items.append((stage_path(name), len(data)))
        return items

    try:
        yield put
    finally:
        q = f"/api/files/rmproject?project={STAGE_PROJECT}&confirm=DELETE"
        r = net.post(q, {}, timeout=20.0)
        print(f"removed the staged photos: HTTP {r.status_code} {r.text[:80]}")


# ---- unit A: albums ----------------------------------------------------------------


@pytest.mark.hil
@pytest.mark.net
@pytest.mark.agent
def test_l48_album_of_three_is_one_turn(device, net, owner_chat, stage):
    """The baseline is read after one single-photo turn on the same boot: the first turn
    after a boot sets the poll task's stack low-water mark and the long-lived buffers, so
    a baseline read before any turn (16 KB free on 22 Oct) compares a cold device."""
    warm = stage(1, 120000, seed=4800)
    uid = tb.first_update_id() + 4800
    _inject(device, album_page(uid - 10, owner_chat, warm, caption="warm up, reply ok"))
    time.sleep(10.0)
    items = stage(3, 120000, seed=4801)
    base = _state(net)
    caption = f"All of this for dinner {uid}"
    lines = _inject(device, album_page(uid, owner_chat, items, caption=caption, group="L48A"))
    fetched = [ln for ln in lines if "telegram: attachment downloaded" in ln]
    described = [ln for ln in lines if re.search(r"telegram: media \d/3 describing", ln)]
    summary = lines[-1]
    print("\n".join(ln for ln in lines if "telegram:" in ln or "vision" in ln))
    assert len(fetched) == 3, f"expected 3 downloads, got {len(fetched)}"
    assert len(described) in (0, 3), f"expected 3 descriptions (or none without a captioner): {described}"
    assert "3 messages batched into 1 turn" in summary, summary
    rows = _wait(lambda: [r for r in album_turn_rows(_episodic(net), 3) if caption in r["text"]], 120)
    assert len(rows) == 1, f"expected exactly one turn for the album, got {len(rows)}"
    got = labels_in(rows[0]["text"])
    assert [(k, n) for k, n, _ in got] == [(1, 3), (2, 3), (3, 3)], got
    assert all(ref for _, _, ref in got), "every fetched photo carries its content hash"
    assert f"They wrote: {caption}" in rows[0]["text"]

    def settled():
        s = _state(net)
        return s if s["mem"]["intLargest"] >= base["mem"]["intLargest"] - 512 else None

    after = _wait(settled, 60) or _state(net)
    print(
        f"pollStackMin {base['mem']['pollStackMin']} -> {after['mem']['pollStackMin']}, "
        f"intLargest {base['mem']['intLargest']} -> {after['mem']['intLargest']}, "
        f"psramFree {base['psramFree']} -> {after['psramFree']}"
    )
    assert after["mem"]["pollStackMin"] >= base["mem"]["pollStackMin"] - 512
    assert after["mem"]["intLargest"] >= base["mem"]["intLargest"] - 512
    assert after["psramFree"] >= base["psramFree"] - 16384, "PSRAM did not come back (slots, staging)"


@pytest.mark.hil
@pytest.mark.net
@pytest.mark.agent
def test_l48_album_and_text_is_one_turn(device, net, owner_chat, stage):
    items = stage(3, 90000, seed=4811)
    uid = tb.first_update_id() + 4810
    marker = f"did you see all three {uid}?"
    lines = _inject(device, album_page(uid, owner_chat, items, caption="lunch", group="L48B", text=marker))
    assert "4 messages batched into 1 turn" in lines[-1], lines[-1]
    rows = _wait(lambda: [r for r in album_turn_rows(_episodic(net), 3) if marker in r["text"]], 120)
    assert len(rows) == 1, rows
    text = rows[0]["text"]
    assert text.find("[image 3/3") < text.find(marker), "the text must come after the third photo"


@pytest.mark.hil
@pytest.mark.net
@pytest.mark.agent
def test_l48_eleven_photos_are_two_turns(device, net, owner_chat, stage):
    items = stage(11, 40000, seed=4821)
    uid = tb.first_update_id() + 4820
    lines = _inject(device, eleven_page(uid, owner_chat, items))
    assert "10 messages batched into 1 turn" in lines[-1] and "resumes next cycle" in lines[-1], lines[-1]
    second = _lines_until(device, INJECT_DONE, 600)
    assert "1 message batched into 1 turn" in second[-1], second[-1]
    tens = _wait(lambda: [r for r in album_turn_rows(_episodic(net), 10) if f"eleven {uid}" in r["text"]], 120)
    ones = [r for r in album_turn_rows(_episodic(net), 1) if f"lone {uid}" in r["text"]]
    assert len(tens) == 1 and len(labels_in(tens[0]["text"])) == 10, tens
    assert f"lone {uid}" not in tens[0]["text"], "the 11th photo must not ride the 10-photo turn"
    assert len(ones) == 1, f"the 11th photo runs as its own turn: {ones}"


def trace_summary(traces: List[Dict[str, int]]) -> Dict[str, int]:
    """One turn's head exchanges reduced to its largest body, its smallest largest-block,
    its lowest minimum free heap and its allocation failures."""
    mins = [t["min"] for t in traces if t["min"] is not None]
    return {
        "body": max(t["body"] for t in traces),
        "largest": min(t["largest"] for t in traces),
        "min": min(mins) if mins else 0,
        "af": sum(t["af"] for t in traces),
    }


@pytest.mark.hil
@pytest.mark.net
@pytest.mark.agent
def test_l48_ten_photos_against_one(device, net, owner_chat, stage):
    one = stage(1, 120000, seed=4831)
    ten = stage(10, 120000, seed=4841)
    runs: Dict[str, List[Dict[str, int]]] = {"one": [], "ten": []}
    for r in range(3):
        for name, items in (("one", one), ("ten", ten)):
            uid = tb.first_update_id() + 4900 + 20 * r + (0 if name == "one" else 10)
            lines = _inject(device, album_page(uid, owner_chat, items, group=f"L48D{r}{name}"))
            print(f"run {r} {name}: {lines[-1]}")
            traces = head_traces(lines)
            assert traces, f"no head exchange traced in run {r} ({name})"
            runs[name].append(trace_summary(traces))
    for name in ("one", "ten"):
        print(name, runs[name])
    assert all(r["af"] == 0 for rs in runs.values() for r in rs), "an allocation failed during a turn"
    one_body = max(r["body"] for r in runs["one"])
    ten_body = max(r["body"] for r in runs["ten"])
    assert ten_body < one_body + 10 * 2600 + 4096, "a 10-photo turn grew more than its labels and descriptions"
    assert min(r["largest"] for r in runs["ten"]) >= min(r["largest"] for r in runs["one"]) - 2048


# ---- unit L: the storm guards ------------------------------------------------------


@pytest.mark.hil
@pytest.mark.net
@pytest.mark.agent
def test_l48_storm_one_turn_for_fifty_pages(device, net, owner_chat):
    """The 19 Oct storm on this build: the same page served again and again with a full
    settings store, and a restart in the middle of the episode."""
    import test_l42_replay_full_nvs as l42

    uid = tb.first_update_id() + 4950
    marker = f"storm leg {uid}: reply with one word"
    page = tb.page([tb.text_update(uid, owner_chat, marker)])
    before = _guard(device)
    l42.nvs_fill(device, 4000)
    try:
        notices = 0
        for k in range(50):
            if k == 25:
                l42.restart_and_wait(net)
                device.reopen_after_reenumerate()
                mid = _guard(device)
                print(f"after the restart: {mid}")
                assert mid["booted"] == 1, f"the guard did not report its boot: {mid}"
                assert mid["kept"] == 1, f"the restart rebuilt the guard record: {mid}"
                assert mid["dups"] == before["dups"] + 24, f"the restart lost the duplicate count: {mid}"
            lines = _inject(device, page, timeout=300)
            notices += sum("guard notice (duplicate)" in ln for ln in lines)
        after = _guard(device)
        rows = [r for r in _episodic(net, 120) if r.get("role") == "user" and marker in (r.get("text") or "")]
        assert len(rows) == 1, f"expected one paid turn, got {len(rows)}"
        assert notices == 1, f"expected one owner notice, got {notices}"
        assert after["dups"] == before["dups"] + 49, f"duplicate count {after['dups'] - before['dups']}"
    finally:
        l42.nvs_free(device)


@pytest.mark.hil
@pytest.mark.net
def test_l48_guard_is_fresh_after_a_flash():
    """The first boot of a newly flashed image rebuilds the guard record (the previous
    image's build id differs), so it counts nothing. That boot is over before any
    console can watch it (the flash's own restart starts it, and opening unit L's port
    restarts the board again), so the leg reads it from the durable log over HTTP only,
    with no fixture that would touch the device: the guard line of the first boot whose
    boot record names the running build. Needs NIMBUS_TEST_IP and NIMBUS_TEST_TOKEN;
    skips when that boot has rotated out of the log."""
    import requests

    ip, tok = os.environ.get("NIMBUS_TEST_IP"), os.environ.get("NIMBUS_TEST_TOKEN")
    if not (ip and tok):
        pytest.skip("reads the durable log over HTTP: set NIMBUS_TEST_IP and NIMBUS_TEST_TOKEN")
    hdr, base = {"X-Nimbus-Token": tok}, f"http://{ip}"
    build = requests.get(base + "/api/state", headers=hdr, timeout=8).json()["build"]
    names = [f["name"] for f in requests.get(base + "/api/errlog?list=1", headers=hdr, timeout=8).json()["files"]]
    text = ""
    for name in log_files_oldest_first(names):
        q = "" if name == "nimbus.log" else f"?file={name}"
        text += requests.get(base + "/api/errlog" + q, headers=hdr, timeout=30).text
    line = first_boot_guard_line(text, build)
    print(f"build {build}: {line}")
    if line is None:
        pytest.skip(f"the first boot of {build} is no longer in the durable log")
    assert "storm guard fresh" in line and "page tries 0" in line and "duplicates 0" in line, line


# ---- host tests for the helpers -------------------------------------------------------


@pytest.mark.host
def test_host_labels_and_album_rows():
    text = (
        "Message 1 of 4 from Ana:\n[image 1/3 ref=0123456789abcdef] pasta\n\nThey wrote: dinner\n\n"
        "Message 2 of 4 from Ana:\n[image 2/3 ref=fedcba9876543210] salad\n\n"
        "Message 3 of 4 from Ana:\n[image 3/3] (could not be fetched: too large)\n\n"
        "Message 4 of 4 from Ana:\ndid you see all three?"
    )
    assert labels_in(text) == [(1, 3, "0123456789abcdef"), (2, 3, "fedcba9876543210"), (3, 3, None)]
    rows = [
        {"role": "user", "kind": "message", "text": text},
        {"role": "user", "kind": "image", "text": "[image 1/3 ref=0123456789abcdef] pasta"},
        {"role": "assistant", "text": "[image 1/3] quoted back"},
    ]
    assert album_turn_rows(rows, 3) == [rows[0]]
    assert album_turn_rows(rows, 2) == []


@pytest.mark.host
def test_host_guard_counts():
    assert guard_counts("TGGUARD dups=49 ceiling=0 poison=0 tries=0 holding=0 booted=1 kept=1") == {
        "dups": 49,
        "ceiling": 0,
        "poison": 0,
        "tries": 0,
        "holding": 0,
        "booted": 1,
        "kept": 1,
    }
    assert guard_counts("TGGUARD dups=0 ceiling=0 poison=0 tries=0 holding=0 booted=0 kept=0")["booted"] == 0
    old = guard_counts("TGGUARD dups=3 ceiling=0 poison=0 tries=0 holding=1")
    assert old["booted"] == -1 and old["kept"] == -1
    with pytest.raises(ValueError):
        guard_counts("ERR unknown 'TGGUARD?'")


@pytest.mark.host
def test_host_head_traces():
    lines = [
        "[agent] transport: heaptrace api.anthropic.com/v1/messages body=71508 pre=24760/11764 conn=20000/9000 "
        "wrote=19000/8800 done=21000/9500 fail=none af=0 min=18000 at=1000 ms=900",
        "[agent] transport: heaptrace api.openai.com/v1/responses body=1000 pre=1/1 conn=1/1 wrote=1/1 done=1/1 "
        "fail=none af=0 min=- at=1 ms=1",
        "[agent] telegram: media 1/3 describing",
    ]
    t = head_traces(lines)
    assert t == [{"body": 71508, "largest": 8800, "min": 18000, "af": 0}]
    assert head_traces(lines, host="api.openai.com")[0]["min"] is None


@pytest.mark.host
def test_host_trace_summary():
    s = trace_summary(
        [
            {"body": 9000, "largest": 9000, "min": 20000, "af": 0},
            {"body": 12000, "largest": 8000, "min": None, "af": 1},
        ]
    )
    assert s == {"body": 12000, "largest": 8000, "min": 20000, "af": 1}


@pytest.mark.host
def test_host_album_page_shape():
    body = json.loads(album_page(100, 77, [("/mem/inject/a.jpg", 10), ("/mem/inject/b.jpg", 20)], "cap", "G", "hi"))
    msgs = [u["message"] for u in body["result"]]
    assert [u["update_id"] for u in body["result"]] == [100, 101, 102]
    assert msgs[0]["caption"] == "cap" and "caption" not in msgs[1]
    assert msgs[0]["media_group_id"] == "G" and msgs[1]["media_group_id"] == "G"
    assert msgs[0]["photo"][-1]["file_id"] == "LOCAL_/mem/inject/a.jpg"
    assert msgs[2]["text"] == "hi"


@pytest.mark.host
def test_host_staged_pages_fit_one_console_line():
    """The largest page a leg injects (ten photos plus one, staged paths, a 10-digit
    chat, the largest update ids) rides one console line: a longer line is cut and the
    leg would wait out a turn that never comes."""
    items = [(stage_path(f"p4821_{i}.jpg"), 99999999) for i in range(11)]
    page = eleven_page(999999999, 9999999999, items)
    body = json.loads(page)
    assert len(body["result"]) == 11 and body["result"][10]["message"].get("media_group_id") is None
    assert body["result"][0]["message"]["caption"] == "eleven 999999999"
    assert body["result"][10]["message"]["caption"] == "lone 999999999"
    assert body["result"][0]["message"]["photo"][-1]["file_id"] == "LOCAL_/mem/files/l48/p4821_0.jpg"
    assert command_line_problem("TGINJECT " + page) is None, len(page)
    assert len(page) < 3600, f"{len(page)} B leaves too little room under the console's line"
    big = album_page(999999999, 9999999999, items[:3], caption="All of this for dinner", group="L48A", text="x" * 64)
    assert command_line_problem("TGINJECT " + big) is None


@pytest.mark.host
def test_host_paced_chunks():
    page = "TGINJECT " + "x" * 3300
    pieces = paced_chunks(page)
    assert b"".join(pieces) == (page + "\n").encode()
    assert all(len(p) <= PACE_BYTES for p in pieces) and pieces[-1].endswith(b"\n")
    assert len(pieces) * PACE_GAP_S < 1.0, "a full page is written in under a second"


@pytest.mark.host
def test_host_first_boot_guard_line():
    log = "\n".join(
        [
            "0 92 boot boot: n=747 fw=v4.5.16 build=v4.5.16-1-g13e56dbe reason=crash(watchdog)(0x07)",
            "29 8836 - telegram: storm guard fresh (reset 6, page tries 0, duplicates 0)",
            "0 92 boot boot: n=748 fw=v4.5.16 build=v4.5.16-9-g4276e945 reason=crash(watchdog)(0x07)",
            "6 4141 - [sd] mounted",
            "23 5829 - telegram: storm guard fresh (reset 6, page tries 0, duplicates 0)",
            "0 92 boot boot: n=749 fw=v4.5.16 build=v4.5.16-9-g4276e945 reason=usb(0x15)",
            "23 2618 - telegram: storm guard kept (reset 11, page tries 0, duplicates 0)",
        ]
    )
    assert "n=" not in first_boot_guard_line(log, "v4.5.16-9-g4276e945")
    assert first_boot_guard_line(log, "v4.5.16-9-g4276e945").startswith("23 5829")
    assert first_boot_guard_line(log, "v4.5.16-1-g13e56dbe").startswith("29 8836")
    assert first_boot_guard_line(log, "v9") is None
    cut = log.replace("23 5829 - telegram: storm guard fresh (reset 6, page tries 0, duplicates 0)\n", "")
    assert first_boot_guard_line(cut, "v4.5.16-9-g4276e945") is None, "a first boot without its line is not a later one"
    names = ["nimbus.log", "nimbus.log.1", "nimbus.log.3", "nimbus.log.2"]
    assert log_files_oldest_first(names) == ["nimbus.log.3", "nimbus.log.2", "nimbus.log.1", "nimbus.log"]
