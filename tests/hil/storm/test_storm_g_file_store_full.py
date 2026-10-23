"""Storm leg g: the file store at its limit.

Reproduces 19 Oct 2026 (L19-6, L19-35): the store reached its 256-file limit (136 copies
of one replayed photo, 17 of another item) and every new file the owner sent was
refused without a word.

Drive: the store filled to its limit with small uploads (three bench projects), then
five owner photos (the candidate: ``TGINJECT`` pages whose photo is a card file staged
before the fill; v4.5.13 has no raw page inject, so it gets five owner messages asking
it to keep a note, through the test inject route and the LAN mock), then one fresh
message. Every bench project, and the files the photos left in the owner chat's
project, are removed afterwards. Every other provider key is cleared. Unit L.

Bounds on the release candidate: at most one turn per photo, each photo read once
(no repeated download), the owner told that the store is full, and every turn reaching
the owner as exactly one message (a photo answered twice, or a notice repeated on its
own, is a second message), no log line repeated more than once per photo plus once (in
bot mode each message to the owner logs the same send line), the fresh message answered
within 180 s.

Run: NIMBUS_STORM_UNIT=L python3 -m pytest tests/hil/storm/test_storm_g_file_store_full.py --allow-hardware -s
"""

from __future__ import annotations

import re
import time

import pytest
import requests

import grading as g
import kit

pytestmark = [pytest.mark.hil, pytest.mark.net]

LEG = "g_file_store_full"
LIMIT = 256  # the file store's entry limit (nimbus::orch::FileStore::Limits::maxEntries)
PHOTOS = 5
FILL_PROJECTS = ("stormg1", "stormg2", "stormg3")
STAGE_PROJECT = "stormgp"
NOTICE_RE = re.compile(
    r"full|storage|space|remove|delete|could not be (?:saved|stored|kept)|not (?:saved|stored)", re.I
)
DOWNLOAD_RE = re.compile(r"telegram: attachment downloaded")
# In bot mode every message to the owner logs the same "telegram: send ok to <chat>" line:
# one reply per photo plus the episode's one notice that the store is full. More than
# that is a send loop, still caught here (and by messagesPerTurn and the notice count).
MAX_SENDS = PHOTOS + 1
BOUNDS = g.Bounds(
    max_paid_turns=PHOTOS,
    max_provider_requests=3 * PHOTOS,
    max_identical=MAX_SENDS,
    notices=(1, 1),
    usable_within_s=180.0,
)


def _upload(storm, project: str, name: str, data: bytes, mime: str = "text/plain") -> requests.Response:
    url = f"http://{storm.net.ip}/api/files/upload?project={project}&name={name}"
    return requests.post(url, files={"file": (name, data, mime)}, headers=storm.net._auth(), timeout=30)


def _count(storm) -> int:
    return int((storm.state().get("files") or {}).get("count", -1))


def _fill_store(storm) -> dict:
    """Small uploads round-robin over the bench projects until the store refuses one."""
    start = _count(storm)
    refused, k = "", 0
    while k < LIMIT + 8:
        r = _upload(storm, FILL_PROJECTS[k % len(FILL_PROJECTS)], f"f{k:03d}.txt", b"storm g filler\n")
        k += 1
        if r.status_code >= 400:
            refused = f"HTTP {r.status_code} {r.text[:120]}"
            break
    return {"before": start, "after": _count(storm), "uploads": k, "refused": refused}


def _cleanup(storm) -> None:
    for project in (*FILL_PROJECTS, STAGE_PROJECT, f"chat-{storm.chat}"):
        try:
            r = storm.net.post(f"/api/files/rmproject?project={project}&confirm=DELETE", {}, timeout=30.0)
            print(f"[storm] removed {project}: HTTP {r.status_code}")
        except Exception as e:  # noqa: BLE001 - report, keep cleaning
            print(f"[storm] removing {project} failed: {e}")


def _photo_page(chat: int, uid: int, path: str, size: int) -> str:
    import tg_archive_bench as tb

    u = tb.local_photo_update(uid, chat, path, size)
    u["message"]["caption"] = f"storm g {uid}: keep this photo"
    u["message"]["photo"] = u["message"]["photo"][-1:]
    return tb.page([u])


def _photos(storm, uid: int, staged: tuple) -> None:
    for k in range(PHOTOS):
        if storm.control:
            kit.owner_turn(storm, f"storm g {uid + k}: keep this note in a file: bench note {k}", uid + k)
            continue
        page = _photo_page(storm.chat, uid + k, *staged)
        line = ""
        for _ in range(60):
            line = storm.cmd_paced("TGINJECT " + page, "TGINJECT", timeout=15.0)
            if "busy" not in line:
                break
            time.sleep(3.0)
        assert "queued" in line, f"the photo page was not taken: {line}"
        end = time.time() + 300
        while time.time() < end:
            if kit.INJECT_DONE in storm.device.expect_re(r".", timeout=max(1.0, end - time.time())).string:
                break
        storm.tap.poll()


def told_once_per_turn(w) -> int:
    """0 when the owner was never told the store is full; otherwise the most messages any
    one turn of the window sent (exactly 1 is told once: a reply per photo is an answer,
    a second message from the same turn is a repeat)."""
    if not w.notices(NOTICE_RE):
        return 0
    per_turn = g.rows_per_turn(w.rows)
    return max(per_turn.values()) if per_turn else 1


def test_storm_g_file_store_full(storm):
    import tg_archive_bench as tb

    uid = kit.first_update_id()
    with storm.session(kit.keep_for(storm), kit.head_for(storm)):
        try:
            staged = None
            if not storm.control:
                data = tb.noise_jpeg(320, 240, seed=7071)
                r = _upload(storm, STAGE_PROJECT, "g.jpg", data, "image/jpeg")
                assert r.status_code < 400, f"staging the photo failed: HTTP {r.status_code}"
                staged = (f"/mem/files/{STAGE_PROJECT}/g.jpg", len(data))
            fill = _fill_store(storm)
            print(f"[storm] file store: {fill}")
            w = kit.Window(storm).open()
            _photos(storm, uid, staged)
            storm.wait_quiet(quiet_s=20.0, timeout=600.0)
            w.close()
            probe = storm.fresh_message(
                kit.ANSWER_RE, f"storm g probe {uid}: reply with the single word pong", uid + 50
            )
        finally:
            _cleanup(storm)
    replies = w.replies(NOTICE_RE)
    downloads = [ln.text for ln in w.lines if DOWNLOAD_RE.search(ln.text)]
    peak, line = g.identical_peak(w.lines, ignore=kit.DRIVE_LINES)
    m = g.Metrics(
        paid_turns=g.paid_turns(w.lines),
        provider_requests=w.requests(),
        parked_requests=0,
        notices=told_once_per_turn(w),
        identical_peak=max(peak, len(downloads) - PHOTOS + 1 if downloads else 0),
        identical_line=line[:160],
        usable=probe["usable"],
        usable_s=probe["usable_s"],
    )
    m.extra.update(
        {
            "fill": fill,
            "downloads": len(downloads),
            "replies": replies,
            "messagesPerTurn": g.rows_per_turn(w.rows),
            "probeReply": probe["reply"][:200],
        }
    )
    if storm.control:
        m.notes.append(
            "v4.5.13 has no raw page inject, so its five owner messages are text asking for a note to be kept; "
            "the photo path itself is the field record (136 copies of one replayed photo, new files refused silently)"
        )
    kit.assert_graded(storm.finish(LEG, m, BOUNDS))
