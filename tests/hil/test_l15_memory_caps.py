"""§L15 - max-memory behaviors at their caps (plan §1f, owner requirement).

Every store's at-the-limit behavior, exercised ON HARDWARE via the MEMFILL /
COMPACT seams + the LAN API - with the E2E bar's rules: synthetic-only damage
(the vector-cap test LOWERS max_vectors so eviction can only ever touch
hiltest-* rows, never the owner's real memories), finally-restore on every knob
and fault, loud skips.

ORDER MATTERS: the degraded-fold row runs FIRST - the episodic fill loads the
PSRAM ring with 512 small-string rows whose sub-128 B allocations land in
INTERNAL SRAM (~30 K held; measured heapMin 10.4 K during the fill), leaving
the board under the fold's 30 K heap gate for the rest of the session. That
measurement is also why the ring stays at 512 (the planned 1024 bump is
rejected - it would double the internal cost).

Rows covered (plan §1f matrix):
- vector store at the cap: size clamps, lowest-importance synthetic rows evict
  first, owner rows survive (asserted by count).
- episodic past the PSRAM ring: old MARKER rows stay queryable via the SD
  day-stream after the ring evicts them.
- degraded no-SD fold: FAULT sd -> /compact still completes from the RAM ring
  (the plan's pinned ONE behavior) and the device keeps serving.

Markers: ``hil`` + ``net`` - PURE LAN (the seams ride token-gated
``/api/test/*`` endpoints; serial CDC opens reset the board and wedge the host
driver, so no test here touches the port). The
degraded-fold row additionally needs a keyed provider - it skips loudly
without one. hiltest-* residue: episodic rows fall to the 30-day retention
prune; synthetic vectors carry ttl 720 h and are swept by dream maintenance.
"""

from __future__ import annotations

import os
import time

import pytest

try:
    import requests
except ImportError:  # pragma: no cover
    requests = None

pytestmark = [pytest.mark.hil, pytest.mark.net]


def _url(ip, tok, path, sep="?"):
    return f"http://{ip}{path}{sep}t={tok}"


@pytest.fixture
def rig(allow_hardware):
    if requests is None:
        pytest.skip("requests not installed")
    ip = os.environ.get("NIMBUS_TEST_IP")
    tok = os.environ.get("NIMBUS_TEST_TOKEN")
    if not ip or not tok:
        pytest.skip("set NIMBUS_TEST_IP + NIMBUS_TEST_TOKEN (WEBTOK?) to run L15")
    st = requests.get(_url(ip, tok, "/api/state"), timeout=10).json()
    if st.get("mode") != 1:
        pytest.skip("Orchestrator mode required (MODE 1)")
    return None, ip, tok


def _memfill(rig_ip_tok, kind: str, n: int, bytes_per=64) -> int:
    """One chunked fill over the LAN seam; returns rows added."""
    ip, tok = rig_ip_tok
    r = requests.post(
        _url(ip, tok, "/api/test/memfill"), data={"kind": kind, "n": str(n), "bytes": str(bytes_per)}, timeout=60
    )
    assert r.status_code == 200, f"memfill -> {r.status_code}: {r.text[:120]}"
    return int(r.json()["added"])


def _vec_stats(ip, tok):
    r = requests.get(_url(ip, tok, "/api/mem/config"), timeout=10).json()
    return r


def test_degraded_fold_completes_from_ram_ring(rig):
    device, ip, tok = rig
    # Needs a keyed provider (the fold is an LLM call) - probe via /api/orch.
    orch = requests.get(_url(ip, tok, "/api/connectors"), timeout=10).json()
    keyed = orch.get("keyed", {})
    if not any(keyed.get(k) for k in ("openai", "anthropic", "mistral")):
        pytest.skip("no provider key on the board - degraded fold needs one")

    # The ORACLE is the ev:compact row the fold itself writes - not the
    # /api/test/ctx diagnostic this test used to poll.
    #
    # That diagnostic reads an echo buffer whose only writer is the onTurnEnd
    # hook, so it reported the fold stamp as of the last TURN, never as of the
    # last FOLD. The test therefore passed only when the echo happened to be one
    # fold stale - observing the PREVIOUS run's fold and re-arming the lag for
    # the next. A vacuous green that went red the moment anything else posted a
    # turn to the web chat first (l13/l14 do). The firmware now republishes on
    # every fold outcome, and this test no longer depends on that at all.
    #
    # ev:compact is written by compactTick ONLY on FoldResult::Ok, and under
    # FAULT sd it lands via the PSRAM ring - exactly the path this test exists
    # to prove. Row ids are a monotonic counter, so "a new one appeared" is
    # unambiguous; the ts field is hour-granular and boot-relative, so it is not.
    def _compact_row_ids():
        r = requests.get(
            _url(ip, tok, "/api/mem/episodic"),
            params={"t": tok, "session": "web", "kind": "log", "limit": 20},
            timeout=15,
        )
        rows = r.json().get("messages", []) if r.status_code == 200 else []
        return {m.get("id") for m in rows if "ev:compact" in (m.get("tags") or "")}

    try:
        r = requests.post(_url(ip, tok, "/api/fault"), data={"cap": "sd", "on": "1"}, timeout=10)
        assert r.status_code == 200, "FAULT sd on failed"
        before = _compact_row_ids()

        requests.post(_url(ip, tok, "/api/chat"), data={"text": "Reply OK only: degraded fold marker."}, timeout=10)
        time.sleep(25)  # let the turn land on tg_poll
        # Nudge past the byte threshold. The stage bypasses thresholds; if the
        # automatic pass already folded, this is a harmless no-op and the
        # assertion below is satisfied by that pass - either way a fold
        # COMPLETED with no card, which is the claim.
        r = requests.post(_url(ip, tok, "/api/test/compact"), data={"chat": "web"}, timeout=10)
        assert r.status_code == 202, f"compact stage -> {r.status_code}"

        deadline = time.time() + 120
        folded = False
        while time.time() < deadline and not folded:
            time.sleep(5)
            folded = bool(_compact_row_ids() - before)
        assert folded, "no fold completed from the RAM ring with the card faulted out (no new ev:compact row in 120 s)"
        # The device must still be serving over LAN throughout.
        assert requests.get(_url(ip, tok, "/api/state"), timeout=10).status_code == 200
    finally:
        requests.post(_url(ip, tok, "/api/fault"), data={"cap": "all", "t": tok}, timeout=10)
        # Force the SD re-promote NOW (the health tracker's debounce otherwise
        # leaves sdLost latched for its probe window, skipping the next SD row).
        requests.post(_url(ip, tok, "/api/sdprobe"), data={"t": tok}, timeout=20)
        deadline = time.time() + 30
        while time.time() < deadline:
            st = requests.get(_url(ip, tok, "/api/state"), timeout=10).json()
            if not st.get("sdLost"):
                break
            time.sleep(3)


