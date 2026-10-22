"""L48 - albums and bursts as one turn, and the inbound storm guards (lane D, wave 2).

Host-tested first (test/test_tg_burst, test_tg_settle, test_tg_dup_guard,
test_orch_msg_batch, test_media_label, test_tg_archive): a page is held until an
album has fully arrived, every photo is fetched and described inside its turn, the
turn runs with every item, the page is acked only after its turns returned, a
duplicate costs no turn, the hourly ceiling bounds inbound turns, and a page that
crashes the device twice is acked unprocessed. These legs prove it on hardware with
no phone and no bot token: TGINJECT pages whose photos are card files (FSPUT, file id
``LOCAL_/mem/...``), so neither the dedicated test bot nor the test chat is needed
(the ``locks/tg-bot`` lock is not taken).

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
  logs ``storm guard fresh`` and counts nothing (a fresh device); run it right after the
  flash, before any other leg.

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

CHAT_DEFAULT = 4848484848  # a synthetic owner chat for the injected pages
INJECT_DONE = "telegram: injected page:"
LABEL_RE = re.compile(r"\[image (\d+)/(\d+)(?: ref=([0-9a-f]{16}))?")
GUARD_RE = re.compile(r"TGGUARD dups=(\d+) ceiling=(\d+) poison=(\d+) tries=(\d+) holding=(\d)")
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
    m = GUARD_RE.search(line or "")
    if not m:
        raise ValueError(f"not a TGGUARD line: {line!r}")
    keys = ("dups", "ceiling", "poison", "tries", "holding")
    return {k: int(v) for k, v in zip(keys, m.groups())}


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


def album_page(
    uid: int, chat: int, items: List[Tuple[str, int]], caption: str = "", group: str = "", text: str = ""
) -> str:
    """One getUpdates page: an album of card files (path, size), the caption on the
    first item, then an optional follow-up text."""
    ups = []
    for i, (path, size) in enumerate(items):
        u = tb.local_photo_update(uid + i, chat, path, size, group=group or None)
        if i == 0 and caption:
            u["message"]["caption"] = caption
        ups.append(u)
    if text:
        ups.append(tb.text_update(uid + len(items), chat, text))
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


def _inject(device, page: str, timeout: float = 900.0) -> List[str]:
    """TGINJECT one page and return the console lines until it has run (the album's
    downloads, descriptions and turn all happen before the summary line)."""
    for _ in range(30):
        line = device.cmd("TGINJECT " + page, "TGINJECT", timeout=15.0)
        if "busy" not in line:
            break
        time.sleep(3.0)
    assert "queued" in line, f"the page was not queued: {line}"
    return _lines_until(device, INJECT_DONE, timeout)


def _guard(device) -> Dict[str, int]:
    return guard_counts(device.cmd("TGGUARD?", "TGGUARD", timeout=5.0))


@pytest.fixture
def owner_chat(device, net):
    """The injected chat on the allowlist (it is the owner: the first entry), restored
    afterwards. No bot token is set: replies and notices are logged, not sent."""
    device.ensure_mode(1)
    before = net.get_json("/api/orch", timeout=8.0).get("tgAllow", "")
    chat = _chat()
    r = net.post("/api/orch", {"tgAllow": str(chat)}, timeout=8.0)
    assert r.status_code == 200, f"setting the allowlist failed: HTTP {r.status_code}"
    try:
        yield chat
    finally:
        net.post("/api/orch", {"tgAllow": before}, timeout=8.0)


def _stage(device, n: int, size: int, seed: int) -> List[Tuple[str, int]]:
    items = []
    for i in range(n):
        data = tb.jpeg_near(size, seed=seed + i)
        path = f"/mem/inject/l48_{seed}_{i}.jpg"
        tb.fsput(device, data, path)
        items.append((path, len(data)))
    return items


# ---- unit A: albums ----------------------------------------------------------------


@pytest.mark.hil
@pytest.mark.net
@pytest.mark.agent
def test_l48_album_of_three_is_one_turn(device, net, owner_chat):
    items = _stage(device, 3, 120000, seed=4801)
    base = _state(net)
    uid = tb.first_update_id() + 4800
    lines = _inject(device, album_page(uid, owner_chat, items, caption="All of this for dinner", group="L48A"))
    fetched = [ln for ln in lines if "telegram: attachment downloaded" in ln]
    described = [ln for ln in lines if re.search(r"telegram: media \d/3 describing", ln)]
    summary = lines[-1]
    print("\n".join(ln for ln in lines if "telegram:" in ln or "vision" in ln))
    assert len(fetched) == 3, f"expected 3 downloads, got {len(fetched)}"
    assert len(described) in (0, 3), f"expected 3 descriptions (or none without a captioner): {described}"
    assert "3 messages batched into 1 turn" in summary, summary
    rows = _wait(lambda: album_turn_rows(_episodic(net), 3), 120)
    assert len(rows) == 1, f"expected exactly one turn for the album, got {len(rows)}"
    got = labels_in(rows[0]["text"])
    assert [(k, n) for k, n, _ in got] == [(1, 3), (2, 3), (3, 3)], got
    assert all(ref for _, _, ref in got), "every fetched photo carries its content hash"
    assert "They wrote: All of this for dinner" in rows[0]["text"]

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
def test_l48_album_and_text_is_one_turn(device, net, owner_chat):
    items = _stage(device, 3, 90000, seed=4811)
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
def test_l48_eleven_photos_are_two_turns(device, net, owner_chat):
    items = _stage(device, 11, 40000, seed=4821)
    uid = tb.first_update_id() + 4820
    page = album_page(uid, owner_chat, items[:10], group="L48C")
    extra = json.loads(page)
    extra["result"].append(tb.local_photo_update(uid + 10, owner_chat, items[10][0], items[10][1]))
    lines = _inject(device, json.dumps(extra))
    assert "10 messages batched into 1 turn" in lines[-1] and "resumes next cycle" in lines[-1], lines[-1]
    second = _lines_until(device, INJECT_DONE, 600)
    assert "1 message batched into 1 turn" in second[-1], second[-1]
    tens = _wait(lambda: album_turn_rows(_episodic(net), 10), 120)
    ones = album_turn_rows(_episodic(net), 1)
    assert len(tens) == 1 and len(labels_in(tens[0]["text"])) == 10
    assert any(items[10][0] not in r["text"] for r in ones)  # the 11th ran as its own turn


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
def test_l48_ten_photos_against_one(device, net, owner_chat):
    one = _stage(device, 1, 120000, seed=4831)
    ten = _stage(device, 10, 120000, seed=4841)
    runs: Dict[str, List[Dict[str, int]]] = {"one": [], "ten": []}
    for r in range(3):
        for name, items in (("one", one), ("ten", ten)):
            uid = tb.first_update_id() + 4900 + 20 * r + (0 if name == "one" else 10)
            traces = head_traces(_inject(device, album_page(uid, owner_chat, items, group=f"L48D{r}{name}")))
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
def test_l48_guard_is_fresh_after_a_flash(device):
    """Run first after flashing a new image: the guard record from the previous image is
    rebuilt (its build id differs), so the first boot counts nothing."""
    device.reboot_and_confirm()
    line = device.expect("telegram: storm guard", timeout=60.0)
    g = _guard(device)
    print(line, g)
    if "kept" in line:
        pytest.skip("not the first boot of this image: run this leg right after the flash")
    assert "fresh" in line and g["dups"] == 0 and g["tries"] == 0 and g["poison"] == 0, (line, g)


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
    assert guard_counts("TGGUARD dups=49 ceiling=0 poison=0 tries=0 holding=0") == {
        "dups": 49,
        "ceiling": 0,
        "poison": 0,
        "tries": 0,
        "holding": 0,
    }
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
