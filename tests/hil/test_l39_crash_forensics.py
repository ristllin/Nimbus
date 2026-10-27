"""L39 - every crash leaves a durable, readable cause (crash forensics, panic F0).

The 18 Oct panic on a field unit left one fact: ``reason=crash(panic)``. No task, PC,
backtrace or panic string, because the firmware wrote an ELF core dump to flash on every
panic and never read it, lost the first six durable lines of every boot, and never put
the reset reason in the durable log. This suite proves, on a real board, the class rule:
**every crash kind leaves a boot record and a crash report in the durable log, read on
the next boot, deduped, and the core dump stays in flash for a bench pull.**

The boot record and crash lines (src/sys/crash_report.cpp; formats in lib/core
``nimbus/sys/boot_record.h`` and ``nimbus/crash_summary.h``)::

    boot: n=17 fw=v4.5.14 build=... reason=crash(panic)(0x0c) wake=none mode=orch t=1792336105 prev=none
    crash: fp=1a2b3c4d new=1 this-reset=1 task=loopTask pc=0x4037... cause=29(StoreProhibited) ... size=23456/65536 elf=0badf00d1 fw-match=1
    crash-bt: depth=8 corrupt=0 0x...,0x...
    crash-why: abort() was called at PC 0x... on core 0

Legs (``hil`` + ``net``: serial for the console verbs, the LAN for ``GET /api/errlog``
and ``/api/state``; the two destructive legs carry only ``destructive``, so ``-m hil`` and
``-m "net and not manual"`` never factory-reset the board or roll back its firmware):

* each PANIC kind: the boot line has the crash slug and the raw ROM code; ``crash:`` has
  ``new=1 this-reset=1`` and the expected task; ``crash-why`` is not empty; the image
  fits the 64 KB partition; ``/api/state`` carries the same summary;
* a plain reboot after it: ``new=0 this-reset=0`` (stale, one line);
* seq 0 exists in every boot (no pre-mount hole);
* a host USB-CDC reset reads ``reason=usb(0x15)`` (it read ``unknown`` before);
* offline boots keep their timeline rows (pre-sync events survive resets): every earlier
  offline boot's row says "from boot n=<k>, clock never synced", and the boot that
  syncs labels its own row "[pre-sync, stamped at first clock sync]";
* live: a panic at the transport write during a real turn proves the dump fits 64 KB
  with tg_poll deep in a TLS exchange and names the transport frame (the panic lands
  before any request byte is sent, so the provider bills nothing);
* the RTC breadcrumb: after a mid-turn panic the boot line's ``prev=`` names the turn;
* destructive: a factory reset erases the dump; an ``OTASIM crash`` early-boot abort is
  explained by the rolled-back boot (its ``prev=`` names the OTA guard stage).

The PANIC verbs are the test console's (NIMBUS_TEST only): ``PANIC abort|nullptr|canary|
twdt|oom`` crash at once on the console task; ``PANIC <nullptr|abort|canary> arm=turn
[round=N]`` arms a panic that fires on tg_poll at the transport body write in round N of
the next turn (``nimbus::crash::armTurnPanic``, dropped if that turn ends first). The parsers and
verdicts are host-tested in test_l39_crash_forensics_host.py.

This suite replaces the first skeleton of L39 and keeps its oracle: the boot line names
the reset class and the raw code, ``crash:`` reads ``new=1 this-reset=1`` with the image
under 64 KB and a non-empty ``crash-why``, a plain reboot reads ``new=0``, every boot has
seq 0, and the mid-turn dump lands on tg_poll (``crash_verdict``, ``stale_verdict``,
``every_boot_starts_at_seq0``).
"""

from __future__ import annotations

import os
import re
import time
from typing import Dict, List, Optional

import pytest

from device import ExpectTimeout

try:  # the LAN client's transport; a connect or read failure while the board rejoins
    import requests

    # A streamed page the board cut mid-body is retried too: a card download waits out a
    # tool's hold of the card, but one whose card waits pass 60 s is cut rather than
    # ended short (nimbus/net/card_stream.h); firmware without card streams also ended
    # them early on a short hold.
    TRANSIENT_NET_ERRORS: tuple = (
        requests.exceptions.ConnectionError,
        requests.exceptions.Timeout,
        requests.exceptions.ChunkedEncodingError,
    )
except ImportError:  # pragma: no cover - requests ships with the HIL environment
    TRANSIENT_NET_ERRORS = (OSError,)

PARTITION_BYTES = 65536

