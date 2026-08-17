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
  E. no watchdog: the fill + the deep queries must not reboot the device.

Residue: the rows live in session "hiltest-epi" (the standard fill session) and
are left in place - they are what makes a re-run meaningful. Markers: hil + net;
needs --allow-hardware, an Orchestrator board WITH SD, NIMBUS_TEST_IP/TOKEN.
"""

from __future__ import annotations

import json
import os
import time

import pytest

try:
    import requests
except ImportError:  # pragma: no cover
    requests = None

pytestmark = [pytest.mark.hil, pytest.mark.net]

NEEDLE = f"bilge pump serial is BP-{int(time.time()) % 100000}"
# The BYTE budget binds first: kHydrateMaxBytes is 256 KB and a row here is
# 256 B, so ~1000 rows already exceed what the boot scan can index. 2000 leaves
# margin without spending ten minutes writing to the card.
FILL_ROWS = 2000
FILL_CHUNK = 200  # the seam's per-call cap
FILL_BYTES = 256


def _u(ip, tok, path):
    sep = "&" if "?" in path else "?"
    return f"http://{ip}{path}{sep}t={tok}"


def _state(ip, tok):
    return requests.get(_u(ip, tok, "/api/state"), timeout=10).json()


def _stats(ip, tok):
    return requests.get(_u(ip, tok, "/api/mem/stats"), timeout=15).json()


def _fill(ip, tok, n, text=None):
    body = {"kind": "epi", "n": str(n), "bytes": str(FILL_BYTES)}
    if text:
        body["text"] = text
    r = requests.post(_u(ip, tok, "/api/test/memfill"), data=body, timeout=60)
    if r.status_code == 404:
        pytest.skip("/api/test/memfill absent - flash [env:test]")
    assert r.status_code == 200, f"memfill -> {r.status_code}: {r.text[:160]}"
    return int(r.json()["added"])


def _astool(ip, tok, tool, args, chat="web"):
    body = json.dumps({"jsonrpc": "2.0", "id": 1, "method": "tools/call", "params": {"name": tool, "arguments": args}})
    r = requests.post(_u(ip, tok, "/api/test/astool"), data={"chat": chat, "body": body}, timeout=30)
    if r.status_code == 404:
        pytest.skip("/api/test/astool absent - flash [env:test]")
    assert r.status_code == 200, f"{tool}: {r.status_code} {r.text[:200]}"
    return r.json()["result"]["content"][0]["text"]


def _epi(ip, tok, **params):
    q = "&".join(f"{k}={requests.utils.quote(str(v))}" for k, v in params.items())
    r = requests.get(_u(ip, tok, f"/api/mem/episodic?{q}"), timeout=30)
    assert r.status_code == 200, f"/api/mem/episodic -> {r.status_code}"
    return r.json()


def _reboot_and_wait(ip, tok, timeout=120):
    try:
        requests.post(_u(ip, tok, "/api/test/reboot"), timeout=5)
    except requests.RequestException:
        pass  # the device drops the socket as it restarts
    time.sleep(12)
    deadline = time.time() + timeout
    while time.time() < deadline:
        try:
            if _state(ip, tok).get("mode") == 1:
                return True
        except requests.RequestException:
            pass
        time.sleep(5)
    return False


@pytest.fixture(scope="module")
def board():
    if requests is None:
        pytest.skip("requests not installed")
    ip = os.environ.get("NIMBUS_TEST_IP")
    tok = os.environ.get("NIMBUS_TEST_TOKEN")
    if not ip or not tok:
        pytest.skip("set NIMBUS_TEST_IP + NIMBUS_TEST_TOKEN")
    st = _state(ip, tok)
    if st.get("mode") != 1:
        pytest.skip("not in Orchestrator mode")
    if not st.get("memSd"):
        pytest.skip("no SD - deep history IS the SD day-streams (warn: coverage lost)")
    return ip, tok


@pytest.fixture(scope="module")
def filled(board):
    """Plant the needle, bury it under >4000 rows, restart. Skips loudly if the
    fill cannot reach the truncation threshold - a passing test on a short
    history would prove nothing."""
    ip, tok = board
    _fill(ip, tok, 1, text=NEEDLE)  # OLDEST row: what a deep query must find
    planted = 1
    while planted < FILL_ROWS:
        planted += _fill(ip, tok, min(FILL_CHUNK, FILL_ROWS - planted))
        time.sleep(2)  # SD append + watchdog headroom
    assert _reboot_and_wait(ip, tok), "device did not come back after the fill"
    return ip, tok, planted


def test_boot_scan_truncates_on_a_real_card(filled):
    ip, tok, planted = filled
    st = _stats(ip, tok)
    if not st.get("epiTruncated"):
        pytest.skip(
            f"boot scan indexed everything ({planted} rows planted, "
            f"{st.get('episodicMsgs')} indexed) - history too short to "
            f"exercise the cold path (warn: coverage lost)"
        )
    assert st.get("epiFloor"), "truncated scan must report the day it reached"


def test_cold_scan_is_the_difference_between_miss_and_hit(filled):
    ip, tok, _ = filled
    if not _stats(ip, tok).get("epiTruncated"):
        pytest.skip("boot scan did not truncate (warn: coverage lost)")

    # RED - the indexed range alone cannot see it.
    hot = _epi(ip, tok, text="bilge pump", limit=20, cold=0)
    assert not any(NEEDLE in m.get("text", "") for m in hot.get("messages", [])), (
        "the needle was in the INDEX - the fill did not bury it deep enough"
    )
    assert hot.get("olderExists") is True, "a truncated store must admit older history"

    # GREEN - opted in, paging as far as the budget allows.
    before, found, pages = "", False, 0
    while pages < 40 and not found:
        pages += 1
        page = _epi(ip, tok, text="bilge pump", limit=20, cold=1, before=before)
        found = any(NEEDLE in m.get("text", "") for m in page.get("messages", []))
        nxt = page.get("nextBefore") or ""
        assert nxt != before or not nxt, f"cursor did not advance on page {pages}"
        assert page.get("searchedTo"), "every page reports how far back it looked"
        if not nxt:
            break
        before = nxt
    assert found, f"cold scan never reached the needle in {pages} pages"


def test_model_facing_tool_pages_to_the_needle(filled):
    ip, tok, _ = filled
    if not _stats(ip, tok).get("epiTruncated"):
        pytest.skip("boot scan did not truncate (warn: coverage lost)")
    before, found, pages = "", False, 0
    while pages < 40 and not found:
        pages += 1
        args = {"text": "bilge pump", "limit": 20}
        if before:
            args["before"] = before
        out = _astool(ip, tok, "memory.episodic", args)
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
    ip, tok, _ = filled

    # Every boot writes one row into the "system" session. The fixture's own
    # restart is the last one there may be: another arriving after these queries
    # ran means the deep read tripped the watchdog. Counting rows is the oracle
    # (/api/state carries no uptime field - an earlier version asserted on one
    # that never existed, which passes for the wrong reason on a healthy board).
    def boot_rows():
        rows = _epi(ip, tok, session="system", limit=50).get("messages", [])
        return [m["id"] for m in rows if "Booted" in m.get("text", "")]

    before_ids = boot_rows()
    for _ in range(3):  # a few more deep reads, deliberately
        _epi(ip, tok, text="bilge pump", limit=20, cold=1)
    assert _state(ip, tok).get("mode") == 1, "device not serving after the deep queries"
    assert boot_rows() == before_ids, "the device restarted during a deep query"
