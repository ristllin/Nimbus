"""L58 - the nightly memory upkeep on the bench (program-2026-10-18, lane M; RB-Q2b).

Since v5.2.0 (lane RB-Q2b) the night makes no model call: it stores memories that
waited for an embedding, then decays, prunes and de-duplicates long-term memories,
once per local day. This leg drives the console ``DREAM`` drill on a bench unit and
reads the result where the owner would: the ``ev:dream`` timeline row (``EPIQ Nightly
dream``), ``/api/state`` and the absence of any provider request or Telegram send.

Rows are read with ``EPIQ --full`` and the counted-row reader of the turn record
legs (the summary line, then each row whole, newest first).

The drill is queued: the console replies ``DREAM queued`` and the turn task logs
``DREAM fired ok=<0|1> detail=<...>`` when the night ends. Every drill below also
checks that the device did not restart (STATUS uptime only grows, no panic banner)
and that no "File system is not mounted" line appeared while it ran.

Legs:
  0. Mount state over HTTP: ``/api/state`` says ``fsMounted`` before and after a
     drill, the last night (``dream``) is reported with its state saved, the device
     did not restart (``uptimeMs`` grows), and the ``/api/health`` internal-storage
     row is ok.
  1. No model call: with every provider unreachable (``FAULT provider on``) the
     night still reports ok or skipped, no turn starts, nothing is sent to Telegram.
  2. Storage: the night writes no NVS (STATUS: used bytes and failed NVS writes are
     the same before and after the drill; run it on the NVSFILL'd unit).

Conditions: unit L (the fill unit), Orchestrator mode, the owner's Telegram chat
allowlisted, the unit on the LAN. Every leg costs nothing (no request leaves the
device).
"""

from __future__ import annotations

import re
import time

import pytest

from device import Device, ExpectTimeout
from test_turn_record_bench import _epiq

pytestmark = [pytest.mark.hil, pytest.mark.agent]

_PANIC = ("Guru Meditation", "Rebooting...", "Backtrace:")
_FS_DOWN = "File system is not mounted"
# What a model turn logs when it starts (the orchestrator's turn line).
_TURN_MARKS = ("orchestrator: turn host=",)


def _dream_rows(dev: Device) -> list[str]:
    """The newest ev:dream rows (up to three), newest first, each whole."""
    return [text for _, text in _epiq(dev, "Nightly dream") if text.startswith("Nightly dream ")]


def _status(dev: Device) -> dict:
    """STATUS fields this leg reads (read only): uptime, NVS use, the LAN IP."""
    line = dev.cmd("STATUS", "STATUS fw=", timeout=8.0)

    def field(pattern: str) -> str:
        m = re.search(pattern, line)
        assert m, f"STATUS has no {pattern!r}: {line}"
        return m.group(1)

    return {
        "uptime": int(field(r"uptime=(\d+)")),
        "nvs": int(field(r"nvs=(\d+)/\d+")),
        "nvsfail": int(field(r"nvsfail=(\d+)")),
        "ip": field(r"\bip=(\d+\.\d+\.\d+\.\d+)"),
    }


def _drill(dev: Device, timeout: float = 120.0) -> str:
    """Run the DREAM drill; return the transcript lines it produced. Fails on a
    restart, a panic banner or a file-system-not-mounted line in the window."""
    before = _status(dev)
    t0 = time.time()
    dev.drain(quiet=0.5)
    start = len(dev._transcript)
    dev.send("DREAM")
    queued = dev.expect("DREAM ", timeout=5.0)
    assert "DREAM queued" in queued, f"the drill was not queued (an earlier night still runs?): {queued}"
    dev.expect("DREAM fired", timeout=timeout)
    # Telegram sends are queued; give one time to surface (the night sends none).
    try:
        dev.expect("telegram: send", timeout=6.0)
    except ExpectTimeout:
        pass
    window = "\n".join(dev._transcript[start:])
    for banner in _PANIC:
        assert banner not in window, f"the device crashed during the drill ({banner!r})"
    assert _FS_DOWN not in window, "a file-system-not-mounted error appeared during the drill"
    # STATUS uptime is millis(): without a restart it grew by about the wall time.
    grown = _status(dev)["uptime"] - before["uptime"]
    assert grown >= (time.time() - t0) * 1000 - 5000, "the device restarted during the drill"
    return window