LINE_RE = re.compile(r"^(?P<seq>\d+) (?P<ms>\d+) (?P<cat>\S+) (?P<msg>.*)$")
BOOT_RE = re.compile(
    r"^boot: n=(?P<n>\d+|\?) fw=(?P<fw>\S+) build=(?P<build>\S+) "
    r"reason=(?P<slug>\S+)\(0x(?P<raw>[0-9a-f]{2,8})\) wake=(?P<wake>\S+) "
    r"mode=(?P<mode>orch|notifier) t=(?P<t>\d+)(?: ota=(?P<ota>\S+))? prev=(?P<prev>\S+)$"
)
CRASH_RE = re.compile(
    r"^crash: fp=(?P<fp>[0-9a-f]{8}) new=(?P<new>[01?]) this-reset=(?P<this>[01]) "
    r"task=(?P<task>\S+) pc=0x(?P<pc>[0-9a-f]{8}) cause=(?P<cause>\d+)\((?P<cname>\w+)\) "
    r"vaddr=0x(?P<vaddr>[0-9a-f]{8}) size=(?P<size>\d+)/(?P<part>\d+) "
    r"elf=(?P<elf>\S+) fw-match=(?P<fwm>[01])$"
)
CRASH_BAD_RE = re.compile(
    r"^crash: image=(?P<state>[a-z-]+) fp=(?P<fp>[0-9a-f]{8}) new=(?P<new>[01?]) "
    r"this-reset=(?P<this>[01]) size=(?P<size>\d+)/(?P<part>\d+)$"
)
BT_RE = re.compile(r"^crash-bt: depth=(?P<depth>\d+) corrupt=(?P<corrupt>[01])(?: (?P<pcs>\S+))?$")
WHY_RE = re.compile(r"^crash-why: (?P<why>.+)$")

# The immediate PANIC kinds run on the console's task (the Arduino loop task). A
# watchdog panic records the task the interrupt preempted, normally that spinning task.
IMMEDIATE_KINDS = ("abort", "nullptr", "canary", "twdt", "oom")
# `PANIC <kind> arm=turn [round=N]` fires on the Telegram poll task inside a turn.
ARMED_KINDS = ("nullptr", "abort", "canary")
LOOP_TASKS = ("loopTask",)
WDT_TASKS = ("loopTask", "IDLE0", "IDLE1")
TURN_TASKS = ("tg_poll",)


def kind_slug(kind: str) -> str:
    """The boot-line slug a PANIC kind produces (ev:boot vocabulary)."""
    return "crash(watchdog)" if kind == "twdt" else "crash(panic)"


def kind_tasks(kind: str, armed: bool = False) -> tuple:
    if armed:
        return TURN_TASKS
    return WDT_TASKS if kind == "twdt" else LOOP_TASKS


# ---- pure parsers (host-tested) ----------------------------------------------
def parse_log(text: str) -> List[Dict[str, str]]:
    """Durable log text -> [{seq, ms, cat, msg}] (malformed lines skipped)."""
    out = []
    for raw in text.splitlines():
        m = LINE_RE.match(raw.strip())
        if m:
            out.append(
                {"seq": int(m.group("seq")), "ms": int(m.group("ms")), "cat": m.group("cat"), "msg": m.group("msg")}
            )
    return out


def split_boots(lines: List[Dict[str, str]]) -> List[List[Dict[str, str]]]:
    """Split parsed lines into boots at each ``boot:`` record. Lines before the first
    record (an older firmware, or a file that rotated mid-boot) form no boot."""
    boots: List[List[Dict[str, str]]] = []
    for ln in lines:
        if ln["msg"].startswith("boot: "):
            boots.append([ln])
        elif boots:
            boots[-1].append(ln)
    return boots


def boot_report(section: List[Dict[str, str]]) -> dict:
    """One boot's forensic facts: the boot record, the crash line(s), seq of the record."""
    first = section[0]
    bm = BOOT_RE.match(first["msg"])
    if not bm:
        raise ValueError(f"not a boot record: {first['msg']!r}")
    rep = {"seq0": first["seq"], "boot": bm.groupdict(), "crash": None, "bad": None, "bt": None, "why": None}
    for ln in section[1:]:
        msg = ln["msg"]
        if rep["crash"] is None and (m := CRASH_RE.match(msg)):
            rep["crash"] = m.groupdict()
        elif rep["bad"] is None and (m := CRASH_BAD_RE.match(msg)):
            rep["bad"] = m.groupdict()
        elif rep["bt"] is None and (m := BT_RE.match(msg)):
            rep["bt"] = m.groupdict()
        elif rep["why"] is None and (m := WHY_RE.match(msg)):
            rep["why"] = m.group("why")
    return rep


