"""Shared web-tier auth rig for the L15-L20 LAN suites (CUM-418 item 1).

Since CUM-45 the device rejects the web token passed as a ``?t=`` query string
(``src/net/webui.cpp`` ``webAuthOk``): only the ``X-Nimbus-Token`` HEADER (or a
form field) is accepted. The L15-L20 tiers historically built ``?t=`` URLs, so
every request 401'd, and each tier's rig read that 401 as "not Orchestrator mode"
and skipped the WHOLE tier green - the tier reported green-by-skip in every run
since CUM-45 (verified live: ``?t=`` -> 401, header -> 200 on the same board and
token).

This module is the one place the auth contract lives (the same one
``tests/hil/net.py`` already uses): a shared ``requests.Session`` that carries the
token as the header on every request, and a gate that tells the three outcomes
APART instead of collapsing them to one skip:

  * env not set / board unreachable  -> skip (nothing to run against)
  * 401                              -> FAIL loudly (the token was rejected - the
                                        exact regression that skipped the tier green)
  * 200 but mode != 1                -> skip "Orchestrator mode required (MODE 1)"

Nothing here touches the network at import time, so ``pytest --collect-only`` stays
clean on a device-less box.
"""

from __future__ import annotations

import os
import threading
import time

import pytest

try:
    import requests
except ImportError:  # pragma: no cover - collection must still succeed
    requests = None


def make_session(tok: str):
    """A requests.Session that authenticates every request with the device web
    token as the ``X-Nimbus-Token`` header (never a ``?t=`` query, which the device
    rejects since CUM-45). One session per tier keeps the connection warm and puts
    the auth contract in exactly one place."""
    s = requests.Session()
    s.headers["X-Nimbus-Token"] = tok
    return s


def ip_tok_or_skip(label: str):
    """(ip, tok) from NIMBUS_TEST_IP + NIMBUS_TEST_TOKEN, or a loud skip."""
    if requests is None:
        pytest.skip("requests not installed")
    ip = os.environ.get("NIMBUS_TEST_IP")
    tok = os.environ.get("NIMBUS_TEST_TOKEN")
    if not ip or not tok:
        pytest.skip(f"set NIMBUS_TEST_IP + NIMBUS_TEST_TOKEN (WEBTOK?) to run {label}")
    return ip, tok


def require_orchestrator(session, ip: str, label: str) -> dict:
    """GET /api/state through the header-authed ``session`` and gate the tier,
    telling a REJECTED token (401 -> loud fail) apart from a healthy board that is
    simply not in Orchestrator mode (loud skip). Returns the parsed /api/state.

    A 401 here is never a skip: reading it as "not MODE 1" is the precise bug that
    skipped these tiers green from CUM-45 until CUM-418."""
    try:
        r = session.get(f"http://{ip}/api/state", timeout=10)
    except requests.RequestException as exc:  # configured but unreachable
        pytest.fail(f"{label}: NIMBUS_TEST_IP is set but /api/state is unreachable: {exc}")
    if r.status_code == 401:
        pytest.fail(
            f"{label}: /api/state returned 401 with the X-Nimbus-Token header - the "
            "web token was rejected. Refresh NIMBUS_TEST_TOKEN from WEBTOK? on the "
            "target board. A 401 must NOT be read as 'not Orchestrator mode' and "
            "skipped (that skipped this whole tier green from CUM-45 until CUM-418)."
        )
    if r.status_code != 200:
        pytest.fail(f"{label}: /api/state -> {r.status_code} (expected 200): {r.text[:160]}")
    st = r.json()
    if st.get("mode") != 1:
        pytest.skip("Orchestrator mode required (MODE 1)")
    return st


def chat_turn(session, ip: str, text: str, timeout_s: float = 150.0, poll_s: float = 2.0) -> str:
    """Send one web-chat message and return THAT turn's reply.

    Reads ``GET /api/chat?turn=<id>`` (CUM-482), never the legacy single slot: the
    slot still holds the previous reply while the new message waits, so a reader of
    it lines up one reply behind (the v4.5.14 release run read "OK" from an earlier
    file as a /compact reply, and turn 1 as turn 2). Fails on a turn the device
    forgot (``gone``) and on no answer within ``timeout_s``."""
    r = session.post(f"http://{ip}/api/chat", data={"text": text}, timeout=10)
    if r.status_code == 503:  # the previous reply is still finishing
        time.sleep(3)
        r = session.post(f"http://{ip}/api/chat", data={"text": text}, timeout=10)
    assert r.status_code == 200, f"/api/chat -> {r.status_code}: {r.text[:200]}"
    turn = str(r.json().get("turn") or "")
    assert turn, f"/api/chat gave no turn id: {r.text[:200]}"
    deadline = time.time() + timeout_s
    while time.time() < deadline:
        time.sleep(poll_s)
        j = session.get(f"http://{ip}/api/chat", params={"turn": turn}, timeout=10).json()
        assert not j.get("gone"), f"turn {turn} is gone before it was answered: {text[:60]}"
        if j.get("done"):
            return str(j.get("reply") or "")
    pytest.fail(f"turn {turn} was not answered within {timeout_s:.0f}s: {text[:60]}")


def stitch(acc: str, snap: str, overlap: int = 160):
    """Append the new part of one /api/log snapshot to ``acc``; return (acc, gap).

    The RAM log is a 1,280-byte ring that wraps mid-line, so each read is the last
    1,280 bytes. The tail of what was already read is found in the new snapshot and
    only what follows it is appended. When it is not found the ring turned over
    between two reads: the snapshot is appended after a gap mark and ``gap`` is True."""
    if not acc:
        return snap, False
    key = acc[-overlap:]
    at = snap.rfind(key)
    if at >= 0:
        return acc + snap[at + len(key) :], False
    return acc + "\n[log gap]\n" + snap, True


class LogFollower:
    """Follow /api/log while a turn runs and stitch it into one text.

    One read after a turn sees only its last 1,280 bytes, so the turn's first lines
    are gone; this polls the ring every ``period_s`` in a thread and stitches the
    reads. ``gaps`` counts reads the ring had already turned past (lines lost), so a
    leg that asserts a line is ABSENT can refuse to judge a text with holes in it."""

    def __init__(self, session, ip: str, period_s: float = 0.4):
        self._s, self._ip, self._period = session, ip, period_s
        self._acc, self._base, self.gaps = "", 0, 0
        self._stop = threading.Event()
        self._t = threading.Thread(target=self._run, daemon=True)

    @property
    def text(self) -> str:
        """What the log gained after the follower started (never the lines before it)."""
        return self._acc[self._base :]

    def _read(self) -> None:
        try:
            snap = self._s.get(f"http://{self._ip}/api/log", timeout=8).text
        except Exception:  # noqa: BLE001 - a missed read shows up as a gap, never a crash
            return
        self._acc, gap = stitch(self._acc, snap)
        self.gaps += int(gap)

    def _run(self) -> None:
        while not self._stop.is_set():
            self._read()
            self._stop.wait(self._period)

    def __enter__(self) -> "LogFollower":
        self._read()
        self._base = len(self._acc)
        self._t.start()
        return self

    def __exit__(self, *exc) -> None:
        self._stop.set()
        self._t.join(timeout=10)
        time.sleep(1.0)
        self._read()   # the turn's last lines
