"""§L26 - deep history: reach rows the boot scan never indexed (v4.0.0).

The gap this covers, verified on hardware: the episodic boot scan is bounded
(kHydrateMaxRows 4000 / 256 KB), so after a restart the older rows are ON the
card but outside the index - invisible to a plain query. A month of chat was
mostly unreachable and nothing said so.

What runs here (deterministic, no LLM, no cost):
  A. fill ~4400 rows @256 B into ONE day-file (deliberately one file: a row
     cursor alone cannot page inside a file - that is why the cold cursor is
     byte-resolution), with a unique needle planted FIRST (oldest);
  B. restart, and assert on real hardware that the boot scan truncated;
  C. RED/GREEN: cold=0 must miss the needle, cold=1 must find it - the contrast
     IS the test (a green that does not go red when cold scanning is off would
     be proving nothing);
  D. cursor-follow: page the model-facing tool with `before` until the needle
     appears, bounded, asserting the cursor advances every page;
  E. no watchdog: the fill + the deep queries must not reboot the device;
  F. the index converges after the restart (v4.5.14): the boot scan indexes a
     budget-bounded window, then the background backfill indexes the rest of the
     card; once /api/state reports epiIndexComplete, the total is at least the
     pre-restart total and a plain (cold=0) query finds the oldest needle.

Since v4.5.14 the backfill usually completes within seconds of the restart, so
legs B-D find the index already complete; they then skip with a coverage-lost
note (the cold path stays covered by the host suite and by a store past its
index cap). Leg F is the hardware oracle for the convergence.

Residue: the rows live in session "hiltest-epi" (the standard fill session) and
are left in place - they are what makes a re-run meaningful. Markers: net (a web/LAN
leg reached over HTTP, no serial console, so NOT hil - the serial launchd tier has
no LAN route and would only error on it; bench rerun 2 item 4). Run it DIRECTLY.
Needs --allow-hardware, an Orchestrator board WITH SD, NIMBUS_TEST_IP/TOKEN; the
board fixture skips LOUDLY if the host is set but unreachable from this process.
"""

from __future__ import annotations

import json
import time

import pytest
from webrig import ip_tok_or_skip, make_session, require_orchestrator

try:
    import requests
except ImportError:  # pragma: no cover
    requests = None

pytestmark = [pytest.mark.net]

NEEDLE = f"bilge pump serial is BP-{int(time.time()) % 100000}"
# The BYTE budget binds first: kHydrateMaxBytes is 256 KB and a row here is
# 256 B, so ~1000 rows already exceed what the boot scan can index. 2000 leaves
# margin without spending ten minutes writing to the card.
FILL_ROWS = 2000
FILL_CHUNK = 200  # the seam's per-call cap
FILL_BYTES = 256


class _Board:
    """The board under test: its address and a session that carries the web token
    as the X-Nimbus-Token header (the device rejects a ?t= query token). The token
    never appears in a URL, a repr or an assertion message."""

    def __init__(self, ip, session):
        self.ip = ip
        self.s = session

    def url(self, path):
        return f"http://{self.ip}{path}"

    def __repr__(self):
        return f"<board {self.ip}>"


def _state(b):
    return b.s.get(b.url("/api/state"), timeout=10).json()


def _stats(b):
    return b.s.get(b.url("/api/mem/stats"), timeout=15).json()


def _fill(b, n, text=None):
    body = {"kind": "epi", "n": str(n), "bytes": str(FILL_BYTES)}
    if text:
        body["text"] = text
    r = b.s.post(b.url("/api/test/memfill"), data=body, timeout=60)
    if r.status_code == 404:
        pytest.skip("/api/test/memfill absent - flash [env:test]")
    assert r.status_code == 200, f"memfill -> {r.status_code}: {r.text[:160]}"
    return int(r.json()["added"])


def _astool(b, tool, args, chat="web"):
    body = json.dumps({"jsonrpc": "2.0", "id": 1, "method": "tools/call", "params": {"name": tool, "arguments": args}})
    r = b.s.post(b.url("/api/test/astool"), data={"chat": chat, "body": body}, timeout=30)
    if r.status_code == 404:
        pytest.skip("/api/test/astool absent - flash [env:test]")
    assert r.status_code == 200, f"{tool}: {r.status_code} {r.text[:200]}"
    return r.json()["result"]["content"][0]["text"]


