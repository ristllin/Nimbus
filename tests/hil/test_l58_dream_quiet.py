"""L58 - quiet, honest nightly dream (program-2026-10-18, lane M).

The nightly dream used to report "ok pruned=0 deduped=0" whatever happened, and a
failed dream sent "That didn't finish ... ask again to retry" to the owner's
Telegram at about 04:30 (canonical rows 27 and 28). This leg drives the console
``DREAM`` drill on a bench unit and reads the result where the owner would: the
``ev:dream`` timeline row (``EPIQ Nightly dream``) and the absence of any Telegram
send while the quiet turn failed.

Rows are read with ``EPIQ --full`` and the counted-row reader of the turn record
legs (the summary line, then each row whole, newest first): the first version read
only up to the summary line and saw no rows at all, and the default 48-character
preview cuts the counts off.

The drill is queued: the console replies ``DREAM queued`` and the turn task logs
``DREAM fired ok=<0|1> detail=<...>`` when the night ends. (It used to run the night
on the loop task, whose 8 KB stack cannot hold a turn: the device panicked and
rebooted, so ``DREAM fired`` never came.) Every drill below also checks that the
device did not restart (STATUS uptime only grows, no panic banner) and that no
"File system is not mounted" line appeared while it ran.

Legs:
  0. Mount state over HTTP: ``/api/state`` says ``fsMounted`` before and after a
     drill, the last night (``dream``) is reported with its state saved, the
     device did not restart (``uptimeMs`` grows), and the ``/api/health``
     internal-storage row is ok.
  1. Every provider unreachable (``FAULT provider on``): the row says FAILED with
     its cause and the write counts, a retry is scheduled for the same night, and
     the device sends NOTHING to Telegram (the failure notice is held).
  2. Memory refused (``FAULT memory on``) after an owner turn that states durable
     facts: the reflection's writes fail, and the row says degraded with the
     failed count - never ok.
  3. Storage: the dream path writes no NVS (STATUS: used bytes and failed NVS
     writes are the same before and after the drill; run it on the NVSFILL'd unit).
  4. A restart mid-drill (``PANIC arm=turn`` while the reflection turn runs): the
     next boot reconciles it - one ``Nightly dream interrupted`` timeline row,
     the dream's last result fail with "the device restarted during this run",
     and a ``/api/loops`` next run that retries the same night while the window
     is open (the interrupted attempt is one of the bounded retries). The panic
     handler's report is the crash under test, so the leg reads past it to the
     ROM's ``rst:`` banner before the boot scan (which fails on any panic marker)
     takes over. Skipped when the image has no arm hook.

Conditions: unit L (the fill unit), Orchestrator mode, a cheap head
(claude-haiku-5-5), the owner's Telegram chat allowlisted, the unit on the LAN.
Legs 0, 1 and 3 cost nothing (the requests never leave the device); leg 2 costs one
owner turn and one reflection turn on the cheap head (well under $0.01).
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


def _drill(dev: Device, timeout: float = 240.0) -> str:
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
    # Telegram sends are queued; give a held-or-sent notice time to surface.
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


@pytest.fixture(scope="module")
def orch(device: Device, secrets) -> Device:
    if not secrets.get("ANTHROPIC_API_KEY"):
        pytest.skip("no ANTHROPIC_API_KEY for the dream drill")
    if not device.ping():
        pytest.skip("console not answering")
    return device


@pytest.mark.net
def test_mount_state_is_reported_and_the_drill_does_not_restart(orch: Device, net):
    ip = _status(orch)["ip"]
    st0 = net.get_json("/api/state", ip=ip, timeout=8.0)
    assert st0.get("fsMounted") is True, f"internal storage not mounted before the drill: {st0.get('fsMounted')}"
    assert _health_row(net.get_json("/api/health", ip=ip, timeout=8.0), "flash")["state"] == "ok"
    orch.cmd("FAULT provider on", "FAULT provider=1", timeout=5.0)
    try:
        _drill(orch)
    finally:
        orch.cmd("FAULT clear", "FAULT cleared", timeout=5.0)
    st1 = net.get_json("/api/state", ip=ip, timeout=8.0)
    assert st1.get("fsMounted") is True
    assert st1["uptimeMs"] > st0["uptimeMs"], "the device restarted during the drill"
    assert st1.get("testWake") == st0.get("testWake"), "the reset reason changed: the device restarted"
    night = st1.get("dream", {})
    assert night.get("known") is True, night
    assert night.get("last") == "failed", night
    assert night.get("stateSaved") is True, night
    assert _health_row(net.get_json("/api/health", ip=ip, timeout=8.0), "flash")["state"] == "ok"


def test_failed_quiet_dream_sends_nothing_and_says_failed(orch: Device):
    orch.cmd("FAULT provider on", "FAULT provider=1", timeout=5.0)
    try:
        window = _drill(orch)
    finally:
        orch.cmd("FAULT clear", "FAULT cleared", timeout=5.0)
    assert "quiet turn: notice held" in window, "the failure notice was not held"
    assert "telegram: send" not in window, "a failed quiet dream sent a Telegram message"
    rows = _dream_rows(orch)
    assert rows, "no ev:dream row was written"
    newest = rows[0]
    assert newest.startswith("Nightly dream FAILED ("), newest
    assert "memories stored" in newest and "failed)" in newest, newest  # the counts
    # A retry only inside the 03:30-07:00 window and while tries remain that local day
    # (earlier drills count); a daytime drill says "no retry tonight".
    assert re.search(r"retry [23] of 3 at|no retry tonight", newest), newest
    assert "state not saved" not in newest, newest


def test_failed_writes_make_the_night_degraded_with_counts(orch: Device):
    orch.turn(
        "Please remember for later: my sister Anna's birthday is on 3 March, and I work from home on Fridays now."
    )
    # The owner turn must END before memory fails: "orchestrator: turn" also matches
    # the turn's start line, so the fault used to land mid-turn and the night queued
    # behind a turn whose memory tools kept failing (the 240 s wait ran out).
    orch.expect("turn end:", timeout=180.0)
    orch.drain(quiet=2.0)
    orch.cmd("FAULT memory on", "FAULT memory=1", timeout=5.0)
    try:
        window = _drill(orch, timeout=480.0)  # every memory write of the night fails and is retried by the model
    finally:
        orch.cmd("FAULT clear", "FAULT cleared", timeout=5.0)
    assert "telegram: send" not in window, "the dream messaged the owner"
    rows = _dream_rows(orch)
    assert rows, "no ev:dream row was written"
    newest = rows[0]
    assert not newest.startswith("Nightly dream ok"), newest
    if newest.startswith("Nightly dream degraded"):
        m = re.search(r"(\d+) failed\)", newest)
        assert m and int(m.group(1)) >= 1, newest
    else:
        # The model chose not to write (allowed on a short day): the row still
        # reports the counts and is not "ok" with writes that failed.
        assert "memories stored" in newest, newest


def test_dream_writes_no_nvs(orch: Device):
    before = _status(orch)
    orch.cmd("FAULT provider on", "FAULT provider=1", timeout=5.0)
    try:
        _drill(orch)
    finally:
        orch.cmd("FAULT clear", "FAULT cleared", timeout=5.0)
    after = _status(orch)
    assert (after["nvs"], after["nvsfail"]) == (before["nvs"], before["nvsfail"]), (
        "the dream path wrote NVS (used bytes or failed writes moved)"
    )


_ARMED_PANIC = "Guru Meditation"  # the armed null store, as the panic handler prints it
_ROM_RESET = "rst:"  # the ROM banner of the boot after it


def _await_armed_panic_reboot(dev: Device, panic_timeout: float = 240.0) -> str:
    """Wait out the panic this leg armed and the reboot after it; returns the panic line.

    The panic handler prints its report ("Guru Meditation ...", the backtrace,
    "Rebooting...") before the reset. wait_ready fails on any panic marker, so it is
    only called once the ROM's ``rst:`` banner arrived: called straight after DREAM,
    it read the armed panic as a boot failure (unit L, 21 Oct: "panic during boot"
    while the next boot came up and ran). A drill that ends with no panic fails here,
    so the leg cannot pass on a turn the arm never reached."""
    line = dev.expect(_ARMED_PANIC, timeout=panic_timeout)
    try:
        dev.expect(_ROM_RESET, timeout=20.0)
    except Exception:  # noqa: BLE001 - the USB endpoint dropped at the reset (re-enumerated)
        dev.reopen_after_reenumerate(drain_boot=False)
    dev.wait_ready(timeout=90.0)
    return line


def _arm_turn_panic(dev: Device) -> None:
    """Arm the mid-turn panic (lane O1's hook); skip when the build lacks it."""
    try:
        armed = dev.cmd("PANIC nullptr arm=turn", "arm", timeout=5.0)
    except ExpectTimeout:
        pytest.skip("PANIC arm=turn hook not wired yet (lane O1)")
    if "not wired" in armed:
        pytest.skip("PANIC arm=turn hook not wired yet (lane O1)")


@pytest.mark.net
def test_interrupted_dream_is_reconciled_at_the_next_boot(orch: Device, net):
    """L19-31: a DREAM drill the restart cuts short leaves an in-flight marker.
    The next boot reconciles it: one ``Nightly dream interrupted`` row (never the
    previous night's verdict), the dream reports fail with "the device restarted
    during this run", and the routine's next run retries the same night while
    the night's window is still open (a daytime drill keeps the next slot)."""
    _arm_turn_panic(orch)
    ip = _status(orch)["ip"]
    orch.drain(quiet=0.5)
    orch.send("DREAM")
    orch.expect("DREAM queued", timeout=5.0)
    # The armed panic fires deep in the reflection turn; the device restarts.
    panic = _await_armed_panic_reboot(orch)
    assert "StoreProhibited" in panic, f"not the armed null store: {panic}"
    # The boot reconcile waits for the synced clock; poll for its verdict.
    night = {}
    deadline = time.time() + 120.0
    while time.time() < deadline:
        night = net.get_json("/api/state", ip=ip, timeout=8.0).get("dream", {})
        if night.get("known") and night.get("cause") == "restarted":
            break
        time.sleep(3.0)
    assert night.get("known") is True, f"no dream verdict after the restart: {night}"
    assert night.get("last") == "failed", night
    assert night.get("cause") == "restarted", night
    assert night.get("err") == "the device restarted during this run", night
    assert night.get("stateSaved") is True, night
    rows = _dream_rows(orch)
    newest = rows[0] if rows else ""
    assert newest.startswith("Nightly dream interrupted (the device restarted during the reflection)"), (
        f"no interrupted row in the timeline: {rows}"
    )
    # The loop record: fail, and a next run that honors the night's window.
    loops = net.get_json("/api/loops", ip=ip, timeout=8.0)
    dream = next((lp for lp in loops if lp.get("id") == "dream"), None)
    assert dream is not None, "the reserved dream routine is missing from /api/loops"
    assert dream.get("lastResult") == "fail", dream
    now = int(net.get_json("/api/state", ip=ip, timeout=8.0).get("clock", {}).get("epoch", 0))
    retry = re.search(r"retry (\d) of 3 at (\d{2}):(\d{2})", newest)
    if retry:
        assert 2 <= int(retry.group(1)) <= 3, newest
        assert 0 < int(dream.get("nextRun", 0)) - now <= 900 + 120, (
            f"a retry was promised but nextRun is not tonight's retry: {dream}"
        )
    else:
        assert "no retry tonight" in newest, newest
        assert int(dream.get("nextRun", 0)) > now, f"the window is closed but the routine is due at once: {dream}"