def crash_verdict(rep: dict, kind: str, armed: bool = False) -> "tuple[bool, str]":
    """The per-kind class rule for the boot right after a PANIC <kind> (``armed``: the
    arm=turn form, which fires on the turn task)."""
    slug, tasks = kind_slug(kind), kind_tasks(kind, armed)
    b, c = rep["boot"], rep["crash"]
    if rep["seq0"] != 0:
        return False, f"boot record is seq {rep['seq0']}, not 0 (pre-mount hole)"
    if b["slug"] != slug:
        return False, f"boot reason {b['slug']!r}, want {slug!r}"
    if c is None:
        return False, "no crash: line after a crash boot"
    if (c["new"], c["this"]) != ("1", "1"):
        return False, f"crash: new={c['new']} this-reset={c['this']}, want new=1 this-reset=1"
    if c["task"] not in tasks:
        return False, f"crash task {c['task']!r}, want one of {tasks}"
    if int(c["size"]) >= PARTITION_BYTES or int(c["part"]) != PARTITION_BYTES:
        return False, f"image size {c['size']}/{c['part']} does not fit the 64 KB partition"
    if not rep["why"] or rep["why"] == "(no reason recorded)":
        return False, "crash-why is empty"
    if rep["bt"] is None:
        return False, "no crash-bt: line"
    return True, "ok"


def stale_verdict(rep: dict) -> "tuple[bool, str]":
    """A plain reboot with an already-reported dump: one stale line, no repeat."""
    c = rep["crash"]
    if c is None:
        return False, "the stored dump was not mentioned (crash: line missing)"
    if (c["new"], c["this"]) != ("0", "0"):
        return False, f"crash: new={c['new']} this-reset={c['this']}, want new=0 this-reset=0"
    if rep["bt"] is not None or rep["why"] is not None:
        return False, "a stale dump repeated its backtrace/reason lines"
    return True, "ok"


def every_boot_starts_at_seq0(boots: List[List[Dict[str, str]]]) -> "tuple[bool, str]":
    for i, sec in enumerate(boots):
        if sec[0]["seq"] != 0:
            return False, f"boot {i} record is seq {sec[0]['seq']}"
    return True, f"{len(boots)} boot(s), all start at seq 0"


def boot_number(rep: dict) -> Optional[int]:
    """The boot record's n= (None for ``n=?``, a unit with no storage counter)."""
    n = rep["boot"]["n"]
    return int(n) if n.isdigit() else None


def first_boot_after(boots: List[List[Dict[str, str]]], before_n: Optional[int]) -> "tuple[Optional[dict], List[str]]":
    """The boot a crash produced: the FIRST boot numbered above ``before_n``, plus the
    reset slugs of any boots after it.

    On the S3's native USB a host serial reopen resets the chip (reason ``usb``). When
    the harness has to reopen after a crash, that reset adds a boot the firmware did not
    cause, and the newest boot then reads ``usb`` with the dump already reported
    (``new=0``). The crash record is the first new boot, never simply the newest one.
    With no boot number to compare (``before_n`` None) the newest boot is the answer."""
    reps = []
    for sec in boots:
        try:
            reps.append(boot_report(sec))
        except ValueError:
            continue  # a boot line from another firmware: not ours to judge
    if before_n is None:
        return (reps[-1] if reps else None), []
    newer = [r for r in reps if (boot_number(r) or -1) > before_n]
    if not newer:
        return None, []
    return newer[0], [r["boot"]["slug"] for r in newer[1:]]


def latest_turn_crash(boots: List[List[Dict[str, str]]]) -> Optional[dict]:
    """The newest boot whose crash line is a fresh crash on the turn task (a mid-turn
    panic), skipping boots the harness caused after it."""
    for sec in reversed(boots):
        try:
            rep = boot_report(sec)
        except ValueError:
            continue
        c = rep["crash"]
        if c and c["this"] == "1" and c["task"] in TURN_TASKS:
            return rep
    return None


def poll_until(fetch, done, budget_s: float, interval_s: float = 5.0, clock=time.time, sleep=time.sleep):
    """Call ``fetch()`` until ``done(value)`` holds or ``budget_s`` runs out; returns
    ``(last value or None, last transient error or None)``.

    A connect or read failure inside the budget is a board that is rejoining Wi-Fi or
    rebooting, not a verdict, so it is retried; any other exception propagates."""
    deadline = clock() + budget_s
    last, err = None, None
    while True:
        try:
            last, err = fetch(), None
            if done(last):
                return last, None
        except TRANSIENT_NET_ERRORS as exc:
            err = exc
        if clock() >= deadline:
            return last, err
        sleep(interval_s)


