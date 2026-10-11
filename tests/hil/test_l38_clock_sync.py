"""L38 - the device clock syncs within 60 s of READY on a stored-credential boot (CUM-504).

The bug: SNTP was started only from a WiFi.onEvent(GOT_IP) handler that setup()
registered about 10 s after net::begin() had started the station on its stored
credentials. On a strong network the join finished first, GOT_IP fired with nobody
listening, and SNTP never started: the clock stayed at 1970 for the whole uptime, no
daily or weekly routine fired and every prompt said the date-time was UNKNOWN. The fix
(nimbus/wifi/net_up.h, host-tested in test/test_net_up) derives network-up from the
live station state on every main-loop tick, so a station that is already up when the
loop starts kicks SNTP on the first tick.

Why ``REBOOT cold``
-------------------
A plain console REBOOT carries the RTC-held wall clock across the software reset, so a
unit that ever synced boots with a valid clock and this leg would pass on the buggy
firmware. ``REBOOT cold`` (test console, NIMBUS_TEST only) clears the clock and then
restarts, so the boot starts unsynced exactly like a power-on. The power cycle itself
stays a manual leg at the bottom.

The automatable class (asserted here over serial)
-------------------------------------------------
Orchestrator mode, Wi-Fi credentials stored (the fixture provisions them if the unit is
not on the LAN), ``REBOOT cold``, then from the READY beacon:

* the boot log shows ``loops: begin ... clock=0`` (the boot really started unsynced; a
  missing line fails, it never passes by default);
* STATUS ``clk=`` (the strict "clock is real" test /api/state ``clock.synced`` uses)
  reads 1 within SYNC_BUDGET_S;
* the boot logs ``loops: SNTP kicked (tz=..., link up)``: the network-up latch fired,
  so the pass is not borrowed from a later reconnect.

The CUM-504 precondition is the station already holding an IP at READY (the READY
beacon carries ``ip=``): that is the fast join whose GOT_IP fired before the old handler
existed. Bench B (rssi about -45 dBm) hits it on every boot; the test prints whether
this boot did, so the evidence says which ordering was exercised.

The LAN leg (``net``) then checks /api/state ``clock.synced`` and that the reserved
dream routine got a computed wall-clock ``nextRun`` (no ``no-clock``).

The parsers and the verdict are host-tested in test_l38_clock_sync_host.py.
"""

from __future__ import annotations

import re
import time
from dataclasses import dataclass, field
from typing import List, Optional, Tuple

import pytest

from device import ExpectTimeout

# The CUM-504 bar: READY -> wall clock real. A kick lands on the first loop tick and an
# NTP answer normally takes a few seconds; the budget also covers the post-boot SD scan
# that can hold the console (and the loop) for a while on a card with a large history.
SYNC_BUDGET_S = 60.0

# After the clock is real, routines are rebased on the loops sweep (every 20 s, on the
# turn task, which can sit in a Telegram long-poll for up to ~30 s).
ROUTINE_BUDGET_S = 90.0

STATUS_EVERY_S = 2.0

READY_RE = re.compile(r"READY\s+mode=(?P<mode>\d+)\s+ip=(?P<ip>\S+)")
STATUS_RE = re.compile(r"^STATUS\s")
CLK_RE = re.compile(r"\bclk=(?P<clk>[01])\b")
WIFI_RE = re.compile(r"\bwifi=(?P<wifi>\d)\b")
# A POSIX TZ can itself hold commas ("CET-1CEST,M3.5.0,M10.5.0/3"), never ", " or ")":
# the reason is the trailing ", <words>" before the closing parenthesis.
KICK_RE = re.compile(r"loops: SNTP kicked \(tz=(?P<tz>[^)]*?)(?:, (?P<why>[a-z][a-z ]*))?\)")
LOOPS_BEGIN_RE = re.compile(r"loops: begin - \d+ loop\(s\), clock=(?P<clk>[01])")

DREAM_ID = "dream"