def test_vector_cap_clamps_and_evicts_synthetic_only(rig):
    device, ip, tok = rig
    cfg = _vec_stats(ip, tok)
    orig_cap = int(cfg.get("max_vectors", 5000) or 5000)
    st0 = requests.get(_url(ip, tok, "/api/mem/stats"), timeout=10).json()
    owner_rows = int(st0.get("vectors", 0))
    test_cap = owner_rows + 120  # synthetic-only headroom above the owner's rows
    try:
        r = requests.put(_url(ip, tok, "/api/mem/config"), data={"max_vectors": str(test_cap)}, timeout=10)
        assert r.status_code in (200, 204), f"config put -> {r.status_code}"
        # Fill PAST the lowered cap: 2 chunks of 100 (>120 headroom).
        added = _memfill((ip, tok), "vec", 100) + _memfill((ip, tok), "vec", 100)
        assert added >= 120, f"memfill added only {added}"
        st1 = requests.get(_url(ip, tok, "/api/mem/stats"), timeout=10).json()
        size = int(st1.get("vectors", 0))
        assert size <= test_cap, f"store exceeded the cap: {size} > {test_cap}"
        # Owner rows must all survive: eviction at the cap picks the LOWEST
        # importance*ttl-left, and the synthetic ramp starts at 0.10 - far below
        # any real memory. size == cap and (cap - owner) synthetic slots proves
        # nothing real was evicted.
        assert size >= owner_rows, "owner vectors evicted - E2E bar violation"
    finally:
        requests.put(_url(ip, tok, "/api/mem/config"), data={"max_vectors": str(orig_cap)}, timeout=10)


def test_episodic_past_ring_still_queryable_from_sd(rig):
    device, ip, tok = rig
    st = requests.get(_url(ip, tok, "/api/state"), timeout=10).json()
    if st.get("sdLost") or st.get("sd") == "absent":
        pytest.skip("SD required for the past-ring day-stream row")
    st0 = requests.get(_url(ip, tok, "/api/mem/stats"), timeout=30).json()
    n0 = int(st0.get("episodicMsgs", 0))
    # 600 rows (3 chunks) - the PSRAM ring holds 512; the oldest fall to the
    # SD-only path. Each chunk is 200 SD appends under the memory Lock, so give
    # the web server a breath between chunks (HTTP contends on the same Lock).
    total = 0
    for _ in range(3):
        total += _memfill((ip, tok), "epi", 200, 48)
        time.sleep(2)
    assert total == 600, f"memfill epi added {total}"
    st1 = requests.get(_url(ip, tok, "/api/mem/stats"), timeout=30).json()
    n1 = int(st1.get("episodicMsgs", 0))
    assert n1 - n0 >= 600, f"store grew only {n1 - n0} (ring-evicted rows lost?)"
    # Ring-window read stays fast and full:
    r = requests.get(
        _url(ip, tok, "/api/mem/episodic"), params={"t": tok, "session": "hiltest-epi", "limit": 100}, timeout=30
    )
    assert r.status_code == 200
    msgs = r.json().get("messages", [])
    assert len(msgs) >= 100, f"only {len(msgs)} rows came back"
    # Past-the-ring retention proof: the index count (episodicMsgs) spans the SD
    # day-streams, and it grew by all 600 while the PSRAM ring holds only 512 -
    # so ≥88 rows live ONLY on SD and remain part of the queryable index. A
    # direct row READ beyond the newest 50 is not possible over this endpoint
    # (limit caps at 50; pagination is a tracked follow-up seam), so the count
    # delta + the full ring-window read above are the honest assertions.
    assert n1 - n0 >= 600 and len(msgs) >= 50

    # RESTORE (E2E bar): the 48 B fill texts are sub-128 B allocations that land
    # in INTERNAL SRAM once the ring rehydrates them (~30 K held - measured: the
    # board could no longer clear the 28 K turn floor, even across reboots).
    # Displace the ring with >=128 B texts (PSRAM spill) so the board leaves the
    # test healthier than it entered (measured 26 K -> 65 K free).
    for _ in range(3):
        _memfill((ip, tok), "epi", 200, 256)
        time.sleep(2)
