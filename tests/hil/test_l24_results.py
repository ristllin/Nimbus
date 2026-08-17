"""§L24 - the recent-results ring (spill → results.get → namespace scoping) E2E.

The Context Fabric added a PSRAM results ring so a truncated tool output or an
overflowed sub-agent result is no longer LOST - it spills under a tag and the
model (or a test) fetches the full text back with results.get. This suite proves,
on real hardware:

  A. surface wired - results.list / results.get reachable over /mcp, and a miss
     points the caller at memory.episodic (the actionable dead-end).
  B. marker round-trip - a seeded result comes back BYTE-COMPLETE through
     results.get, including a unique marker (the standing E2E bar: MARKER in,
     MARKER out, not a prose echo).
  C. namespace scoping - the prism 2026-08-05 CRITICAL: the ring holds full tool
     outputs from every turn, so a read MUST be scoped. A foreign chat fetching
     another tenant's tag gets a MISS, not the bytes. Mutation-checked: point the
     seam's ns at the reader and this test goes green for the wrong reason, so the
     assertion is paired with a same-ns POSITIVE fetch that must still pass.
  D. live spill (opt-in, paid) - a real deep fan-out turn overflows the 6-slot
     fresh-results ring; results.list must then show a sub: entry. Skips LOUDLY if
     the model under-spawns (no overflow) rather than passing vacuously.

Parts A–C are deterministic (they use the TEST-only /api/test/resultput +
/api/test/astool seams - no LLM, no cost) and carry the ``net`` marker. Part D is
``agent`` (deselected by default like the other paid suites).

Markers: ``hil`` + ``net`` (+ ``agent`` on Part D); needs ``--allow-hardware``, a
LAN-reachable Orchestrator-mode board, and the token (NIMBUS_TEST_TOKEN / WEBTOK?).
The seams are compiled only in ``[env:test]`` - a production board 404s them, and
the test skips LOUDLY on 404 so a mis-flashed board can't read as a pass.
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

# Two distinct NON-owner chats -> two distinct namespaces (nsForChat(id, admin=False)).
# Neither is the owner, so neither has readAll - the scoping check is real.
CHAT_A = "924101"
CHAT_B = "924102"
MARKER = f"SPILL-{int(time.time()) % 100000}"  # unique per run; reruns can't alias


def _u(ip, tok, path):
    sep = "&" if "?" in path else "?"
    return f"http://{ip}{path}{sep}t={tok}"


def _resultput(ip, tok, chat, text, kind="tool", name="seed"):
    r = requests.post(
        _u(ip, tok, "/api/test/resultput"), data={"chat": chat, "text": text, "kind": kind, "name": name}, timeout=15
    )
    if r.status_code == 404:
        pytest.skip("/api/test/resultput not present - board not flashed [env:test]")
    assert r.status_code == 200, f"resultput {r.status_code}: {r.text[:200]}"
    return r.json()


def _astool(ip, tok, chat, tool, args=None):
    """Call one MCP tool AS `chat`, through the real dispatcher + Principal path."""
    body = json.dumps(
        {"jsonrpc": "2.0", "id": 1, "method": "tools/call", "params": {"name": tool, "arguments": args or {}}}
    )
    r = requests.post(_u(ip, tok, "/api/test/astool"), data={"chat": chat, "body": body}, timeout=20)
    if r.status_code == 404:
        pytest.skip("/api/test/astool not present - board not flashed [env:test]")
    assert r.status_code == 200, f"astool {tool} as {chat}: {r.status_code} {r.text[:200]}"
    return r.json()


def _text(resp) -> str:
    """Flatten an MCP tools/call result to searchable text (errors included - a
    leak inside an error is still a leak, and a miss message is what we assert)."""
    if not isinstance(resp, dict):
        return str(resp)
    out = []

    def walk(x):
        if isinstance(x, dict):
            if isinstance(x.get("text"), str):
                out.append(x["text"])
            for v in x.values():
                walk(v)
        elif isinstance(x, list):
            for v in x:
                walk(v)

    walk(resp)
    return "\n".join(out)


@pytest.fixture(scope="module")
def board():
    if requests is None:
        pytest.skip("requests not installed")
    ip = os.environ.get("NIMBUS_TEST_IP")
    tok = os.environ.get("NIMBUS_TEST_TOKEN")
    if not ip or not tok:
        pytest.skip("set NIMBUS_TEST_IP + NIMBUS_TEST_TOKEN (WEBTOK?) to run L24")
    st = requests.get(_u(ip, tok, "/api/state"), timeout=10).json()
    if st.get("mode") != 1:
        pytest.skip("board is not in Orchestrator mode - flash/MODE 1 first")
    return ip, tok


def test_results_surface_and_miss_pointer(board):
    """A: results.list answers, and a bogus tag misses toward memory.episodic."""
    ip, tok = board
    lst = _text(_astool(ip, tok, CHAT_A, "results.list"))
    assert lst is not None and lst != "", "results.list returned nothing at all"
    # A never-stored tag: the miss must be actionable, not a bare false.
    miss = _text(_astool(ip, tok, CHAT_A, "results.get", {"tag": "r-nope-000"}))
    assert "episodic" in miss.lower(), f"miss message not actionable: {miss[:160]!r}"


def test_marker_roundtrips_full_text(board):
    """B: a seeded result returns byte-complete through results.get - MARKER in/out.

    The payload is padded past a single results.get page so the fetch also proves
    the paginated view stitches (offset paging), the exact path the loop clamp uses.
    """
    ip, tok = board
    # ~12 KB so the tail (with the marker) sits past the first ~8 KB page.
    body = f"[RESULT {MARKER} head]\n" + ("filler-line data ................\n" * 380) + f"\n[RESULT {MARKER} tail]"
    put = _resultput(ip, tok, CHAT_A, body, name="l24-roundtrip")
    tag = put.get("tag")
    assert tag, f"no tag from resultput: {put}"

    # Page through results.get AS the owner of the ns (present + complete).
    seen, offset, guard = "", 0, 0
    while guard < 8:
        guard += 1
        got = _text(_astool(ip, tok, CHAT_A, "results.get", {"tag": tag, "offset": offset}))
        # Header line: "bytes A-B of C". Strip it, accumulate the body.
        lines = got.split("\n", 1)
        header = lines[0]
        chunk = lines[1] if len(lines) > 1 else ""
        seen += chunk
        # Parse "bytes A-B of C" to advance / detect completion.
        try:
            span, _, total = header.replace("bytes ", "").partition(" of ")
            _a, _, b = span.partition("-")
            offset, total = int(b), int(total)
        except ValueError:
            break
        if offset >= total or not chunk:
            break
    assert f"[RESULT {MARKER} head]" in seen, "marker HEAD missing from results.get"
    assert f"[RESULT {MARKER} tail]" in seen, (
        "marker TAIL missing - pagination dropped the past-page bytes (the exact "
        "prism CRITICAL: header claims the full window but the body was clipped)"
    )


def test_ring_read_is_namespace_scoped(board):
    """C: a foreign chat cannot read another tenant's spilled result (prism CRITICAL).

    Paired POSITIVE fetch (same ns) guards against a vacuous pass - if scoping were
    broken open, the negative would still need the positive to hold, and if scoping
    rejected everyone the positive would fail. Only correct scoping passes both.
    """
    ip, tok = board
    secret = f"OWNER-ONLY {MARKER} do-not-leak"
    put = _resultput(ip, tok, CHAT_A, secret, name="l24-scoped")
    tag = put.get("tag")
    assert tag, f"no tag: {put}"

    mine = _text(_astool(ip, tok, CHAT_A, "results.get", {"tag": tag}))
    assert secret.split()[0] in mine, f"owner ns cannot read its own entry: {mine[:160]!r}"

    theirs = _text(_astool(ip, tok, CHAT_B, "results.get", {"tag": tag}))
    assert MARKER not in theirs, f"NAMESPACE LEAK: foreign chat read the entry: {theirs[:200]!r}"
    assert "episodic" in theirs.lower() or "not" in theirs.lower(), (
        f"foreign read should MISS (not silently empty): {theirs[:160]!r}"
    )
    # And the foreign chat's list must not enumerate it either.
    their_list = _text(_astool(ip, tok, CHAT_B, "results.list"))
    assert tag not in their_list, f"foreign results.list leaked the tag: {their_list[:200]!r}"


@pytest.mark.agent
def test_live_spill_overflows_fresh_ring(board):
    """D: a real deep fan-out overflows the 6-slot fresh-results ring -> a sub: spill.

    Paid + slow (spawns several cloud sub-agents). Skips LOUDLY if the model
    under-spawns so no overflow occurs - a vacuous pass would hide a real gap.
    """
    ip, tok = board
    prompt = (
        "Spawn SEVEN independent sub-agents in parallel, one per item, and have each "
        "reply with only a one-sentence answer ending in its tag: "
        "(1) a fun fact about the moon [RT-1], (2) about the ocean [RT-2], "
        "(3) about bees [RT-3], (4) about coffee [RT-4], (5) about music [RT-5], "
        "(6) about mountains [RT-6], (7) about rivers [RT-7]. Do not answer them "
        "yourself - delegate all seven."
    )
    r = requests.post(_u(ip, tok, "/api/chat"), data={"text": prompt}, timeout=15)
    assert r.status_code == 200, f"chat send {r.status_code}: {r.text[:200]}"
    # Let the fan-out complete (7 cloud sub-agents + synthesis).
    deadline = time.time() + 240
    reply = ""
    while time.time() < deadline:
        time.sleep(6)
        j = requests.get(_u(ip, tok, "/api/chat"), timeout=10).json()
        if not j.get("pending", True) and j.get("reply"):
            reply = j["reply"]
            break
    assert reply, "no synthesis reply within 240s"

    # The head runs as the owner (web chat) - read the ring as the owner.
    owner_chat = os.environ.get("NIMBUS_TEST_OWNER_CHAT", "")
    lister = owner_chat or CHAT_A
    lst = _text(_astool(ip, tok, lister, "results.list"))
    if "sub:" not in lst and lister == CHAT_A:
        # CHAT_A can't see the owner's spill - retry as owner if we know it; else
        # this is an honest skip, not a fail (scoping is WORKING, we just can't peek).
        pytest.skip(
            "live turn ran but its spill is owner-scoped; set "
            "NIMBUS_TEST_OWNER_CHAT to the owner chat id to assert the sub: entry"
        )
    assert "sub:" in lst, (
        "no sub: entry after a 7-way fan-out - the model under-spawned (no overflow) "
        f"or the spill hook is not wired. results.list=\n{lst[:400]}"
    )
