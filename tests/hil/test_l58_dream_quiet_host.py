"""Host checks for the L58 dream legs' stream readers (no device).

The bench run on unit L (21 Oct) failed leg 4 with "panic during boot" right after
``PANIC arm=turn``: the leg called wait_ready straight after DREAM, and wait_ready
fails on any panic marker, so it read the panic the leg had armed as a boot failure
while the next boot came up and ran. These pin the reader on canned serial lines:
the armed panic's report is read past to the ROM ``rst:`` banner, the boot scan only
starts after it, and a drill the panic never reached fails.
"""

from __future__ import annotations

import pytest

from device import BootError, Device, DeviceError
from test_l58_dream_quiet import _await_armed_panic_reboot

pytestmark = pytest.mark.host

PANIC = "Guru Meditation Error: Core  0 panic'ed (StoreProhibited). Exception was unhandled."
ARMED_REPORT = [
    "[agent] orchestrator: turn host=anthropic (conv=0 heap=30880)",
    PANIC,
    "Core  0 register dump:",
    "PC      : 0x421c9772  PS      : 0x00060a30  A0      : 0x822dbba9",
    "Backtrace: 0x421c976f:0x3fcf1d50 0x422dbba3:0x3fcf1d70 0x421cb23a:0x3fcf1d90",
    "Rebooting...",
]
NEXT_BOOT = [
    "rst:0xc (RTC_SW_CPU_RST),boot:0x8 (SPI_FAST_FLASH_BOOT)",
    "[agent] boot: n=120 fw=v4.5.13 reason=crash(panic)(0x0c) prev=n119/80s/turn-routine",
    "[agent] crash: fp=42603dfb new=1 this-reset=1 task=tg_poll pc=0x421c9772 cause=29(StoreProhibited)",
    "[agent] crash-why: StoreProhibited in tg_poll: a write to an invalid address (0x00000000)",
    "orch: done",
    "READY mode=1 ip=192.168.50.61",
]


def _canned(lines):
    dev = Device(port="/dev/null-host-test")
    dev._pushback = list(lines)
    return dev


def test_the_armed_panic_is_read_past_before_the_boot_scan():
    dev = _canned(ARMED_REPORT + NEXT_BOOT)
    assert _await_armed_panic_reboot(dev, panic_timeout=1.0) == PANIC
    assert dev._pushback == []  # the boot was read through READY


def test_the_old_flow_reads_the_armed_panic_as_a_boot_failure():
    """Counter-test: the same stream handed straight to wait_ready is the bench failure."""
    dev = _canned(ARMED_REPORT + NEXT_BOOT)
    with pytest.raises(BootError, match="panic during boot"):
        dev.wait_ready(timeout=1.0)


def test_a_drill_the_panic_never_reached_fails():
    dev = _canned(["DREAM fired ok=1 detail=(dream d20747: ran a1 pruned=0 deduped=0)"])
    with pytest.raises(DeviceError):
        _await_armed_panic_reboot(dev, panic_timeout=1.0)


def test_a_second_panic_in_the_next_boot_still_fails():
    """Reading past the armed panic never hides a real one: a panic after the rst:
    banner is a boot failure."""
    boot_panic = NEXT_BOOT[:2] + [PANIC] + NEXT_BOOT[2:]
    dev = _canned(ARMED_REPORT + boot_panic)
    with pytest.raises(BootError, match="panic during boot"):
        _await_armed_panic_reboot(dev, panic_timeout=1.0)