# The active durable log file and the one rotated before it (src/sys/errlog.h).
ACTIVE_LOG = "nimbus.log"
PREVIOUS_LOG = "nimbus.log.1"
# A busy answer (503: internal memory short, e.g. right after a crash boot while the
# TLS sessions come up) is the device protecting itself, not an empty log: retried.
ERRLOG_BUSY_BUDGET_S = 45.0
ERRLOG_RETRY_S = 3.0


def starts_mid_boot(text: str) -> bool:
    """True when a log file does not begin with a boot record: it rotated inside a
    boot, so that boot's record (and its crash lines) sit in the previous file."""
    lines = parse_log(text)
    return not lines or not lines[0]["msg"].startswith("boot: ")


def stitch_rotated(previous: str, active: str) -> str:
    """The durable log as one text: the active file alone when it starts with a boot
    record, else the previous rotated file first (the boot the rotation split)."""
    if not starts_mid_boot(active) or not previous:
        return active
    return (previous if previous.endswith("\n") else previous + "\n") + active


# Pre-sync timeline rows (nimbus/orch/pending_events.h stampedPendingText): a row from
# an earlier boot names that boot; the boot that syncs keeps the plain suffix on its own.
NEVER_SYNCED_RE = re.compile(r"\[from boot n=(\d+), clock never synced")
OWN_PRESYNC = "[pre-sync, stamped at first clock sync]"
# Cold boots the offline leg makes; the last one is the boot that syncs.
OFFLINE_BOOTS = 3


def offline_rows_verdict(page: dict, seen_ids: set, earlier_boots: int) -> "tuple[bool, str]":
    """The class rule over the boot rows the first sync wrote (rows not in
    ``seen_ids``): ``earlier_boots`` distinct earlier offline boots each have a "clock
    never synced" row, and the syncing boot has its own pre-sync row."""
    new = [
        str(m.get("text") or "")
        for m in page.get("messages", [])
        if str(m.get("id")) not in seen_ids and str(m.get("text") or "").startswith("Booted ")
    ]
    earlier = sorted({int(n) for text in new for n in NEVER_SYNCED_RE.findall(text)})
    if len(earlier) < earlier_boots:
        return (
            False,
            f"{len(earlier)} earlier offline boot(s) {earlier} reached the timeline, want {earlier_boots}: {new}",
        )
    if not any(OWN_PRESYNC in text for text in new):
        return False, f"the boot that synced has no row of its own: {new}"
    return True, f"earlier offline boots {earlier} and the syncing boot's own row"


# ---- device helpers --------------------------------------------------------------
def _lan_ip(ip_from_beacon: Optional[str]) -> str:
    ip = os.environ.get("NIMBUS_TEST_IP") or ip_from_beacon
    if not ip or ip == "0.0.0.0":
        pytest.skip("L39 reads /api/errlog over the LAN: set NIMBUS_TEST_IP or provision Wi-Fi")
    return ip


def _errlog_file(net, ip: str, name: str = ACTIVE_LOG) -> str:
    """One durable log file's text. A busy answer is retried within a budget; a
    rotated file that does not exist yet reads empty; any other answer fails with its
    status (it used to read as "no boot record")."""
    path = "/api/errlog" if name == ACTIVE_LOG else f"/api/errlog?file={name}"
    resp, err = poll_until(
        lambda: net.get(path, ip=ip, timeout=15.0), lambda r: r.status_code != 503, ERRLOG_BUSY_BUDGET_S, ERRLOG_RETRY_S
    )
    assert resp is not None, f"GET {path}: no answer ({err!r})"
    if name != ACTIVE_LOG and resp.status_code == 404:
        return ""
    assert resp.status_code == 200, f"GET {path} -> {resp.status_code}: {resp.text[:160]!r}"
    return resp.text


def _boots(net, ip: str) -> List[List[Dict[str, str]]]:
    active = _errlog_file(net, ip)
    previous = _errlog_file(net, ip, PREVIOUS_LOG) if starts_mid_boot(active) else ""
    boots = split_boots(parse_log(stitch_rotated(previous, active)))
    assert boots, "the durable log holds no boot record (is this the crash-forensics image?)"
    return boots


def _latest_report(net, ip: str) -> dict:
    return boot_report(_boots(net, ip)[-1])


