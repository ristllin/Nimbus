"""Turn record bench legs (program-2026-10-18, lane T): why a turn failed, and what it cost.

Two legs, run by the bench operator on a fill unit (never a production device):

* ``test_b1_failure_provenance`` - a forced head 400 during the console ``DREAM``
  drill must leave the FIRST cause in every place a failure shows: the ``ev:error``
  row, the turn-end row (present, at most 256 B), the ``ev:dream`` row, the dream's
  own status (``/api/state`` ``dream.err``) and ``device.status`` ``lastFail``. The
  drill is not a scheduled fire, so it leaves the routine record (``/api/loops``)
  as it was: the record's ``lastErr`` is written by the scheduler's fire and by the
  boot reconcile of an interrupted night (host-tested in test_harness_dream_night;
  the reconcile on hardware in test_l58_dream_quiet). The faulted
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

Both legs read history rows with the console's ``EPIQ --full`` (whole rows; the
default 48-character preview cannot show a cause or a turn-end row's later fields).
B1 first checks that the unit's internal storage is mounted and fails with the reason
and the last log lines when it is not, or when DREAM never answers.

Environment: NIMBUS_TEST_IP + NIMBUS_TEST_TOKEN (``WEBTOK?``) for the LAN reads.
"""

from __future__ import annotations

import json
import re
import time
from typing import Iterable, Iterator, List, Optional, Tuple

import pytest

from device import ExpectTimeout
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


# ---- EPIQ replies (host-tested in test_turn_record_bench_host.py) ----------------
# The console answers ``EPIQ rows=<n> ...`` and then one line per row,
# ``  <id> <text>``: a 48-character preview, or the whole row with ``--full``.
# device.expect() returns only the summary line, so the rows are read after it.
EPIQ_PREVIEW_CHARS = 48
_EPIQ_SUMMARY = re.compile(r"EPIQ rows=(\d+)")
_EPIQ_ROW = re.compile(r"^(m[0-9a-fA-F]+) (.*)$")
EPIQ_ROW_WAIT_S = 3.0  # bounded wait for the row lines after the summary


def epiq_rows_from_lines(lines: Iterable[str]) -> List[Tuple[str, str]]:
    """The rows of one EPIQ reply: its summary line, then as many row lines as the
    summary counts, skipping log lines that interleave. Never reads past the last
    row, so a live line source is not waited on for nothing."""
    it = iter(lines)
    count: Optional[int] = None
    for line in it:
        m = _EPIQ_SUMMARY.search(line)
        if m:
            count = int(m.group(1))
            break
    if count is None:
        raise AssertionError("no 'EPIQ rows=' summary line in the reply")
    rows: List[Tuple[str, str]] = []
    while len(rows) < count:
        line = next(it, None)
        if line is None:
            break
        r = _EPIQ_ROW.match(line.strip())
        if r:
            rows.append((r.group(1), r.group(2)))
    return rows


def find_row(rows: List[Tuple[str, str]], needle: str) -> str:
    """The first row text holding ``needle``; fails with the reason when none does,
    naming a 48-character preview (an image without ``EPIQ --full``) when the text
    sits past what the preview shows."""
    for _, text in rows:
        if needle in text:
            return text
    previews = [t for _, t in rows if len(t) == EPIQ_PREVIEW_CHARS]
    if previews:
        raise AssertionError(
            f"no row holds {needle!r}: the rows are {EPIQ_PREVIEW_CHARS}-character previews "
            f"(this image has no EPIQ --full), so the text is unreachable: {previews}"
        )
    raise AssertionError(f"no row holds {needle!r}: {rows}")


def _epiq_lines(device, query: str, timeout: float) -> Iterator[str]:
    device.drain(quiet=0.2)
    device.send(f"EPIQ --full {query}")
    yield device.expect("EPIQ rows=", timeout=timeout)
    deadline = time.time() + EPIQ_ROW_WAIT_S
    while True:
        left = deadline - time.time()
        if left <= 0:
            return
        try:
            yield device.expect_re(r"^m[0-9a-fA-F]+ ", timeout=left).string
        except ExpectTimeout:
            return


def _epiq(device, query: str) -> List[Tuple[str, str]]:
    """EPIQ --full <query>: up to three matching history rows (newest first), whole."""
    return epiq_rows_from_lines(_epiq_lines(device, query, timeout=8.0))


# ---- B1 environment checks (host-tested) ------------------------------------------
NOT_MOUNTED = "File system is not mounted"
NOT_MOUNTED_BURST = 3  # this many in a short window: the unit's storage is down
DREAM_STARTED = "dream: maintenance"  # the first line every dream fire logs
PRECHECK_S = 6.0  # the burst repeats every one to two seconds
RECENT_LINES = 40  # the device transcript spans the session: judge only its end


def not_mounted_count(lines: Iterable[str]) -> int:
    return sum(1 for line in lines if NOT_MOUNTED in line)


def storage_down_reason(lines: List[str]) -> Optional[str]:
    """Why B1 cannot run on this unit, or None: a burst of "not mounted" errors means
    the internal storage (LittleFS /data, the dream state and the durable log) is not
    mounted, a condition of the bench unit rather than of the code under test."""
    n = not_mounted_count(lines)
    if n < NOT_MOUNTED_BURST:
        return None
    return (
        f"the unit's internal storage is not mounted ({n} '{NOT_MOUNTED}' errors in the log): "
        "the dream state and the durable log live there, so the dream leg cannot run on this unit"
    )