def _epi(b, **params):
    q = "&".join(f"{k}={requests.utils.quote(str(v))}" for k, v in params.items())
    r = b.s.get(b.url(f"/api/mem/episodic?{q}"), timeout=30)
    assert r.status_code == 200, f"/api/mem/episodic -> {r.status_code}"
    return r.json()


def _reboot_and_wait(b, timeout=120):
    try:
        b.s.post(b.url("/api/test/reboot"), timeout=5)
    except requests.RequestException:
        pass  # the device drops the socket as it restarts
    time.sleep(12)
    deadline = time.time() + timeout
    while time.time() < deadline:
        try:
            if _state(b).get("mode") == 1:
                return True
        except requests.RequestException:
            pass
        time.sleep(5)
    return False


@pytest.fixture(scope="module")
def board():
    ip, tok = ip_tok_or_skip("L26")
    session = make_session(tok)
    try:
        session.get(f"http://{ip}/api/state", timeout=10)
    except requests.RequestException as exc:
        pytest.skip(
            f"NIMBUS_TEST_IP is set but the board is unreachable over HTTP from this "
            f"process ({exc}). L26 is a web/LAN leg: run it DIRECTLY (not under the "
            f"serial launchd job, which has no LAN route on this Mac). See PR_BODY runbook."
        )
    # A rejected token fails loudly here; only a healthy board outside
    # Orchestrator mode skips (webrig: a 401 must never read as a skip).
    st = require_orchestrator(session, ip, "L26")
    if not st.get("memSd"):
        pytest.skip("no SD - deep history IS the SD day-streams (warn: coverage lost)")
    return _Board(ip, session)


@pytest.fixture(scope="module")
def filled(board):
    """Plant the needle, bury it under >4000 rows, restart. Skips loudly if the
    fill cannot reach the truncation threshold - a passing test on a short
    history would prove nothing."""
    b = board
    _fill(b, 1, text=NEEDLE)  # OLDEST row: what a deep query must find
    planted = 1
    while planted < FILL_ROWS:
        planted += _fill(b, min(FILL_CHUNK, FILL_ROWS - planted))
        time.sleep(2)  # SD append + watchdog headroom
    pre_total = int(_epi(b, limit=1).get("total", 0))
    assert _reboot_and_wait(b), "device did not come back after the fill"
    return b, planted, pre_total


def _index_complete(b):
    return bool(_state(b).get("epiIndexComplete"))


def test_boot_scan_truncates_on_a_real_card(filled):
    b, planted, _ = filled
    st = _stats(b)
    if "epiBootIndexed" in st:
        # v4.5.14+: the backfill may already have completed the index, so the
        # boot scan's own share is the evidence that it stopped at its budget.
        booted, total = int(st["epiBootIndexed"]), int(st.get("episodicMsgs", 0))
        if booted >= total:
            pytest.skip(
                f"boot scan indexed everything ({planted} rows planted, {total} rows) - "
                f"history too short to exercise the budget (warn: coverage lost)"
            )
        assert booted < total, "the boot scan should index only a budget-bounded window"
        return
    if not st.get("epiTruncated"):
        pytest.skip(
            f"boot scan indexed everything ({planted} rows planted, "
            f"{st.get('episodicMsgs')} indexed) - history too short to "
            f"exercise the cold path (warn: coverage lost)"
        )
    assert st.get("epiFloor"), "truncated scan must report the day it reached"


def test_index_converges_after_restart(filled):
    """F: after the restart the index converges to the whole card, in the
    background, without a turn - and the oldest row is then reachable by a
    plain query. Deterministic, no LLM."""
    b, _, pre_total = filled
    if "epiIndexComplete" not in _state(b):
        pytest.skip("firmware predates the index backfill (no epiIndexComplete in /api/state)")
    deadline = time.time() + 180
    while time.time() < deadline and not _index_complete(b):
        time.sleep(1)
    assert _index_complete(b), "the index did not complete within 180 s of the restart"
    page = _epi(b, limit=1)
    assert page.get("indexComplete") is True, "/api/mem/episodic must agree with /api/state"
    assert int(page.get("total", 0)) >= pre_total, (
        f"index total {page.get('total')} is below the pre-restart total {pre_total}"
    )
    page_floor = _stats(b).get("epiFloor", "")
    # A plain query reaches it through the index alone. One call is bounded (the
    # needle sits about 2,000 rows deep), so it pages with the cursor the device
    # hands back, like the model does; every page must be honest about how far
    # back it looked and must not need the cold pass.
    before, found, pages = "", False, 0
    while pages < 20 and not found:
        pages += 1
        hot = _epi(b, text="bilge pump", limit=50, cold=0, before=before)
        found = any(NEEDLE in m.get("text", "") for m in hot.get("messages", []))
        assert int(hot.get("coldFiles", 0)) == 0, f"page {pages} fell to the cold pass on a complete index"
        nxt = hot.get("nextBefore") or ""
        if not found and nxt:
            assert hot.get("olderExists") is True, f"page {pages} hands back a cursor but denies older history"
            assert hot.get("searchedTo", "") >= page_floor, (
                f"page {pages} claims it searched below the index floor ({hot.get('searchedTo')} < {page_floor})"
            )
        assert nxt != before or not nxt, f"cursor did not advance on page {pages}"
        if not nxt:
            break
        before = nxt
    assert found, f"the oldest needle is on the card but {pages} plain pages did not reach it after the index completed"
    st = _stats(b)
    assert st.get("epiIndexComplete") is True
    assert int(st.get("epiBootIndexed", 0)) + int(st.get("epiBackfillRows", 0)) <= int(st.get("episodicMsgs", 0))


