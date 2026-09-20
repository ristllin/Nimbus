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


def is_wedge_candidate(exc: BaseException) -> bool:
    """True when `exc` COULD be a stalled USB-Serial-JTAG endpoint. This is only a
    candidate: an ``ExpectTimeout`` is far more often an ordinary content-assertion
    miss on a perfectly live console (a wanted line never arrived), so a candidate
    must be confirmed by an actual liveness probe before any recovery runs (see
    WedgeSentinel.try_recover). A firmware crash / boot-loop (BootError) is a real
    finding and is never a candidate."""
    name = type(exc).__name__
    if name == "BootError":
        return False
    if name in ("ExpectTimeout", "DeviceLostError"):
        return True
    if name == "DeviceError":
        msg = str(exc).lower()
        return any(m in msg for m in _WEDGE_MSG_MARKERS)
    return False


# Back-compat alias (older name); prefer is_wedge_candidate.
is_console_death = is_wedge_candidate


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

    def _console_alive(self) -> bool:
        """Cheap liveness probe: PING -> PONG. Never raises."""
        try:
            return bool(self.device.ping(timeout=3.0))
        except Exception:  # noqa: BLE001 - a dead link must read as "not alive", not crash
            return False

    def try_recover(self) -> bool:
        """Confirm the console is actually dead, then recover it with a two-rung
        ladder: first a one-shot libusb bus reset + reopen (no chip reboot, uptime
        preserved); if that does not bring the console back, an esptool hard reset
        (which drives the chip's reset line - this DOES reboot, so it is the last
        resort). Returns True if the console is or comes back alive. A bare candidate
        signature (usually an ordinary ExpectTimeout on a LIVE console) is NOT
        treated as a wedge: the confirming ping short-circuits it, so no gratuitous
        USB reset happens on a normal test failure."""
        if self.device is None or self.recovery_attempted:
            return self.recovered
        # Confirm death before the disruptive reset. If the console answers, this was
        # just a content-assertion miss, not a wedge - leave everything untouched.
        if self._console_alive():
            return False
        self.recovery_attempted = True
        self._log(
            "\n[wedge_guard] CUM-141: console confirmed unresponsive - attempting one in-place USB bus reset (no reboot)"
        )
        alive = self._bus_reset_recover()
        if not alive:
            alive = self._esptool_recover()
        self.recovered = alive
        self.wedged = not alive
        if alive:
            marker = "RECOVERED (console back)"
        else:
            marker = "STILL WEDGED (needs physical BOOT+RST / power cycle)"
        self._log(f"[wedge_guard] CUM-141: recovery result -> {marker}")
        return alive

    def _bus_reset_recover(self) -> bool:
        """Rung 1: a libusb bus reset clears the stalled endpoint without rebooting
        the chip (uptime preserved). MAC-safe: Device.bus_reset() skips the other
        board by its serial (a wedged S3 has none)."""
        try:
            if not self.device.bus_reset():
                self._log("[wedge_guard] CUM-141: bus reset reported no matching device")
            self.device.close()
            self._sleep(2.0)  # endpoint re-enumerates
            self.device.open()  # open() probes link-liveness itself (no REBOOT)
            return bool(self.device.ping(timeout=3.0))
        except Exception as exc:  # noqa: BLE001 - recovery must never raise into the run
            self._log(f"[wedge_guard] CUM-141: bus-reset recovery raised {exc!r}")
            return False

    def _esptool_recover(self) -> bool:
        """Rung 2: a wedge a bus reset cannot clear needs a hard reset. esptool
        drives the chip's reset line (this REBOOTS the chip - uptime is not
        preserved), then reopen and confirm. Targets this board's own port, never
        the other one. Absent the hook (an older Device), this rung is a no-op."""
        fn = getattr(self.device, "esptool_hard_reset", None)
        if fn is None:
            return False
        self._log(
            "[wedge_guard] CUM-418: bus reset did not recover the console - escalating "
            "to an esptool hard reset (reboots the chip)"
        )
        try:
            if not fn():
                self._log("[wedge_guard] CUM-418: esptool hard reset did not run")
                return False
            self.device.close()
            self._sleep(2.0)
            self.device.open()
            return bool(self.device.ping(timeout=3.0))
        except Exception as exc:  # noqa: BLE001 - recovery must never raise into the run
            self._log(f"[wedge_guard] CUM-418: esptool recovery raised {exc!r}")
            return False