def _boot_n(net, ip: str) -> Optional[int]:
    """This boot's number (the n= of its boot record), read before a crash is fired."""
    n = net.get_json("/api/state", ip=ip, timeout=10.0)["boot"]["n"]
    return int(n) if isinstance(n, int) or str(n).isdigit() else None


def _crash_report(net, ip: str, before_n: Optional[int]) -> dict:
    """The record of the boot the crash produced (see first_boot_after)."""
    rep, later = first_boot_after(_boots(net, ip), before_n)
    assert rep is not None, f"no boot after boot n={before_n} in the durable log"
    if later:
        print(f"[L39] boot(s) after the crash boot, caused by the harness: {later}")
    rep["later"] = later
    return rep


def _arm_log(net, ip: str) -> str:
    """The durable log's PANIC arm=turn lines (an arm the turn dropped says so)."""
    text = _errlog_file(net, ip)
    return " | ".join(ln for ln in text.splitlines() if "PANIC arm=turn" in ln)[-400:]


class NoCrash(Exception):
    """The board did not reset within the window: nothing crashed."""


def _new_boot_seen(net, ip: str, before_n: Optional[int], budget_s: float) -> bool:
    """Over the LAN: did a boot newer than ``before_n`` start? (connection errors while
    the board reboots are retried, see poll_until)."""
    if before_n is None:
        return True  # nothing to compare: assume the silent console hid a reset
    value, _ = poll_until(lambda: _boot_n(net, ip), lambda n: n is not None and n > before_n, budget_s, interval_s=3.0)
    return value is not None and value > before_n


def host_link_dropped(net, ip: str, before_n: Optional[int], budget_s: float = 60.0) -> bool:
    """A console that went silent where a reboot should answer is the host link, not
    the board, once a boot newer than ``before_n`` answers over the LAN."""
    return net is not None and _new_boot_seen(net, ip, before_n, budget_s)


def console_after_silent_boot(device) -> bool:
    """Win the console back after a reboot the host link missed. On the S3's native
    USB the console can stay silent after a panic reset while the board boots and
    runs (unit L, 22 Oct 2026: the ROM printed up to "entry", the app's boot never
    reached the host, the board served HTTP on the crash boot). A libusb bus reset
    (no chip reset; MAC-safe with two boards attached through NIMBUS_HIL_OTHER_SERIAL)
    and a reopen; True once the console answers PING. A reopen may reset the chip
    (a usb boot after the crash boot, which first_boot_after skips)."""
    device.close()
    device.bus_reset()
    try:
        device.reopen_after_reenumerate(drain_boot=True)
    except Exception as exc:  # noqa: BLE001 - reported, the caller decides
        print(f"[L39] the console did not come back after a bus reset: {exc}")
        return False
    return device.ping(timeout=5.0)


def _await_crash_reboot(device, net, ip: str, before_n: Optional[int], timeout: float) -> "Optional[str]":
    """Wait out a crash and the reboot after it; returns the READY beacon's IP.

    The panic handler prints its own report ("Guru Meditation", "abort()", a backtrace)
    before the reset. Those lines are the crash under test, not a boot failure, so they
    are read past here until the ROM's ``rst:`` banner; only then does the boot scanner
    (which fails on panic markers) take over.

    The serial handle is reopened only when it is really gone. On the S3's native USB a
    reopen resets the chip (reason ``usb``), so a reopen after a window in which nothing
    crashed used to manufacture a reboot and hide the fact that the panic never fired.
    Now: a dropped port is reopened; a silent console with no reset banner is checked over
    the LAN, and if no new boot started, NoCrash is raised and the board is left alone."""
    try:
        device.expect("rst:", timeout=timeout)
    except ExpectTimeout:
        if not _new_boot_seen(net, ip, before_n, 45.0):
            raise NoCrash(f"no reset within {timeout:.0f} s of the panic (boot n={before_n} still running)")
        device.reopen_after_reenumerate(drain_boot=False)  # the handle was a dead endpoint
    except Exception:  # noqa: BLE001 - the USB endpoint dropped (re-enumerated)
        device.reopen_after_reenumerate(drain_boot=False)
    try:
        _, ip2 = device.wait_ready(timeout=60.0)
        return ip2
    except ExpectTimeout:
        # A silent console after the reset is the host link, not the board, once a
        # new boot answers over the LAN: judge the crash there (the report is read
        # over HTTP anyway) and win the console back for the next leg.
        if not host_link_dropped(net, ip, before_n):
            raise
        print("[L39] the console stayed silent after the reset; the new boot answered over the LAN")
        if not console_after_silent_boot(device):
            raise ExpectTimeout(
                "the console after a bus reset (the crash boot answers over the LAN; a USB wedge: "
                "tools/usb_reset.py --skip <other unit MAC> --any)",
                device._transcript,
            ) from None
        return None