def dream_never_fired_reason(tail: List[str], waited_s: float) -> str:
    """The failure text for a DREAM that never answered: what the log shows, and the
    last 20 lines."""
    why = f"DREAM did not answer within {waited_s:.0f} s"
    recent = tail[-RECENT_LINES:]
    down = storage_down_reason(recent)
    if down:
        why += "; " + down
    elif not any(DREAM_STARTED in line for line in recent):
        why += "; no dream started (no 'dream: maintenance' line)"
    last = "\n    ".join(tail[-20:]) if tail else "<no lines received>"
    return f"{why}\nlast 20 lines:\n    {last}"


# ---- B1 dream surfaces (host-tested) ----------------------------------------------
# The routine record fields a scheduled fire moves; a console drill moves none of them.
LOOP_FIRE_FIELDS = ("lastRun", "lastResult", "consecFails", "firesToday", "tokensToday", "lastErr")


def dream_loop(loops) -> Optional[dict]:
    """The reserved dream routine from a GET /api/loops answer (a list, or {"loops": [...]})."""
    items = loops if isinstance(loops, list) else loops.get("loops", [])
    return next((x for x in items if x.get("id") == "dream"), None)


def drill_moved_loop(before: Optional[dict], after: Optional[dict]) -> Optional[str]:
    """Why the drill moved the dream routine record, or None. A drill is not a
    scheduled fire: it must not claim a run, burn a daily fire or roll the breaker."""
    if before is None or after is None:
        return f"the dream routine is missing from /api/loops (before={before}, after={after})"
    moved = {k: (before.get(k), after.get(k)) for k in LOOP_FIRE_FIELDS if before.get(k) != after.get(k)}
    return f"the drill moved the dream routine record: {moved}" if moved else None


def drill_status_reason(dream: dict) -> Optional[str]:
    """Why /api/state ``dream`` does not report the drill's failure, or None."""
    if not dream.get("known"):
        return f"/api/state reports no dream since boot: {dream}"
    if dream.get("last") != "failed":
        return f"/api/state dream.last is {dream.get('last')!r}, want 'failed': {dream}"
    if not dream.get("err"):
        return f"/api/state dream.err is empty for a failed night: {dream}"
    return None


def _watch(device, seconds: float) -> List[str]:
    """Every line the device logs for ``seconds`` (bounded)."""
    seen: List[str] = []
    deadline = time.time() + seconds
    while True:
        left = deadline - time.time()
        if left <= 0:
            return seen
        try:
            seen.append(device.expect_re(r".", timeout=left).string)
        except ExpectTimeout:
            return seen


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
    # The bench unit's storage must be mounted (the dream state and the durable log
    # live there): a "not mounted" burst fails here with the reason, not as a timeout.
    down = storage_down_reason(_watch(device, PRECHECK_S))
    if down:
        pytest.fail(f"B1 cannot run: {down}")
    before = _nvs(device)
    loop_before = dream_loop(s.get(f"http://{ip}/api/loops", timeout=10).json())
    _provfault(device, f"api.anthropic.com 400 {ANTHROPIC_400}")
    fired, never = "", None
    try:
        device.send("DREAM")
        fired = device.expect("DREAM fired", timeout=240.0)
    except ExpectTimeout as e:
        never = dream_never_fired_reason(e.tail, 240.0)
    finally:
        _provfault(device, "off")
    if never:
        pytest.fail(never)
    if "ok=1" in fired:
        pytest.skip("the dream succeeded on another provider: restrict the unit to the faulted provider")

    err = find_row(_epiq(device, "Turn FAILED"), "Turn FAILED")
    assert FIRST_CAUSE in err, f"ev:error lacks the first cause: {err[:300]}"
    assert "(dream)" in err, f"ev:error does not say whose turn failed: {err[:300]}"

    dream = find_row(_epiq(device, "Nightly dream FAILED"), "Nightly dream FAILED")
    assert FIRST_CAUSE[:20] in dream or "anthropic" in dream, f"ev:dream lacks the cause: {dream[:300]}"
    assert "spent" in dream, f"ev:dream spend is not labelled: {dream[:300]}"

    end = find_row(_epiq(device, '"src":"dream"'), '"src":"dream"')
    assert len(end.encode()) <= TURN_END_ROW_MAX, f"turn-end row over {TURN_END_ROW_MAX} B: {end}"
    assert '"ok":false' in end and "anthropic" in end, f"turn-end row lacks the cause: {end}"

    night = s.get(f"http://{ip}/api/state", timeout=10).json().get("dream", {})
    why = drill_status_reason(night)
    assert why is None, why
    assert "anthropic" in night["err"] or FIRST_CAUSE[:20] in night["err"], f"dream.err lacks the cause: {night}"
    moved = drill_moved_loop(loop_before, dream_loop(s.get(f"http://{ip}/api/loops", timeout=10).json()))
    assert moved is None, moved

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
        end = find_row(_epiq(device, '"src":"serial"'), '"src":"serial"')
        assert len(end.encode()) <= TURN_END_ROW_MAX, f"turn-end row over {TURN_END_ROW_MAX} B: {end}"
    finally:
        device.send("NVSFILL 0")
        device.expect("NVSFILL cleared", timeout=60.0)
