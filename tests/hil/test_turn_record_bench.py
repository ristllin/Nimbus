"""Turn record bench legs (program-2026-10-18, lane T): why a turn failed, and what it cost.

Two legs, run by the bench operator on a fill unit (never a production device):

* ``test_b1_failure_provenance`` - a forced head 400 during the console ``DREAM``
  drill must leave the FIRST cause in every place a failure shows: the ``ev:error``
  row, the turn-end row (present, at most 256 B), the ``ev:dream`` row,
  ``/api/loops`` ``lastErr`` and ``device.status`` ``lastFail``. The faulted
  requests bill nothing, so the settings store (NVS) must not change: STATUS
  ``nvs=`` / ``nvsfree=`` / ``nvsfail=`` are compared here, and the operator diffs
  raw NVS dumps taken before and after (``esptool read-flash 0x9000 0x6000``).
  Needs ``PROVFAULT`` (the provider-wire test hook) and a unit whose head can only
  reach the faulted provider (one keyed provider, or the others over budget):
  otherwise another provider answers and the leg SKIPs, loudly.
* ``test_b2_full_nvs_turn`` - with NVS filled to about 288 B free (``NVSFILL``),
  one plain turn still writes its turn-end row and its ``turn spend:`` line, and
  adds at most one failed settings write (the one ledger write), never one per
  provider or tag.

The switched-turn leg (a turn that moves provider mid-turn showing the per-host
split, the ``via`` array and the Anthropic cache write under its claude model)
needs a counted provider fault on the device (fail requests 2.. of one host); the
test build has no such verb yet, so it is proven live in tools/harness-lab
(``metering-switch``) and listed as a follow-up.

Environment: NIMBUS_TEST_IP + NIMBUS_TEST_TOKEN (``WEBTOK?``) for the LAN reads.
"""

from __future__ import annotations

import json
import re

import pytest

from webrig import ip_tok_or_skip, make_session, require_orchestrator

pytestmark = pytest.mark.hil

# The ceiling the device stores a turn-end row under (ORCH_TRACE_END_MAX).
TURN_END_ROW_MAX = 256
# About 288 B free: 9 free 32-byte NVS entries.
NVS_TARGET_FREE_ENTRIES = 9

ANTHROPIC_400 = json.dumps(
    {
        "type": "error",
        "error": {
            "type": "invalid_request_error",
            "message": "The request body is not valid JSON: invalid unicode code point at line 1 column 55142",
        },
    }
)
FIRST_CAUSE = "invalid unicode code point"


def _nvs(device) -> dict:
    m = device.cmd_re(
        "STATUS", r"nvs=(?P<used>\d+)/(?P<total>\d+)\s+nvsfree=(?P<free>\d+)\s+nvsfail=(?P<fail>\d+)", timeout=4.0
    )
    return {k: int(v) for k, v in m.groupdict().items()}


def _epiq(device, text: str) -> str:
    """EPIQ <text>: up to three matching history rows (newest first), as one reply."""
    device.send(f"EPIQ {text}")
    return device.expect("EPIQ", timeout=8.0)


def _mcp(session, ip: str, tool: str, args: dict) -> str:
    body = json.dumps({"jsonrpc": "2.0", "id": 1, "method": "tools/call", "params": {"name": tool, "arguments": args}})
    r = session.post(f"http://{ip}/mcp", data=body, headers={"Content-Type": "application/json"}, timeout=15)
    assert r.status_code == 200, f"/mcp {tool} -> {r.status_code}: {r.text[:160]}"
    content = r.json().get("result", {}).get("content", [])
    return " ".join(c.get("text", "") for c in content)


def _provfault(device, arg: str) -> str:
    device.send(f"PROVFAULT {arg}")
    line = device.expect("PROVFAULT", timeout=4.0)
    if "not wired" in line:
        pytest.skip("PROVFAULT is not wired in this build (needs the provider-wire test hook)")
    return line


def test_b1_failure_provenance(device):
    ip, tok = ip_tok_or_skip("turn record B1")
    s = make_session(tok)
    require_orchestrator(s, ip, "turn record B1")
    before = _nvs(device)
    _provfault(device, f"api.anthropic.com 400 {ANTHROPIC_400}")
    try:
        device.send("DREAM")
        fired = device.expect("DREAM fired", timeout=240.0)
    finally:
        _provfault(device, "off")
    if "ok=1" in fired:
        pytest.skip("the dream succeeded on another provider: restrict the unit to the faulted provider")

    err = _epiq(device, "Turn FAILED")
    assert FIRST_CAUSE in err, f"ev:error lacks the first cause: {err[:300]}"
    assert "(dream)" in err, f"ev:error does not say whose turn failed: {err[:300]}"

    dream = _epiq(device, "Nightly dream FAILED")
    assert FIRST_CAUSE[:20] in dream or "anthropic" in dream, f"ev:dream lacks the cause: {dream[:300]}"
    assert "spent" in dream, f"ev:dream spend is not labelled: {dream[:300]}"

    end = _epiq(device, '"src":"dream"')
    rows = re.findall(r"\{[^{}]*\"src\":\"dream\"[^{}]*(?:\[[^\]]*\][^{}]*)*\}", end)
    assert rows, f"no turn-end row for the dream: {end[:300]}"
    assert len(rows[0].encode()) <= TURN_END_ROW_MAX, f"turn-end row over {TURN_END_ROW_MAX} B"
    assert '"ok":false' in rows[0] and "anthropic" in rows[0], f"turn-end row lacks the cause: {rows[0]}"

    loops = s.get(f"http://{ip}/api/loops", timeout=10).json()
    items = loops if isinstance(loops, list) else loops.get("loops", [])
    dream_rec = next((x for x in items if "dream" in str(x.get("name", "")).lower()), None)
    assert dream_rec and dream_rec.get("lastErr"), f"loops lastErr missing: {dream_rec}"
    assert "anthropic" in dream_rec["lastErr"], f"loops lastErr does not name the first cause: {dream_rec}"

    st = _mcp(s, ip, "device.status", {})
    m = re.search(r'"lastFail":(\{.*?\}|null)', st)
    assert m and m.group(1) != "null", f"device.status lastFail missing: {st[:300]}"
    assert "anthropic" in m.group(1) and '"src":"dream"' in m.group(1), f"lastFail: {m.group(1)}"

    after = _nvs(device)
    assert after == before, f"NVS changed across a failed, unbilled dream: {before} -> {after}"


def test_b2_full_nvs_turn(device):
    base = _nvs(device)
    free_entries = base["free"] // 32
    fill = max(0, free_entries - NVS_TARGET_FREE_ENTRIES)
    device.send(f"NVSFILL {fill}")
    device.expect("NVSFILL wrote=", timeout=60.0)
    try:
        filled = _nvs(device)
        device.drain()
        device.turn("Reply with the single word: ok")
        spend = device.expect("turn spend:", timeout=180.0)
        assert " ok " in spend, f"the turn did not finish: {spend}"
        device.expect("turn end:", timeout=30.0)
        after = _nvs(device)
        assert after["fail"] - filled["fail"] <= 1, (
            f"more than one failed settings write for one turn: {filled} -> {after}"
        )
        end = _epiq(device, '"src":"serial"')
        assert '"src":"serial"' in end, f"no turn-end row for the turn: {end[:300]}"
    finally:
        device.send("NVSFILL 0")
        device.expect("NVSFILL cleared", timeout=60.0)