def parse_status_clock(line: str) -> Tuple[int, int]:
    """``STATUS ... wifi=1 ... clk=1`` -> (wifi, clk). Raises ValueError on a non-STATUS
    line or a STATUS line with no ``clk=`` (an image older than CUM-504), so a renamed
    or missing field fails loud instead of passing vacuously."""
    if not STATUS_RE.search(line):
        raise ValueError(f"not a STATUS line: {line!r}")
    clk = CLK_RE.search(line)
    wifi = WIFI_RE.search(line)
    if not clk or not wifi:
        raise ValueError(f"STATUS without wifi=/clk= (pre-CUM-504 image?): {line!r}")
    return int(wifi.group("wifi")), int(clk.group("clk"))


def parse_kick(line: str) -> Optional[str]:
    """``loops: SNTP kicked (tz=UTC0, link up)`` -> "link up". A pre-CUM-504 kick line
    (no reason) returns "", any other line None."""
    m = KICK_RE.search(line)
    if not m:
        return None
    return m.group("why") or ""


def ready_ip_is_lan(ip: Optional[str]) -> bool:
    """The READY beacon's ip= is a real station address (the station joined during
    setup, the CUM-504 precondition), not the unjoined 0.0.0.0 / absent."""
    return bool(ip) and ip not in ("0.0.0.0", "-", "none")


@dataclass
class BootClock:
    """What one boot showed. Times are seconds from the READY beacon."""

    up_at_ready: bool = False
    boot_clock: Optional[int] = None  # loops: begin ... clock=<n>, None if not seen
    status_seen: bool = False
    status_with_clk: bool = False  # at least one whole STATUS line carried clk=
    synced_s: Optional[float] = None
    link_up_kick_s: Optional[float] = None
    kicks: List[Tuple[float, str]] = field(default_factory=list)


def fold(boot_lines: List[str], timeline: List[Tuple[float, str]], up_at_ready: bool) -> BootClock:
    """Fold the boot stream (before READY) and the post-READY (seconds, line) timeline
    into a BootClock. Pure, so the verdict is host-testable."""
    bc = BootClock(up_at_ready=up_at_ready)
    for line in boot_lines:
        m = LOOPS_BEGIN_RE.search(line)
        if m:
            bc.boot_clock = int(m.group("clk"))
    for t, line in timeline:
        why = parse_kick(line)
        if why is not None:
            bc.kicks.append((t, why))
            if why == "link up" and bc.link_up_kick_s is None:
                bc.link_up_kick_s = t
            continue
        if not STATUS_RE.search(line):
            continue
        bc.status_seen = True
        try:
            _wifi, clk = parse_status_clock(line)
        except ValueError:
            continue  # a line cut short by a read timeout; the next poll answers again
        bc.status_with_clk = True
        if clk == 1 and bc.synced_s is None:
            bc.synced_s = t
    return bc


def verdict(bc: BootClock, budget_s: float = SYNC_BUDGET_S) -> List[str]:
    """Every reason this boot fails the CUM-504 bar ([] = pass)."""
    problems: List[str] = []
    if bc.boot_clock is None:
        problems.append("no 'loops: begin ... clock=' line before READY: cannot confirm the boot started unsynced")
    elif bc.boot_clock == 1:
        problems.append("the boot started with a valid clock (REBOOT cold did not clear it): the leg would be vacuous")
    if not bc.status_seen:
        problems.append("the console never answered STATUS after READY")
    elif not bc.status_with_clk:
        problems.append("STATUS has no clk= field: this image predates CUM-504")
    if bc.synced_s is None:
        problems.append(f"the clock never synced within {budget_s:.0f} s of READY (CUM-504)")
    elif bc.synced_s > budget_s:
        problems.append(f"the clock synced {bc.synced_s:.1f} s after READY, over the {budget_s:.0f} s bar")
    if bc.link_up_kick_s is None:
        problems.append(
            "no 'loops: SNTP kicked (..., link up)' after READY: the network-up latch never fired"
            + (f" (kicks seen: {bc.kicks})" if bc.kicks else "")
        )
    return problems


# ---- device plumbing -------------------------------------------------------------------