def test_cold_scan_is_the_difference_between_miss_and_hit(filled):
    b, _, _ = filled
    if "epiIndexComplete" in _state(b) and _index_complete(b):
        pytest.skip("the backfill already completed the index - no unindexed rows to contrast (warn: coverage lost)")
    if not _stats(b).get("epiTruncated"):
        pytest.skip("boot scan did not truncate (warn: coverage lost)")

    # RED - the indexed range alone cannot see it.
    hot = _epi(b, text="bilge pump", limit=20, cold=0)
    if hot.get("indexComplete") is True:
        pytest.skip("the backfill completed the index during the check (warn: coverage lost)")
    assert not any(NEEDLE in m.get("text", "") for m in hot.get("messages", [])), (
        "the needle was in the INDEX - the fill did not bury it deep enough"
    )
    assert hot.get("olderExists") is True, "a truncated store must admit older history"

    # GREEN - opted in, paging as far as the budget allows.
    before, found, pages = "", False, 0
    while pages < 40 and not found:
        pages += 1
        page = _epi(b, text="bilge pump", limit=20, cold=1, before=before)
        found = any(NEEDLE in m.get("text", "") for m in page.get("messages", []))
        nxt = page.get("nextBefore") or ""
        assert nxt != before or not nxt, f"cursor did not advance on page {pages}"
        assert page.get("searchedTo"), "every page reports how far back it looked"
        if not nxt:
            break
        before = nxt
    assert found, f"cold scan never reached the needle in {pages} pages"


def test_model_facing_tool_pages_to_the_needle(filled):
    b, _, _ = filled
    st = _stats(b)
    if not st.get("epiTruncated") and not st.get("epiBackfillRows"):
        pytest.skip("boot scan did not truncate (warn: coverage lost)")
    before, found, pages = "", False, 0
    while pages < 40 and not found:
        pages += 1
        args = {"text": "bilge pump", "limit": 20}
        if before:
            args["before"] = before
        out = _astool(b, "memory.episodic", args)
        found = NEEDLE in out
        # The tool must hand the model the exact token to continue - it never
        # computes a cursor itself.
        tok_marker = 'before="'
        i = out.find(tok_marker)
        nxt = out[i + len(tok_marker) : out.find('"', i + len(tok_marker))] if i >= 0 else ""
        if not nxt:
            break
        assert nxt != before, f"tool cursor did not advance on page {pages}"
        before = nxt
    assert found, f"the tool never reached the needle in {pages} pages"


def test_deep_queries_did_not_reboot_the_device(filled):
    b, _, _ = filled

    # Every boot writes one row into the "system" session. The fixture's own
    # restart is the last one there may be: another arriving after these queries
    # ran means the deep read tripped the watchdog. Counting rows is the oracle
    # (/api/state carries no uptime field - an earlier version asserted on one
    # that never existed, which passes for the wrong reason on a healthy board).
    def boot_rows():
        rows = _epi(b, session="system", limit=50).get("messages", [])
        return [m["id"] for m in rows if "Booted" in m.get("text", "")]

    before_ids = boot_rows()
    for _ in range(3):  # a few more deep reads, deliberately
        _epi(b, text="bilge pump", limit=20, cold=1)
    assert _state(b).get("mode") == 1, "device not serving after the deep queries"
    assert boot_rows() == before_ids, "the device restarted during a deep query"