def _health_row(health: dict, key: str) -> dict:
    for row in health.get("components", []):
        if row.get("key") == key:
            return row
    raise AssertionError(f"/api/health has no {key!r} row: {health}")


def night_made_no_turn(window: str) -> "str | None":
    """Why the drill's window shows a model turn, or None when it shows none."""
    for mark in _TURN_MARKS:
        if mark in window:
            return f"the night started a model turn ({mark!r})"
    if "telegram: send" in window:
        return "the night sent a Telegram message"
    return None


def night_row_ok(newest: str) -> "str | None":
    """Why the newest ev:dream row is not an upkeep row of a night that ran, or None."""
    if not newest.startswith("Nightly dream "):
        return f"not an ev:dream row: {newest!r}"
    if "token" in newest:
        return f"the row reports spend: {newest!r}"
    if newest.startswith("Nightly dream ok: maintenance pruned="):
        return None
    if newest.startswith("Nightly dream skipped (maintenance done earlier today)"):
        return None
    return f"the night neither ran nor was done earlier today: {newest!r}"


@pytest.fixture(scope="module")
def orch(device: Device) -> Device:
    # The session's port open can reset the board: give the console its boot first.
    deadline = time.time() + 45.0
    while not device.ping():
        if time.time() > deadline:
            pytest.skip("console not answering")
        time.sleep(2.0)
    return device


def _faulted_drill(dev: Device) -> str:
    dev.cmd("FAULT provider on", "FAULT provider=1", timeout=5.0)
    try:
        return _drill(dev)
    finally:
        dev.cmd("FAULT clear", "FAULT cleared", timeout=5.0)


@pytest.mark.net
def test_mount_state_is_reported_and_the_drill_does_not_restart(orch: Device, net):
    ip = _status(orch)["ip"]
    st0 = net.get_json("/api/state", ip=ip, timeout=8.0)
    assert st0.get("fsMounted") is True, f"internal storage not mounted before the drill: {st0.get('fsMounted')}"
    assert _health_row(net.get_json("/api/health", ip=ip, timeout=8.0), "flash")["state"] == "ok"
    _faulted_drill(orch)
    st1 = net.get_json("/api/state", ip=ip, timeout=8.0)
    assert st1.get("fsMounted") is True
    assert st1["uptimeMs"] > st0["uptimeMs"], "the device restarted during the drill"
    assert st1.get("testWake") == st0.get("testWake"), "the reset reason changed: the device restarted"
    night = st1.get("dream", {})
    assert night.get("known") is True, night
    assert night.get("last") in ("ran", "skipped"), night
    assert night.get("stateSaved") is True, night
    assert _health_row(net.get_json("/api/health", ip=ip, timeout=8.0), "flash")["state"] == "ok"


def test_the_night_makes_no_model_call(orch: Device):
    window = _faulted_drill(orch)
    assert "DREAM fired ok=1" in window, window[-400:]
    why = night_made_no_turn(window)
    assert why is None, why
    rows = _dream_rows(orch)
    assert rows, "no ev:dream row was written"
    why = night_row_ok(rows[0])
    assert why is None, why


def test_dream_writes_no_nvs(orch: Device):
    before = _status(orch)
    _faulted_drill(orch)
    after = _status(orch)
    assert (after["nvs"], after["nvsfail"]) == (before["nvs"], before["nvsfail"]), (
        "the dream path wrote NVS (used bytes or failed writes moved)"
    )