@pytest.fixture
def stored_creds_device(device, net, secrets, require_secret):
    """Orchestrator mode with Wi-Fi credentials STORED (the boot under test joins from
    them on its own). Provisions through the console only when the unit is not on the
    LAN yet; provisioning persists the network."""
    require_secret(secrets.require_sta)
    device.ensure_mode(1)
    st = device.status(timeout=8.0)
    if st.group("wifi") != "1":
        net.provision(secrets.sta_ssid, secrets.sta_pass)
        net.wait_got_ip(timeout=25.0)
    return device


def _read_until(device, deadline: float, sink: List[str], pattern: "re.Pattern[str]") -> Optional[str]:
    """Read serial lines into ``sink`` until one matches ``pattern`` (returned) or the
    deadline passes (None)."""
    while True:
        left = deadline - time.time()
        if left <= 0:
            return None
        try:
            line = device.expect_re(r".+", timeout=left).group(0)
        except ExpectTimeout:
            return None
        sink.append(line)
        if pattern.search(line):
            return line


def boot_to_ready(device, timeout: float = 120.0) -> Tuple[List[str], Optional[str], float]:
    """Read a fresh boot stream to READY. Returns (boot lines, READY ip, READY wall time).
    Fails loud on a missing beacon rather than guessing."""
    boot: List[str] = []
    ready = _read_until(device, time.time() + timeout, boot, READY_RE)
    if ready is None:
        pytest.fail(f"no READY beacon within {timeout:.0f} s of the restart; tail: {boot[-15:]}")
    return boot, READY_RE.search(ready).group("ip"), time.time()


def cold_boot(device, timeout: float = 120.0) -> Tuple[List[str], Optional[str], float]:
    """``REBOOT cold``, then the boot stream to READY (see boot_to_ready)."""
    device.cmd("REBOOT cold", "REBOOTING cold", timeout=5.0)
    device.reopen_after_reenumerate(drain_boot=False)  # keep the boot stream
    return boot_to_ready(device, timeout)


def watch_clock(device, t_ready: float, budget_s: float = SYNC_BUDGET_S) -> List[Tuple[float, str]]:
    """Poll STATUS every few seconds from READY until the clock is real AND the link-up
    kick has been seen, or the budget (+ one poll) runs out. Records every serial line
    with its offset from READY, kick lines included."""
    timeline: List[Tuple[float, str]] = []
    synced = kicked = False
    end = t_ready + budget_s + STATUS_EVERY_S
    while time.time() < end and not (synced and kicked):
        device.send("STATUS")
        got: List[str] = []
        line = _read_until(device, min(time.time() + 6.0, end), got, STATUS_RE)
        for g in got:
            timeline.append((time.time() - t_ready, g))
            kicked = kicked or parse_kick(g) == "link up"
        if line is not None:
            try:
                synced = parse_status_clock(line)[1] == 1
            except ValueError:
                pass  # cut short or no clk= field: fold() tells the two apart
        time.sleep(STATUS_EVERY_S)
    return timeline


@pytest.mark.hil
def test_clock_syncs_within_60s_of_ready(stored_creds_device):
    device = stored_creds_device
    boot, ready_ip, t_ready = cold_boot(device)
    timeline = watch_clock(device, t_ready)
    bc = fold(boot, timeline, up_at_ready=ready_ip_is_lan(ready_ip))
    print(
        f"\n[L38] READY ip={ready_ip} (CUM-504 precondition, station up at READY: {bc.up_at_ready}); "
        f"boot clock={bc.boot_clock}; link-up kick at {bc.link_up_kick_s} s; synced at {bc.synced_s} s; "
        f"kicks={bc.kicks}"
    )
    problems = verdict(bc)
    assert not problems, "; ".join(problems) + f"; timeline tail: {timeline[-10:]}"