def _panic_and_reboot(device, net, kind: str) -> "tuple[str, Optional[int]]":
    """Fire ``PANIC <kind>`` and wait for the next boot's READY. Returns the LAN IP and
    the boot number that was running before the crash."""
    ip = _lan_ip(None)
    before = _boot_n(net, ip)
    device.drain(quiet=0.2)
    device.send(f"PANIC {kind}")
    # The task watchdog needs ~8 s to fire; the dump write takes a second or two.
    ip2 = _await_crash_reboot(device, net, ip, before, 20.0 if kind == "twdt" else 10.0)
    return _lan_ip(ip2), before


def _require_panic_verb(device) -> None:
    """Loud skip when the image has no PANIC verb (a test console without it)."""
    device.drain(quiet=0.2)
    device.send("PANIC ?")  # an unknown kind: the console prints its usage and does not crash
    try:
        device.expect("kind must be", timeout=3.0)
    except Exception:  # noqa: BLE001 - no usage line means the verb is absent
        pytest.skip("the test console has no PANIC verb (needs the test image with the PANIC verb)")


def _arm_turn_panic(device, kind: str, round_: int) -> None:
    """``PANIC <kind> arm=turn round=<n>``: loud skip when the console hook is not wired."""
    device.drain(quiet=0.2)
    device.send(f"PANIC {kind} arm=turn round={round_}")
    line = device.expect("PANIC", timeout=5.0)
    if "not wired" in line:
        pytest.skip(f"PANIC arm=turn is not wired in this image: {line!r}")
    assert "PANIC armed" in line, f"PANIC arm=turn refused: {line!r}"


# ---- HIL legs ----------------------------------------------------------------------
# Marks sit on each leg, not on the module: the destructive legs carry only
# `destructive` (the L33 convention), so the gate's `-m hil` and the LAN suite's
# `-m "net and not manual"` never select a factory reset or a firmware rollback.


@pytest.mark.hil
@pytest.mark.net
@pytest.mark.parametrize("kind", IMMEDIATE_KINDS)
def test_panic_kind_leaves_a_crash_report(device, net, kind):
    _require_panic_verb(device)
    ip, before = _panic_and_reboot(device, net, kind)
    rep = _crash_report(net, ip, before)
    ok, why = crash_verdict(rep, kind)
    print(f"[L39] {kind}: boot={rep['boot']} crash={rep['crash']} why={rep['why']!r}")
    assert ok, f"PANIC {kind}: {why}"
    state = net.get_json("/api/state", ip=ip, timeout=10.0)["boot"]
    assert state["crash"] and state["crash"]["fp"] == rep["crash"]["fp"], "/api/state names another dump"
    assert state["crash"]["why"] and state["crash"]["why"] != "(no reason recorded)", "/api/state has no reason"
    if not rep["later"]:  # the crash boot is still the running boot
        assert state["crash"]["new"] is True and state["reason"] == kind_slug(kind)
    # A plain reboot right after: the same dump is stale, mentioned once, not repeated.
    device.reset()
    _, ip2 = device.wait_ready(timeout=40.0)
    stale = _latest_report(net, _lan_ip(ip2))
    ok, why = stale_verdict(stale)
    assert ok, f"plain reboot after PANIC {kind}: {why}"
    assert stale["crash"]["fp"] == rep["crash"]["fp"], "the dump changed without a crash (or was erased)"


@pytest.mark.hil
@pytest.mark.net
def test_seq0_exists_in_every_boot(device, net):
    """No pre-mount hole: every boot section of the current log file starts with its
    boot record at seq 0."""
    ip = _lan_ip(None)
    boots = split_boots(parse_log(_errlog_file(net, ip)))
    assert boots, "no boot record in the durable log"
    ok, why = every_boot_starts_at_seq0(boots)
    assert ok, why


