"""CUM-141: mid-run console-wedge detection + in-place auto-recovery.

The net suite (l9 fault injections + token-regen under console I/O) can stall the
USB-Serial-JTAG endpoints mid-run: the app keeps serving HTTP on the LAN while
every console command times out. Historically that turned ONE root cause into 58
cascading DeviceError/ExpectTimeout, each after a full read timeout - a long,
noisy run that buried the real signal.

The recovery (proven live 2026-08-24, see /tmp/nimbus-devices/MAP.md): a libusb
bus reset clears the stalled endpoint WITHOUT rebooting the chip (uptime
preserved). This module is the pure, host-testable core; conftest.py wires it to
the pytest runtest hooks.
"""

from __future__ import annotations

import time

# Exception messages that mean "the console link is dead" (a wedge), not "the
# test's assertion failed" and not "the firmware crashed" (a BootError is a real
# finding and must never be masked as a wedge).
_WEDGE_MSG_MARKERS = ("unresponsive", "no serial", "not open", "silent", "console", "no lines")


def is_console_death(exc: BaseException) -> bool:
    """True when `exc` looks like the USB-Serial-JTAG endpoint stalled (a wedge),
    as opposed to a genuine assertion failure or a firmware boot-loop."""
    name = type(exc).__name__
    # A real crash/boot-loop is a finding, not a wedge - never auto-recover it.
    if name == "BootError":
        return False
    if name in ("ExpectTimeout", "DeviceLostError"):
        return True
    if name == "DeviceError":
        msg = str(exc).lower()
        return any(m in msg for m in _WEDGE_MSG_MARKERS)
    return False


class WedgeSentinel:
    """Session-scoped console-wedge state + the one-shot in-place recovery.

    The device is any object exposing ``bus_reset() -> bool``, ``close()``,
    ``open()`` and ``ping(timeout) -> bool`` (the HIL ``Device``). ``sleep`` is
    injectable so unit tests do not wait real seconds.
    """

    def __init__(self, enabled: bool, sleep=time.sleep, log=print) -> None:
        self.enabled = enabled
        self.device = None  # set by the `device` fixture on a hardware run
        self.recovery_attempted = False
        self.recovered = False
        self.wedged = False
        self._sleep = sleep
        self._log = log

    def try_recover(self) -> bool:
        """One-shot libusb bus reset + reopen, no chip reboot. Returns True if the
        console answers afterward. Idempotent: only the first call does work."""
        if self.device is None or self.recovery_attempted:
            return self.recovered
        self.recovery_attempted = True
        self._log(
            "\n[wedge_guard] CUM-141: console-death signature - attempting one "
            "in-place USB bus reset (no reboot)"
        )
        try:
            if not self.device.bus_reset():
                self._log("[wedge_guard] CUM-141: bus reset reported no matching device")
            self.device.close()
            self._sleep(2.0)  # endpoint re-enumerates
            self.device.open()  # open() probes link-liveness itself (no REBOOT)
            alive = bool(self.device.ping(timeout=3.0))
        except Exception as exc:  # noqa: BLE001 - recovery must never raise into the run
            self._log(f"[wedge_guard] CUM-141: recovery raised {exc!r}")
            alive = False
        self.recovered = alive
        self.wedged = not alive
        marker = "RECOVERED (console back, uptime preserved)" if alive else "STILL WEDGED"
        self._log(f"[wedge_guard] CUM-141: recovery result -> {marker}")
        return alive