def lan_ip_after_ready(device, ready_ip: Optional[str], t_ready: float) -> str:
    """The station's LAN address: from the READY beacon on a fast join, else from STATUS
    once the stored-credential join lands (bounded by the sync budget)."""
    if ready_ip_is_lan(ready_ip):
        return ready_ip
    deadline = t_ready + SYNC_BUDGET_S
    while time.time() < deadline:
        st = device.status(timeout=6.0)
        if st.group("wifi") == "1" and ready_ip_is_lan(st.group("ip")):
            return st.group("ip")
        time.sleep(STATUS_EVERY_S)
    pytest.fail(f"the station never joined from its stored credentials within {SYNC_BUDGET_S:.0f} s of READY")


def seconds_until_state_synced(net, ip: str, t_ready: float) -> Optional[float]:
    """Poll /api/state until clock.synced is true; seconds from READY, or None."""
    deadline = t_ready + SYNC_BUDGET_S
    while time.time() < deadline:
        try:
            if net.get_json("/api/state", ip=ip, timeout=5.0).get("clock", {}).get("synced") is True:
                return time.time() - t_ready
        except Exception:  # noqa: BLE001 - web server still binding; retry
            pass
        time.sleep(STATUS_EVERY_S)
    return None


def dream_routine_after_sync(net, ip: str) -> Optional[dict]:
    """Poll /api/loops until the reserved dream routine has a wall-clock next run (the
    loops sweep rebases it after the sync), returning its last-seen record."""
    dream = None
    deadline = time.time() + ROUTINE_BUDGET_S
    while time.time() < deadline:
        loops = net.get_json("/api/loops", ip=ip, timeout=8.0)
        dream = next((lp for lp in loops if lp.get("id") == DREAM_ID), None)
        if dream and int(dream.get("nextRun", 0)) > 0 and dream.get("whyCode") != "no-clock":
            return dream
        time.sleep(5.0)
    return dream


@pytest.mark.hil
@pytest.mark.net
def test_routine_schedule_computed_after_boot_sync(stored_creds_device, net):
    """LAN half: after a cold boot the web state reports a synced clock and the
    reserved dream routine has a real wall-clock next run (not 'no-clock')."""
    _boot, ready_ip, t_ready = cold_boot(stored_creds_device)
    ip = lan_ip_after_ready(stored_creds_device, ready_ip, t_ready)
    synced_at = seconds_until_state_synced(net, ip, t_ready)
    assert synced_at is not None, f"/api/state clock.synced never went true within {SYNC_BUDGET_S:.0f} s of READY"
    dream = dream_routine_after_sync(net, ip)
    assert dream is not None, "the reserved dream routine is missing from /api/loops"
    assert int(dream.get("nextRun", 0)) > 0, f"dream routine never got a wall-clock next run: {dream}"
    assert dream.get("whyCode") != "no-clock", f"dream routine still waits for the clock: {dream}"
    print(f"\n[L38] synced {synced_at:.1f} s after READY; dream nextRun={dream['nextRun']}")


@pytest.mark.hil
@pytest.mark.manual
def test_clock_syncs_after_a_real_power_cycle(stored_creds_device, require_manual):
    """Owner leg: a true power-on (no RTC carry-over at all), the exact bench-B report."""
    device = stored_creds_device
    require_manual.confirm(
        "CLOCK AFTER A COLD BOOT (CUM-504):\n"
        "  1. Remove ALL power from the unit now (USB and battery) and wait 10 s.\n"
        "  2. Answer y while it is still unpowered, then restore power within 30 s.\n"
        "  3. Do not open the web page or press Sync now.\n"
        "Is the unit unpowered now?"
    )
    saved = device.enumerate_timeout
    device.enumerate_timeout = max(saved, 60.0)  # the human restores power after the y
    try:
        device.reopen_after_reenumerate(drain_boot=False)
    finally:
        device.enumerate_timeout = saved
    boot, ready_ip, t_ready = boot_to_ready(device)
    bc = fold(boot, watch_clock(device, t_ready), up_at_ready=ready_ip_is_lan(ready_ip))
    print(f"\n[L38 power cycle] up at READY={bc.up_at_ready} synced at {bc.synced_s} s kicks={bc.kicks}")
    problems = verdict(bc)
    assert not problems, "; ".join(problems)