@pytest.mark.hil
@pytest.mark.net
def test_usb_cdc_reset_reads_usb(device, net):
    """The host's USB-serial chip reset (esptool --after hard-reset) is ESP_RST_USB with
    ROM code 0x15 on the S3: it used to read "unknown". Also on the WAKE? seam."""
    assert device.esptool_hard_reset(), "esptool could not reset the chip"
    _, ip = device.wait_ready(timeout=40.0)
    rep = _latest_report(net, _lan_ip(ip))
    assert rep["boot"]["slug"] == "usb", f"reason {rep['boot']['slug']!r}({rep['boot']['raw']}), want usb"
    assert rep["boot"]["raw"] in ("15", "16"), f"raw ROM code 0x{rep['boot']['raw']}, want 0x15 or 0x16"
    m = device.cmd_re("WAKE?", r"WAKE reset=(\S+)", timeout=5.0)
    assert m.group(1) == "usb"


@pytest.mark.hil
@pytest.mark.net
@pytest.mark.agent
@pytest.mark.parametrize("kind", ARMED_KINDS)
def test_live_turn_panic_in_the_transport(device, net, kind):
    """``PANIC <kind> arm=turn round=0``: a panic on tg_poll at the first transport body
    write of a real turn, before any request byte leaves (so nothing is billed). The dump
    must fit 64 KB with tg_poll deep in a TLS exchange and the relay connected (``canary``
    overflows tg_poll's stack right there: the capacity worst case), and its backtrace
    is the turn's own."""
    _require_panic_verb(device)
    ip = _lan_ip(None)
    before = _boot_n(net, ip)
    _arm_turn_panic(device, kind, 0)
    r = net.post("/api/chat", {"text": "L39 crash-forensics probe: reply with the word ok."}, ip=ip, timeout=10.0)
    assert r.status_code == 200, f"/api/chat -> {r.status_code}: {r.text[:200]}"
    # The turn reaches its first exchange within seconds, panics, and the board reboots.
    try:
        ip2 = _await_crash_reboot(device, net, ip, before, 90.0)
    except NoCrash as exc:
        pytest.fail(f"the armed panic did not fire: {exc}; log: {_arm_log(net, ip)!r}")
    rep = _crash_report(net, _lan_ip(ip2), before)
    ok, why = crash_verdict(rep, kind, armed=True)
    print(f"[L39] live {kind}: crash={rep['crash']} bt={rep['bt']} why={rep['why']!r}")
    assert ok, f"live PANIC {kind} arm=turn: {why}"
    if kind == "nullptr":
        assert int(rep["bt"]["depth"]) >= 3, "the backtrace is too shallow to name the transport frame"


@pytest.mark.hil
@pytest.mark.net
@pytest.mark.agent
def test_panic_armed_at_a_later_round(device, net):
    """``round=1`` lands in the turn's second exchange: the first one (a tool call the
    prompt asks for) is a real, billed request, about $0.001 on claude-haiku-5-5. The boot
    line's prev= names round 1 of a message turn (the RTC breadcrumb), and the owner is
    told which request was lost. Skips when the model answered without a tool round."""
    _require_panic_verb(device)
    ip = _lan_ip(None)
    before = _boot_n(net, ip)
    _arm_turn_panic(device, "nullptr", 1)
    prompt = "L39 probe: call the device status tool once, then reply with the word ok."
    r = net.post("/api/chat", {"text": prompt}, ip=ip, timeout=10.0)
    assert r.status_code == 200, f"/api/chat -> {r.status_code}: {r.text[:200]}"
    try:
        ip2 = _await_crash_reboot(device, net, ip, before, 120.0)
    except NoCrash as exc:
        log = _arm_log(net, ip)
        if "dropped" in log:  # the model answered in one round, so the arm was dropped
            pytest.skip(f"the turn ended before round 1 (no tool call): {log!r}")
        pytest.fail(f"the armed panic did not fire and was not dropped: {exc}; log: {log!r}")
    rep = _crash_report(net, _lan_ip(ip2), before)
    ok, why = crash_verdict(rep, "nullptr", armed=True)
    assert ok, f"PANIC nullptr arm=turn round=1: {why}"
    assert "turn-message/r1" in rep["boot"]["prev"], f"prev={rep['boot']['prev']!r}, want turn-message/r1"


