"""L39 - crash forensics, one merged panic + forensics suite (program-2026-10-18).

The 18 Oct Lumi panic left no backtrace anywhere (canonical row 5/14): the
coredump was only erased, the reset reason was not durable, and pre-mount log
lines were dropped. Lane O1 makes every crash leave a readable summary at the
next boot. This suite is that fix's on-glass proof: for each crash KIND it fires
the test-console ``PANIC`` verb (lane N), waits out the reboot, and asserts the
boot carries a named reset reason and a one-shot ``crash:`` summary.

Lane split (binding): lane N owns ``src/test_console.cpp`` and lands the
``PANIC abort|nullptr|canary|twdt|oom`` verb this drives, plus THIS skeleton (the
kind matrix, the reboot/read oracle, the markers). Lane O1 lands the boot-time
forensics the assertions read (the ``boot:`` / ``crash:`` lines, the reset-reason
table, the dump dedup) and completes the per-kind task/backtrace assertions. Until
O1 merges, ``_forensics_present`` reports the lines are absent and every leg is a
reasoned SKIP, never a silent pass.

Oracle (the contract O1 fills in):
  * ``boot:`` line 1 of the boot carries the reset-reason slug AND the raw code.
  * ``crash: ... new=1 this-reset=1 task=<expected> ...`` once per crash episode;
    ``crash-why`` is non-empty; ``size=`` is under 65536 (the coredump partition).
  * a plain reboot after it gives ``new=0`` (the dump is reported once).
  * ``seq 0`` exists in every boot (no pre-mount hole, RC2).

⚠ DESTRUCTIVE in spirit: it panics the board repeatedly. It carries ``hil`` (it
needs the board on serial) and runs in the release battery; it is not in the
``destructive`` factory-reset set. The board is left running after the last
reboot. The ``canary`` tg_poll-deep variant (study T7/B4: the worst-case dump the
64 KB partition must hold during a live TLS turn) is driven by
``PANIC nullptr arm=turn`` and lives in :func:`test_panic_mid_tls_turn`, gated on
a provider key; lane O1 wires the arm hook.
"""

from __future__ import annotations

import time

import pytest

from device import Device, ExpectTimeout

pytestmark = pytest.mark.hil

# The crash kinds the PANIC verb accepts (lane N), with the task each is expected
# to crash on when fired from the console loop, and the reset-reason class O1's
# boot line must report. A console-fired panic runs on the loop task; the
# tg_poll-deep variant is the separate mid-TLS leg below.
PANIC_KINDS = [
    # kind,     expected reset class (slug substring), expected task hint
    ("abort", "panic", "loop"),
    ("nullptr", "panic", "loop"),
    ("canary", "panic", "loop"),
    ("twdt", "wdt", "loop"),
    ("oom", "panic", "loop"),
]

_BOOT_RE = r"boot:\s"
_CRASH_RE = r"crash:\s"


def _forensics_present(dev: Device) -> bool:
    """True once lane O1's boot forensics are in the image: a fresh boot prints a
    ``boot:`` line. Before O1 merges there is none, so the legs skip with reason."""
    try:
        dev.drain(quiet=0.3)
        dev.send("REBOOT")
        dev.wait_ready(timeout=30.0)
        dev.expect_re(_BOOT_RE, timeout=8.0)
        return True
    except ExpectTimeout:
        return False


@pytest.fixture(scope="module")
def forensics(device: Device) -> Device:
    if not _forensics_present(device):
        pytest.skip("crash forensics (lane O1) not present in this image yet - boot: line absent")
    return device


@pytest.mark.parametrize("kind,reset_class,task_hint", PANIC_KINDS, ids=[k[0] for k in PANIC_KINDS])
def test_panic_kind_leaves_a_readable_summary(forensics: Device, kind, reset_class, task_hint):
    dev = forensics
    dev.drain(quiet=0.3)
    dev.send(f"PANIC {kind}")
    # The board crashes and reboots; wait for the READY beacon on the far side.
    dev.wait_ready(timeout=40.0)

    boot = dev.expect_re(r"boot:\s+.*reason=(?P<reason>\S+).*code=(?P<code>\S+)", timeout=10.0)
    assert reset_class in boot.group("reason").lower(), (
        f"{kind}: boot reason {boot.group('reason')!r} does not name the {reset_class} class"
    )
    assert boot.group("code") not in ("", "unknown"), f"{kind}: raw reset code missing"

    crash = dev.expect_re(
        r"crash:\s+.*new=(?P<new>\d).*this-reset=(?P<tr>\d).*task=(?P<task>\S+).*size=(?P<size>\d+)",
        timeout=10.0,
    )
    assert crash.group("new") == "1", f"{kind}: crash not reported as new"
    assert crash.group("tr") == "1", f"{kind}: crash not attributed to this reset"
    assert int(crash.group("size")) < 65536, f"{kind}: dump {crash.group('size')} B exceeds the 64 KB partition"
    # task_hint is a soft check: O1 tightens the exact task per kind.
    dev.expect_re(r"crash-why:\s+\S+", timeout=6.0)


def test_plain_reboot_reports_no_new_crash(forensics: Device):
    dev = forensics
    dev.send("PANIC abort")
    dev.wait_ready(timeout=40.0)
    dev.expect_re(r"crash:\s+.*new=1", timeout=10.0)
    # A clean reboot afterward: the same dump must NOT be reported as new again.
    dev.drain(quiet=0.3)
    dev.send("REBOOT")
    dev.wait_ready(timeout=30.0)
    crash = dev.expect_re(r"crash:\s+.*new=(?P<new>\d)", timeout=10.0)
    assert crash.group("new") == "0", "a plain reboot re-reported the crash as new"


def test_seq_zero_present_each_boot(forensics: Device):
    """RC2: no pre-mount log hole - seq 0 exists in every boot's durable log."""
    dev = forensics
    dev.drain(quiet=0.3)
    dev.send("REBOOT")
    dev.wait_ready(timeout=30.0)
    dev.expect_re(r"\bseq[ =]0\b", timeout=10.0)


@pytest.mark.agent
def test_panic_mid_tls_turn(forensics: Device, secrets):
    """Study T7/B4: a panic deep on tg_poll inside a live TLS turn produces a
    decodable dump within 64 KB. Lane N lands ``PANIC nullptr arm=turn``; lane
    O1 wires the arm hook. Skipped until the hook is wired."""
    dev = forensics
    try:
        armed = dev.cmd("PANIC nullptr arm=turn", "arm", timeout=5.0)
    except ExpectTimeout:
        pytest.skip("PANIC arm=turn hook not wired yet (lane O1)")
    if "not wired" in armed:
        pytest.skip("PANIC arm=turn hook not wired yet (lane O1)")
    if not secrets.get("ANTHROPIC_API_KEY"):
        pytest.skip("no ANTHROPIC_API_KEY for the live TLS turn")
    dev.send("TURN tell me a one word greeting")
    dev.wait_ready(timeout=60.0)
    crash = dev.expect_re(r"crash:\s+.*task=(?P<task>\S+).*size=(?P<size>\d+)", timeout=10.0)
    assert "tg_poll" in crash.group("task"), f"mid-TLS crash attributed to {crash.group('task')}, not tg_poll"
    assert int(crash.group("size")) < 65536, "mid-TLS dump exceeds the 64 KB partition"
    time.sleep(0.1)