@pytest.mark.hil
@pytest.mark.net
def test_offline_boots_keep_their_rows(device, net, secrets):
    """Pre-sync device events survive resets: boots that never get a clock (Wi-Fi
    pointed at an absent network, REBOOT cold clears the RTC clock) still reach the
    timeline at the first sync. The link comes back inside the last offline boot, so
    that boot syncs itself and labels its own row "[pre-sync, ...]"; every EARLIER
    offline boot's row says "from boot n=<k>, clock never synced". Three cold boots,
    so the rows of two earlier boots must ride the pending file across resets. Only
    rows the first sync wrote count (a previous run's rows are on the same page)."""
    try:
        secrets.require_sta()
        ssid, pw = secrets.sta_ssid, secrets.sta_pass
    except Exception:  # noqa: BLE001 - SecretsUnavailable
        pytest.skip("needs the bench Wi-Fi credentials to restore the link")
    ip = _lan_ip(None)
    page = "/api/mem/episodic?session=system&limit=40"
    seen = {str(m.get("id")) for m in net.get_json(page, ip=ip, timeout=15.0).get("messages", [])}
    device.wifi("l39-absent-network", "not-a-password")
    for _ in range(OFFLINE_BOOTS):
        device.cmd("REBOOT cold", "REBOOTING", timeout=5.0)
        device.reopen_after_reenumerate(drain_boot=False)
        device.wait_ready(timeout=60.0)
    device.wifi(ssid, pw)
    # The board is rejoining Wi-Fi: a connect timeout inside the budget is retried.
    got, err = poll_until(
        lambda: net.get_json(page, ip=ip, timeout=15.0),
        lambda p: offline_rows_verdict(p, seen, OFFLINE_BOOTS - 1)[0],
        120.0,
    )
    ok, why = offline_rows_verdict(got or {}, seen, OFFLINE_BOOTS - 1)
    assert ok, f"the offline boots' rows did not reach the timeline: {why} (last network error: {err!r})"


@pytest.mark.destructive
def test_factory_reset_erases_the_dump(device, net, secrets):
    """A factory reset erases the core dump (stacks can hold the previous owner's keys):
    the next boot reports no crash and /api/state says crash: null."""
    device.factory_reset()
    try:
        secrets.require_sta()
    except Exception:  # noqa: BLE001 - SecretsUnavailable
        pytest.skip("needs the bench Wi-Fi credentials: a factory reset forgets the network")
    device.wifi(secrets.sta_ssid, secrets.sta_pass)
    ip = _lan_ip(None)
    rep = _latest_report(net, ip)
    assert rep["crash"] is None and rep["bad"] is None, f"a dump survived the factory reset: {rep['crash']}"
    assert net.get_json("/api/state", ip=ip, timeout=10.0)["boot"]["crash"] is None


@pytest.mark.destructive
def test_early_boot_abort_is_explained(device, net):
    """``OTASIM crash`` aborts in the OTA boot guard on every boot until the guard rolls
    back. The image that boots after the rollback reads the last abort's dump. Needs the
    same crash-forensics image in BOTH app slots (flash it twice) so the rolled-back
    image is a reader too."""
    ip0 = _lan_ip(None)
    before = _boot_n(net, ip0)
    device.cmd("OTASIM arm app0", "OTASIM armed", timeout=5.0)
    device.cmd("OTASIM crash", "OTASIM crash", timeout=5.0)
    device.reset()
    # Each boot aborts in the guard (its panic print is expected) until the guard rolls
    # back; the rolled-back image reaches READY. The aborting boots stop before the boot
    # record, so the first numbered boot after `before` is the rolled-back image.
    deadline = time.time() + 180.0
    ip = None
    while time.time() < deadline and ip is None:
        try:
            ip = _await_crash_reboot(device, net, ip0, before, 30.0)
        except Exception:  # noqa: BLE001 - another aborting boot before the rollback
            continue
    device.cmd("OTASIM clear", "OTASIM cleared", timeout=5.0)
    rep = _crash_report(net, _lan_ip(ip), before)
    assert rep["crash"] is not None and rep["crash"]["new"] == "1", "the early-boot abort left no crash report"
    assert rep["why"] and "abort" in rep["why"], f"crash-why {rep['why']!r} does not name the abort"
    # The RTC breadcrumb names where the previous run stopped: inside the boot guard.
    assert "ota-guard" in rep["boot"]["prev"], f"prev={rep['boot']['prev']!r}, want the ota-guard stage"


@pytest.mark.hil
@pytest.mark.net
def test_breadcrumb_names_the_turn_step_after_a_mid_turn_panic(device, net):
    """After a panic in a turn, the boot line's prev= says the run was in a turn (the
    RTC breadcrumb survives the panic reset). Runs after the live legs; skips if the
    newest boot was not a turn crash."""
    rep = latest_turn_crash(_boots(net, _lan_ip(None)))
    if rep is None:
        pytest.skip("no mid-turn crash boot in the durable log (run the live legs first)")
    assert "/turn" in rep["boot"]["prev"], f"prev={rep['boot']['prev']!r}, want the turn stage"
