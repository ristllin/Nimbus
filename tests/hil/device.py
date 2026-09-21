"""Device serial helper (the HIL test spec).

A robust wrapper over the Nimbus USB-CDC serial link that survives NATIVE-USB
RE-ENUMERATION on reset (F13): on the S3 the USB-CDC endpoint is provided by the
firmware itself, so a reset tears the port down and it reappears - possibly under a
DIFFERENT ``/dev/cu.usbmodem*`` node. ``reset()`` therefore closes the handle, waits,
re-globs, and reopens.

The reset pulse mirrors the one in ``tools/nsn_send.py`` (DTR/RTS toggle).

Serial contract this helper speaks (device -> host lines it matches):
  * boot beacon:            ``READY mode=<n> ip=<..>``   (NIMBUS_TEST)
                            fallbacks: ``NSN ready`` / ``PROVISION READY``
  * ping:                   ``PING`` -> ``PONG``
  * status:                 ``STATUS`` -> ``STATUS mode=.. wifi=.. ip=.. heap=.. up=..``
  * render:                 ``RENDER?`` -> ``RENDER screen=.. posture=.. ring=<dark|single|seg:N> bright=..``
  * selftest:               ``TEST <name>`` -> ``RESULT <name> PASS|FAIL|SKIP <k=v>..``
  * wifi reason:            ``WIFI_DISCONNECTED reason=<n>``
  * wifi ip:                ``WIFI_GOT_IP <ip>``

NOTHING in this module opens a port at import time; the port is only touched inside
``Device`` methods that fixtures call after the hardware gate. So importing this
module (and collecting the suite) never contacts hardware.
"""

from __future__ import annotations

import glob
import os
import re
import subprocess
import sys
import time
from typing import List, Optional, Pattern

try:
    import serial  # pyserial
except ImportError:  # pragma: no cover - collection must still succeed
    serial = None  # the device fixture raises a clear error before use


# Native USB-CDC (`usbmodem`) AND the DevKit's UART bridge (`usbserial-*`,
# `SLAB_USBtoUART`, `wchusbserial*`). The UART port is not optional to support:
# it is the ONLY port that can flash a fresh board unaided (its bridge drives
# EN/GPIO 0 electrically - see docs/hardware.md), so a board being brought up for
# the first time is reachable there and nowhere else. Globbing only usbmodem
# made every test LOUD-SKIP on exactly the board that most needed testing.
PORT_GLOBS = ("/dev/cu.usbmodem*", "/dev/cu.usbserial-*", "/dev/cu.SLAB_USBtoUART*", "/dev/cu.wchusbserial*")
PORT_GLOB = " | ".join(PORT_GLOBS)  # for messages only
BAUD = 115200

# Boot-log strings that mean the firmware crashed / is looping - a boot_ok FAIL.
PANIC_MARKERS = ("Guru Meditation", "abort()", "assert failed", "CORRUPT HEAP", "Backtrace:")

# ScreenId names, index == the numeric id the firmware emits (must track
# lib/core/include/nimbus/attention.h ScreenId order).
SCREEN_NAMES = (
    "StatusIdle",
    "JobDetail",
    "Badge",
    "Menu",
    "Battery",
    "Ask",
    "VoiceGlyph",
    "SetupInfo",
    "IdleArt",
    "ConfigQr",
    "SessionDetail",
    "Pairing",
    "SelfTest",
    "Screensaver",
    "TokenDetail",  # full device sign-in code (Connectivity > Device sign-in code,
    # and the Sign-in QR "Show code" tap - CUM-48 #3)
    "TouchCal",  # touch-calibration wizard (Display > Calibrate touch - CUM-189, F5)
)


# The one input that backs any header screen toward StatusIdle. drawHeader
# (lib/core/src/tft_screens.cpp) draws a Back tap region at (0,0,96,44) on every
# backable screen and a Home region at (0,0,110,44) on the status/voice/pairing
# screens; drawScreensaver makes the WHOLE screen a Home region. So a tap at the
# top-left of the header lands a Back or Home on ANY screen that draws one. Menu-
# closed it routes Action::Back/Home -> clear override + StatusIdle (src/main.cpp);
# menu-open it routes onLongPress -> back out one level (lib/core/src/tft_menu_tap
# .cpp), which repeated closes the menu. A blind CENTRE tap (the old code) is dead
# space on Ask (Close is bottom-centre) and on the Sign-in/Config QR, which is why
# it hung on screen=5/9 on the bench.
IDLE_HEADER_TAP = (20, 22)


def idle_nav_step(screen: int) -> str:
    """Pure navigation policy for ensure_status_idle: the kind of input that drives
    ONE step from ``screen`` toward StatusIdle. Total over every ScreenId the
    firmware enum defines (host-tested) so there is never a blind fallback tap that
    loops on a screen it does not understand.

    "idle"    - already StatusIdle, nothing to do.
    "restart" - NO tap region exists on this screen, so a confirmed restart is the
                only escape. TouchCal is the sole case: it consumes the touch
                surface for calibration (drawTouchCal registers no TapRegion).
    "tap"     - tap the header Back/Home target (IDLE_HEADER_TAP)."""
    if screen == SCREEN_NAMES.index("StatusIdle"):
        return "idle"
    if screen == SCREEN_NAMES.index("TouchCal"):
        return "restart"
    return "tap"


class RenderState:
    """Parsed ``RENDER ...`` line: what the panel + ring currently show."""

    def __init__(self, screen: int, posture: int, seg: int, single: bool, dark: bool, bright: int):
        self.screen = screen
        self.posture = posture
        self.seg = seg
        self.single = single
        self.dark = dark
        self.bright = bright

    @property
    def screen_name(self) -> str:
        return SCREEN_NAMES[self.screen] if 0 <= self.screen < len(SCREEN_NAMES) else f"?{self.screen}"

    @property
    def ring(self) -> str:
        """Compact classification used by test assertions: dark|single|seg:N."""
        if self.single:
            return "single"
        if self.seg > 0:
            return f"seg:{self.seg}"
        return "dark"

    def __repr__(self) -> str:  # readable assertion failures
        return f"RenderState(screen={self.screen_name}, posture={self.posture}, ring={self.ring}, bright={self.bright})"


class DeviceError(RuntimeError):
    """Base for device-harness failures."""


class DeviceLostError(DeviceError):
    """The serial node did not reappear after a reset/re-enumeration (F13)."""


class ExpectTimeout(DeviceError):
    """A wanted line/pattern never arrived before the deadline. Carries the
    captured transcript tail so the failure is debuggable."""

    def __init__(self, wanted: str, tail: List[str]):
        self.wanted = wanted
        self.tail = tail
        joined = "\n    ".join(tail[-20:]) if tail else "<no lines received>"
        super().__init__(f"timed out waiting for {wanted!r}; last lines:\n    {joined}")


class BootError(DeviceError):
    """A panic / reboot-loop was seen in the boot capture (F11/F14)."""


def list_ports() -> List[str]:
    """All matching serial nodes, sorted (stable ordering for 'newest' picks)."""
    out = []
    for g in PORT_GLOBS:
        out += glob.glob(g)
    return sorted(set(out))


# Below this uptime a reading is a fresh boot regardless of what it was before -
# the READY beacon plus the board's own bring-up spend a few seconds, so anything
# under ~15 s is post-reset. (Pure so the reboot decision is host-testable.)
FRESH_BOOT_UPTIME_S = 15

# The widest plausible fresh-boot uptime once the console first answers. A board
# with an SD card and a large episodic scan can take tens of seconds to reach its
# console (the L7 watchdog symptom), so the first settled STATUS after a genuine
# restart may read past the fast bar - but it is still a small ABSOLUTE number. A
# fresh boot is never tens of MINUTES in. This ceiling is the fix for the bench
# false-positive (reboot_and_confirm returned uptime=2494s "after a reboot"):
# is_fresh_boot used to accept ANY drop below the pre-reboot reading, so a ~6 s
# dip from a ~42-min uptime read as fresh instead of escalating to a hard reset.
FRESH_BOOT_CEILING_S = 120


def is_fresh_boot(before: Optional[int], after: Optional[int]) -> bool:
    """Did a reboot actually take? True when the post-reboot uptime is small
    ABSOLUTELY - either under the fast bar, or (for a slow SD-scan boot) dropped
    below the pre-reboot reading AND still within a plausible boot window. A soft
    REBOOT that is a no-op on native USB-CDC leaves uptime climbing (the CUM-418
    bench symptom: uptime=3254s after a reboot), which this returns False for so the
    caller can escalate to a hard reset. ``after`` None (console never settled) is
    never a fresh boot.

    The ceiling is load-bearing: a mere DROP is not enough. On the bench a ~6 s dip
    from a ~42-min uptime (before>2494, after=2494) satisfied the old decrease-only
    test and was wrongly returned as fresh - so reboot_and_confirm handed back a
    stale 2494 s instead of escalating. A real fresh boot is a small absolute
    number, never tens of minutes in."""
    if after is None:
        return False
    if after < FRESH_BOOT_UPTIME_S:
        return True
    if before is not None and after < before and after < FRESH_BOOT_CEILING_S:
        return True
    return False


class _BootScan:
    """Pure boot-stream classifier for wait_ready. Fed one line at a time, it
    recognizes the READY beacon and the legacy markers, flags a panic, and detects a
    reboot LOOP - defined as >= 2 ``rst:`` lines with NO app-level progress between
    them. That definition is what lets wait_ready size its wait from the stream
    without false-tripping on the harness's own double reset (reset() plus a
    CDC-reopen USB_UART_CHIP_RESET), where the board reaches app lines between the
    two resets. Host-tested so the sizing logic is covered with no board."""

    # Lines that prove the app actually came up this boot (so a following rst: is a
    # fresh cycle, not the continuation of a loop that never got off the ground).
    _APP_PROGRESS = ("orch:", "[agent]", "READY", "NSN ready", "PROVISION READY", "PONG")

    def __init__(self) -> None:
        self.rst_since_progress = 0
        # A reset marker (rst:) is the boot stream's own proof that the chip
        # actually restarted this cycle - the honest oracle _confirm_boot reads
        # instead of racing STATUS through the post-boot SD scan (bench rerun 2).
        self.saw_reset = False

    def feed(self, line: str):
        """Classify one line: ``("ready", mode, ip)`` / ``("legacy", None, None)``
        / ``"panic"`` / ``"loop"`` / ``"progress"``."""
        if any(marker in line for marker in PANIC_MARKERS):
            return "panic"
        if line.startswith("rst:"):
            self.saw_reset = True
            self.rst_since_progress += 1
            return "loop" if self.rst_since_progress >= 2 else "progress"
        m = re.search(r"READY\s+mode=(?P<mode>\d+)\s+ip=(?P<ip>\S+)", line)
        if m:
            return ("ready", int(m.group("mode")), m.group("ip"))
        if "NSN ready" in line or "PROVISION READY" in line:
            return ("legacy", None, None)
        if any(p in line for p in self._APP_PROGRESS):
            self.rst_since_progress = 0
        return "progress"


class _BootResult:
    """What one boot-stream scan showed, beyond just (mode, ip): whether a reset
    marker was seen (the fresh-boot proof), whether the READY beacon was reached
    (directly or via a PING fallback when the reopen ate it), and the wall-clock
    time the first rst: line arrived (so a caller can size a boot-recency uptime
    from the stream when STATUS is still busy with the SD scan)."""

    __slots__ = ("mode", "ip", "saw_reset", "ready", "reset_at")

    def __init__(self, mode, ip, saw_reset, ready, reset_at):
        self.mode = mode
        self.ip = ip
        self.saw_reset = saw_reset
        self.ready = ready
        self.reset_at = reset_at


class Device:
    """Serial link to one Nimbus device.

    Construction records config only; ``open()`` (or ``reset()``) touches hardware.
    """

    def __init__(
        self,
        port: Optional[str] = None,
        flash_env: Optional[str] = None,
        baud: int = BAUD,
        reset_settle: float = 0.4,
        enumerate_timeout: float = 8.0,
    ):
        self._pinned_port = port  # explicit --port, if any
        self.port = port  # the port currently in use (may change)
        self.flash_env = flash_env
        self.baud = baud
        self.reset_settle = reset_settle
        self.enumerate_timeout = enumerate_timeout
        self._ser = None  # type: ignore[assignment]
        self._transcript: List[str] = []  # rolling capture for debuggable failures
        self._pushback: List[str] = []  # lines replayed to the next _readline()
        # Set by the conftest when the run reaches the board over HTTP only and
        # the console was never opened (see _require_open).
        self.lan_only = False

    # -- port discovery ------------------------------------------------------
    def _glob_ports(self) -> List[str]:
        return list_ports()

    def _resolve_port(self) -> str:
        """Pick the serial node. Prefer the pinned --port if present; else the
        sole match. Ambiguous multi-match without a pin -> LOUD error (never
        guess silently)."""
        ports = self._glob_ports()
        if self._pinned_port:
            if self._pinned_port in ports or os.path.exists(self._pinned_port):
                return self._pinned_port
            # pinned port not (yet) present - caller polls in reopen loop
            raise DeviceLostError(f"pinned port {self._pinned_port} not present; available: {ports}")
        if not ports:
            raise DeviceLostError(f"no serial node matches {PORT_GLOB}")
        if len(ports) == 1:
            return ports[0]
        raise DeviceError(
            f"ambiguous: {len(ports)} serial nodes match {PORT_GLOB} ({ports}); "
            "pass --port to disambiguate (refusing to guess)."
        )

    # -- open / close --------------------------------------------------------
    @staticmethod
    def bus_reset(skip_serial: "Optional[str]" = None, target_serial: "Optional[str]" = None) -> bool:
        """Programmatic unplug/replug: libusb bus reset of the S3 (tools/
        usb_reset.py). Clears the stale host-side CDC state behind every 'wedged'
        episode (verified live 2026-07-02). ~2 s; clears the USB link WITHOUT
        rebooting the chip.

        MAC-safe with TWO boards attached (CUM-418 item 6): a wedged S3 reports NO
        serial, so the target is chosen by SKIPPING the healthy board's MAC
        (``--skip``) or by naming the target's MAC (``--serial``); the board that is
        not the target is never touched. The MACs come from the args or the env
        (NIMBUS_HIL_OTHER_SERIAL = the board to skip, NIMBUS_HIL_TARGET_SERIAL = the
        target); with a single board attached, no filter is needed (sole match)."""
        script = os.path.join(os.path.dirname(__file__), "..", "..", "tools", "usb_reset.py")
        skip_serial = skip_serial or os.environ.get("NIMBUS_HIL_OTHER_SERIAL")
        target_serial = target_serial or os.environ.get("NIMBUS_HIL_TARGET_SERIAL")
        argv = ["python3", script]
        if target_serial:
            argv += ["--serial", target_serial]
        elif skip_serial:
            argv += ["--skip", skip_serial]
        try:
            r = subprocess.run(argv, capture_output=True, text=True, timeout=25)
            return r.returncode == 0
        except (OSError, subprocess.SubprocessError):
            return False

    @staticmethod
    def _open_quiet(port: str, baud: int):
        """Open the port WITHOUT asserting DTR/RTS.

        pySerial's default open asserts both control lines; on the S3's native
        USB-serial-JTAG that strobes the chip's reset circuitry - sometimes a
        spurious reboot, sometimes a WEDGED USB peripheral that only a physical
        replug recovers (this was the root cause behind the 'orchestrator hard
        hang' brick and every zombie-port episode; observed live 2026-07-02).
        Clearing dtr/rts BEFORE open makes attach a pure listen."""
        ser = serial.Serial()
        ser.port = port
        ser.baudrate = baud
        ser.timeout = 0.2
        ser.dtr = False
        ser.rts = False
        ser.open()
        return ser

    def open(self) -> "Device":
        if serial is None:
            raise DeviceError("pyserial not installed; `pip install pyserial`")
        self.port = self._resolve_port()
        self._ser = self._open_quiet(self.port, self.baud)
        time.sleep(0.2)  # settle
        # Same stale-endpoint hazard as reopen (observed at fixture start):
        # a first attach can land on a dead handle. Probe; one gentle re-attach.
        if not self._link_alive(2.0):
            self.close()
            time.sleep(1.0)
            self._ser = self._open_quiet(self._resolve_port(), self.baud)
            time.sleep(0.2)
        return self

    def close(self) -> None:
        if self._ser is not None:
            try:
                self._ser.close()
            finally:
                self._ser = None

    def _require_open(self):
        if self._ser is None:
            # LAN-only mode: the suite reaches the board over HTTP and the
            # console was never opened. A test that needs the console has not
            # FAILED - it cannot run. Reporting 30+ identical errors buries the
            # handful of real failures in the same run, which is how two genuine
            # ones were nearly missed while building the v3.7.0 suites. Skipping
            # is not silent: pytest names every skip and its reason.
            if self.lan_only:
                import pytest

                pytest.skip("needs the serial console; this run is LAN-only (NIMBUS_TEST_IP is set)")
            raise DeviceError("serial port not open; call open()/reset() first")
        return self._ser

    # -- reset + F13 re-enumeration -----------------------------------------
    def reset(self) -> "Device":
        """Reboot the board, robustly.

        A DTR/RTS pulse alone is UNRELIABLE on the S3's native USB-serial-JTAG
        once an app is running (observed live: pulse ignored, wait_ready() then
        times out against app heartbeats - the exact F13 flake). So:

        1. If the NIMBUS_TEST console answers (PING -> PONG), use its REBOOT
           command - a software restart that always takes effect.
        2. Otherwise fall back to the DTR/RTS pulse (freshly-flashed or
           console-less builds, where the pulse usually does work).
        Either way, reopen across the re-enumeration afterwards."""
        ser = self._require_open()
        soft = False
        # Three PING attempts: a reply can be delayed past one window when the
        # device is mid refresh burst or the first attach was marginal.
        for _ in range(3):
            try:
                self.drain(quiet=0.1)
                ser.write(b"PING\n")
                ser.flush()
                deadline = time.time() + 1.5
                while time.time() < deadline:
                    line = self._readline(deadline)
                    if line is not None and "PONG" in line:
                        ser.write(b"REBOOT\n")
                        ser.flush()
                        soft = True
                        break
            except (OSError, IOError):
                break
            if soft:
                break
            time.sleep(0.8)
        if not soft:
            # NO DTR/RTS pulse fallback - EVER (it can wedge the S3's USB; see
            # _open_quiet). Instead: SELF-HEAL. The recurring "wedge" is stale
            # HOST-side CDC driver state after rapid reconnects to a rebooting
            # device - a libusb bus reset (protocol-level replug) clears it in
            # ~2 s (verified live). Reset the bus, reopen, ask once more.
            self.bus_reset()
            self.close()
            time.sleep(2.0)
            self._ser = self._open_quiet(self._resolve_port(), self.baud)
            time.sleep(1.5)  # device reboots after the bus reset
            try:
                self.drain(quiet=0.2)
                ser = self._require_open()
                ser.write(b"PING\n")
                ser.flush()
                deadline = time.time() + 3.0
                while time.time() < deadline:
                    line = self._readline(deadline)
                    if line is not None and "PONG" in line:
                        ser.write(b"REBOOT\n")
                        ser.flush()
                        soft = True
                        break
            except (OSError, IOError):
                pass
        if not soft:
            raise DeviceError(
                "console unresponsive even after a libusb bus reset - the device "
                "needs physical recovery (power-cycle / BOOT+RST; see "
                "guided_recovery()) and a rerun."
            )
        # Do NOT drain after a reset: ESP.restart() reboots to the READY beacon in
        # well under a second, so a "boot noise" drain here would EAT the beacon
        # before wait_ready() runs (observed live: every post-reset wait_ready
        # timed out while the beacon sat in the drained bytes).
        return self.reopen_after_reenumerate(drain_boot=False)

    def reopen_after_reenumerate(self, drain_boot: bool = True) -> "Device":
        """THE F13 FIX. Close the handle, wait for the CDC endpoint to drop, then
        poll-reglob ``/dev/cu.usbmodem*`` until the node reappears (possibly under
        a different name) and reopen. ``drain_boot`` swallows boot noise for
        callers that just want a clean prompt; callers that intend to ASSERT on
        the boot stream (reset()->wait_ready()) must pass False or the READY
        beacon gets eaten. Raise DeviceLostError if nothing comes back within
        ``enumerate_timeout``."""
        self.close()
        time.sleep(self.reset_settle)
        deadline = time.time() + self.enumerate_timeout
        last_err: Optional[Exception] = None
        while time.time() < deadline:
            try:
                self.port = self._resolve_port()
                self._ser = self._open_quiet(self.port, self.baud)
                time.sleep(0.15)
                # LIVENESS PROBE - the node name can persist across the USB
                # re-enumeration while the freshly-opened handle is attached to
                # the DYING endpoint (observed live: reopen "succeeds", then 20 s
                # of <no lines received>). A handle only counts once it proves
                # itself: any output within 2.5 s (boot spew, beacon, heartbeat)
                # or a PING->PONG. Otherwise close and re-attach.
                if not self._link_alive(2.5):
                    self.close()
                    last_err = DeviceError("opened a stale CDC endpoint (silent)")
                    time.sleep(1.0)  # gentle: rapid open/close cycling stresses
                    continue  # the S3 CDC stack (suspected wedge factor)
                if drain_boot:
                    self.drain(quiet=0.3)
                return self
            except (DeviceLostError, DeviceError, OSError) as exc:
                last_err = exc
                time.sleep(1.0)
        raise DeviceLostError(
            f"serial node did not re-enumerate within {self.enumerate_timeout}s "
            f"(last: {last_err}); board may be bricked - see guided_recovery()"
        )

    def _link_alive(self, window: float) -> bool:
        """True once the open handle demonstrably talks: any line arrives within
        ``window`` seconds, else one PING->PONG round-trip (1.5 s). Every line
        consumed here is PUSHED BACK so a following wait_ready()/expect() still
        sees it - the liveness probe must never eat the READY beacon."""
        deadline = time.time() + window
        line = self._readline(deadline)
        if line is not None:
            self._pushback.append(line)
            return True
        try:
            self._require_open().write(b"PING\n")
            self._require_open().flush()
        except (OSError, IOError):
            return False
        line = self._readline(time.time() + 1.5)
        if line is not None:
            self._pushback.append(line)
            return True
        return False

    # -- low-level IO --------------------------------------------------------
    def send(self, line: str) -> None:
        ser = self._require_open()
        ser.write((line + "\n").encode("utf-8"))
        ser.flush()

    def _readline(self, deadline: float) -> Optional[str]:
        """Read one line honoring a wall-clock deadline. Returns None on timeout.
        Serves the pushback queue first (lines consumed by the reopen liveness
        probe are replayed here so nothing observable is ever lost)."""
        if self._pushback:
            return self._pushback.pop(0)
        ser = self._require_open()
        while time.time() < deadline:
            raw = ser.readline()  # honors pyserial per-read timeout (0.2 s)
            if not raw:
                continue
            text = raw.decode("utf-8", "replace").strip()
            if text:
                self._transcript.append(text)
                if len(self._transcript) > 500:
                    self._transcript = self._transcript[-500:]
                return text
        return None

    def drain(self, quiet: float = 0.3) -> None:
        """Read+discard until the stream is idle for ``quiet`` seconds. Used before
        sending a command so stale boot spew isn't mis-matched."""
        if self._ser is None:
            return
        end_by = time.time() + 2.0  # hard cap so a chatty device can't hang drain
        while time.time() < end_by:
            got = self._readline(time.time() + quiet)
            if got is None:
                return

    def expect(self, substr: str, timeout: float = 5.0) -> str:
        """Read lines until one CONTAINS ``substr`` or the deadline passes.
        Returns the matching line; raises ExpectTimeout with a transcript tail."""
        deadline = time.time() + timeout
        while True:
            line = self._readline(deadline)
            if line is None:
                raise ExpectTimeout(substr, self._transcript)
            if substr in line:
                return line

    def expect_re(self, pattern: str, timeout: float = 5.0) -> "re.Match[str]":
        """Like expect() but with ``re.search``. Returns the match object so the
        caller can pull capture groups (reason=, ip=, seg:N, ...)."""
        compiled: Pattern[str] = re.compile(pattern)
        deadline = time.time() + timeout
        while True:
            line = self._readline(deadline)
            if line is None:
                raise ExpectTimeout(pattern, self._transcript)
            m = compiled.search(line)
            if m:
                return m

    def cmd(self, line: str, expect: str, timeout: float = 5.0) -> str:
        """drain -> send -> expect. The common request/response shape."""
        self.drain(quiet=0.2)
        self.send(line)
        return self.expect(expect, timeout=timeout)

    def cmd_re(self, line: str, pattern: str, timeout: float = 5.0) -> "re.Match[str]":
        self.drain(quiet=0.2)
        self.send(line)
        return self.expect_re(pattern, timeout=timeout)

    # -- high-level affordances ---------------------------------------------
    def webtok(self) -> str:
        """``WEBTOK?`` -> the per-device 96-bit web auth token (hex). Raises on
        timeout. Used by Net to token-gate every /api request (owner-batch-2)."""
        self.send("WEBTOK?")
        m = self.expect_re(r"WEBTOK\s+([0-9a-fA-F]+)", timeout=8.0)
        return m.group(1)

    def ping(self, timeout: float = 3.0) -> bool:
        """``PING`` -> ``PONG``. Proves the firmware loop is alive (not hung)."""
        try:
            self.cmd("PING", "PONG", timeout=timeout)
            return True
        except ExpectTimeout:
            return False

    def status(self, timeout: float = 3.0) -> "re.Match[str]":
        """``STATUS`` -> parsed ``STATUS mode=.. wifi=.. ip=.. rssi=.. heap=..
        [minheap.. psram.. sd.. vec.. flashfull..] uptime=..``. The line grew
        extra fields (minheap/psram/sd/vec/flashfull) between heap and uptime, so
        we match heap then skip to uptime tolerantly."""
        return self.cmd_re(
            "STATUS",
            r"STATUS\s+(?:fw=\S+\s+)?(?:build=\S+\s+)?"
            r"mode=(?P<mode>\d+)\s+wifi=(?P<wifi>\d+)\s+"
            r"ip=(?P<ip>\S+)\s+rssi=(?P<rssi>-?\d+)\s+heap=(?P<heap>\d+)"
            r".*?uptime=(?P<up>\d+)",
            timeout=timeout,
        )

    def ensure_mode(self, mode: int, timeout: float = 30.0) -> "Device":
        """Put the device in operating ``mode`` (0=Notifier, 1=Orchestrator).
        No-op when already there; otherwise ``MODE <m>`` persists + reboots and
        we re-attach across the re-enumeration and confirm via the READY beacon.
        Notifier-path tests (nsn frames) call ensure_mode(0) first, so they hold
        on a device that booted in Orchestrator mode - and vice versa."""
        # The first STATUS can race the boot banner (the per-test fixture resets
        # the device), so the device may still be printing its boot lines when we
        # ask. Retry the mode read briefly until it responds with a STATUS line.
        cur = None
        deadline = time.monotonic() + 15.0
        while cur is None:
            try:
                cur = int(self.status(timeout=3.0).group("mode"))
            except ExpectTimeout:
                if time.monotonic() >= deadline:
                    raise
                time.sleep(0.5)
        if cur == mode:
            return self
        self.send(f"MODE {mode}")
        time.sleep(0.3)  # firmware flushes its ack, then restarts
        self.reopen_after_reenumerate(drain_boot=False)  # keep the READY beacon
        got, _ip = self.wait_ready(timeout=timeout)
        if got is not None and int(got) != mode:
            raise DeviceError(f"MODE {mode} did not stick (READY mode={got})")
        return self

    def render(self, timeout: float = 4.0) -> "RenderState":
        """``RENDER?`` -> parsed render summary (matches the firmware's actual
        ``RENDER screen=.. posture=.. seg=.. single=.. dark=.. bright=..`` line,
        observed live). ``ring`` classifies as dark | single | seg:N.

        One retry: the serial diagnostics channel is shared with the display
        task's prints, so a reply line can arrive torn/truncated under burst
        (observed live). A second ask on a quiet stream is reliable; asserting
        on the PARSED reply keeps this honest - a wrong VALUE still fails."""
        pat = (
            r"RENDER\s+screen=(?P<screen>\d+)\s+posture=(?P<posture>\d+)\s+"
            r"seg=(?P<seg>\d+)\s+single=(?P<single>\d+)\s+dark=(?P<dark>\d+)\s+"
            r"bright=(?P<bright>\d+)"
        )
        try:
            m = self.cmd_re("RENDER?", pat, timeout=timeout)
        except ExpectTimeout:
            m = self.cmd_re("RENDER?", pat, timeout=timeout)
        return RenderState(
            screen=int(m.group("screen")),
            posture=int(m.group("posture")),
            seg=int(m.group("seg")),
            single=m.group("single") == "1",
            dark=m.group("dark") == "1",
            bright=int(m.group("bright")),
        )

    def selftest(self, name: str, timeout: float = 20.0) -> "re.Match[str]":
        """``TEST <name>`` -> parsed ``RESULT <name> PASS|FAIL|SKIP <rest>``."""
        return self.cmd_re(
            f"TEST {name}",
            rf"RESULT\s+{re.escape(name)}\s+(?P<verdict>PASS|FAIL|SKIP)"
            r"(?P<rest>.*)$",
            timeout=timeout,
        )

    # Menu paint plus repaint coalescing on the menu's own busy window means a
    # nav step needs a short settle before RENDER? reflects it.
    MENU_SETTLE = 2.6

    def ble_state(self, timeout: float = 3.0) -> "tuple[int, int]":
        """``BLE?`` -> (enabled, connected). enabled=1 means advertising-enabled
        (Notifier mode + Connectivity>Bluetooth on); connected=1 means a central
        is linked. Lets a test assert the Bluetooth toggle drives the radio."""
        m = self.cmd_re("BLE?", r"BLE\s+enabled=(\d)\s+connected=(\d)", timeout=timeout)
        return int(m.group(1)), int(m.group(2))

    def ensure_status_idle(self, timeout: float = 30.0) -> None:
        """Soft precondition for menu tests: get the panel to StatusIdle WITHOUT a
        reset where possible (consecutive resets race the CDC reopen + WiFi rejoin -
        the documented flake), by tapping the header Back/Home target - the
        deliberate exit the firmware draws on EVERY screen that has a header
        (idle_nav_step / IDLE_HEADER_TAP). This replaces the old blind centre tap,
        which was dead space on Ask (screen=5, Close is bottom-centre) and the
        Config/Sign-in QR (screen=9) and so looped forever on the bench.

        A menu backs out one breadcrumb level per tap, so the screen id stays Menu
        while the MENU? view changes - that is progress, not a stall. A true stall
        (same screen AND same menu view across taps) or a no-tap screen (TouchCal)
        escalates to ONE confirmed restart, which lands a provisioned board on
        StatusIdle; a board that then still is not idle raises loudly."""
        menu_id = SCREEN_NAMES.index("Menu")
        deadline = time.time() + timeout
        rebooted = False
        last_sig = None
        stall = 0
        while time.time() < deadline:
            r = self.render()
            step = idle_nav_step(r.screen)
            if step == "idle":
                return
            # Signature for stall detection: the screen id, plus the menu view when
            # on the Menu screen (backing out changes the view, not the id).
            menu_view = self.cmd("MENU?", "MENU ", timeout=4.0) if r.screen == menu_id else ""
            sig = (r.screen, menu_view)
            stall = stall + 1 if sig == last_sig else 0
            last_sig = sig
            if step == "restart" or stall >= 3:
                if rebooted:
                    break  # restarted once already and still not idle -> loud timeout
                # A confirmed restart lands a provisioned board on StatusIdle. It can
                # run longer than the outer deadline (esptool escalation), so check
                # the post-reboot screen here and return if idle - do NOT let the
                # while-deadline expire mid-recovery and raise on an idle board.
                self.reboot_and_confirm(timeout=max(30.0, timeout))
                rebooted = True
                last_sig = None
                stall = 0
                if idle_nav_step(self.render().screen) == "idle":
                    return
                deadline = time.time() + timeout  # fresh budget to clear a post-boot screen
                continue
            x, y = IDLE_HEADER_TAP
            self.cmd(f"TAP {x} {y}", "TAP<", timeout=5.0)
            time.sleep(self.MENU_SETTLE)
        stuck = SCREEN_NAMES[last_sig[0]] if last_sig and 0 <= last_sig[0] < len(SCREEN_NAMES) else "?"
        raise ExpectTimeout(f"StatusIdle precondition (stuck on {stuck})", self._transcript)

    def menu_wait_screen(self, screen: int, timeout: float = 8.0) -> "RenderState":
        """Poll RENDER? until the panel reports ``screen``; return that state.
        Raises ExpectTimeout on failure (a real 'menu never appeared' assertion)."""
        deadline = time.time() + timeout
        last = None
        while time.time() < deadline:
            last = self.render()
            if last.screen == screen:
                return last
            time.sleep(0.4)
        raise ExpectTimeout(f"screen={screen} (last {last!r})", self._transcript)

    def turn(self, text: str) -> None:
        """``TURN <text>`` - fire one orchestrator turn (Orchestrator mode)."""
        self.send(f"TURN {text}")

    def wifi(self, ssid: str, password: str) -> None:
        """``WIFI <ssid>|<pass>`` (NIMBUS_TEST). Provision creds over serial."""
        self.send(f"WIFI {ssid}|{password}")

    # -- fresh-device / first-run helpers (CUM-245, F5) ---------------------
    def factory_reset(self, timeout: float = 45.0):
        """``FACTRESET`` -> the OUT-OF-BOX (default / absent-NVS) state, then a fresh boot.

        The same deferred seam as web ``POST /api/factory-reset``: the main loop erases every
        config namespace (KEEPING only the board's physical identity) and reboots. Returns
        ``(mode, ip)`` from the fresh ``READY`` beacon.

        ⚠ DESTRUCTIVE: wipes Wi-Fi creds, the onboarded flag, provider keys, the token and
        every owner setting. It KEEPS the hardware identity (scrModel, tftFlip, tchCal,
        otaType; CUM-50/CUM-230) so a reset board never comes back on the wrong driver. A
        once-calibrated board therefore still has its touch cal after this; use
        ``clear_touch_cal()`` for the out-of-box TOUCH state. The board comes back OFF its
        LAN; re-provision Wi-Fi in teardown (``device.wifi(ssid, pass)``). The reboot
        re-enumerates the USB-CDC endpoint, so this reopens across it (keeping the boot
        stream) and asserts the beacon like ``reset()``."""
        self.cmd("FACTRESET", "FACTRESET", timeout=8.0)  # ack; main loop erases + reboots
        # Do NOT drain: the erase-and-restart reaches READY quickly, so keep the beacon.
        self.reopen_after_reenumerate(drain_boot=False)
        return self.wait_ready(timeout=timeout)

    def clear_touch_cal(self, timeout: float = 45.0):
        """``CALGATE clear`` (NIMBUS_TEST) -> erase the stored touch cal and restart, so the
        next boot is the genuine out-of-box TOUCH state (gate armed on a resistive panel).
        Needed because ``factory_reset()`` keeps tchCal as hardware identity. Returns
        ``(mode, ip)`` from the fresh ``READY`` beacon, like ``factory_reset()``."""
        self.cmd("CALGATE clear", "CALGATE cleared=1", timeout=8.0)
        self.reopen_after_reenumerate(drain_boot=False)
        return self.wait_ready(timeout=timeout)

    _CALGATE_RE = r"CALGATE\s+active=(?P<active>\d)\s+kind=(?P<kind>\w+)\s+stored=(?P<stored>\d)"

    def cal_gate(self, op: str = "", timeout: float = 6.0):
        """``CALGATE`` / ``CALGATE skip`` (NIMBUS_TEST) - the first-run touch-cal GATE.

        ``op=""`` (or ``"?"``) queries the live gate; ``op="skip"`` drives the DELIBERATE
        long-hold skip the same way an on-glass hold does (``serviceCalGate`` reads the raw
        panel, not the injectable TAP buffer, so the skip is otherwise undriveable from the
        console). The four-corner SOLVE stays a finger-on-glass leg.

        Returns ``(active: bool, kind: str, stored: bool)``: whether the gate owns the panel,
        the touch class ("res"|"cap"), and whether a calibration is persisted."""
        arg = " skip" if op == "skip" else "?"  # "clear" has its own helper (it restarts)
        m = self.cmd_re("CALGATE" + arg, self._CALGATE_RE, timeout=timeout)
        return (m.group("active") == "1", m.group("kind"), m.group("stored") == "1")

    def tap(self, x: int, y: int, hold: bool = False, timeout: float = 5.0) -> str:
        """``TAP <x> <y> [HOLD]`` -> the ``TAP<`` ack. A synthetic press-release (or, with
        ``hold``, a press held until ``TAPUP``) at a panel coordinate. Injected taps take the
        ``drainTouch`` path, so a tap issued while the first-run cal GATE owns the panel is
        (correctly) inert - which is exactly the "taps do not navigate while gated" assertion."""
        return self.cmd(f"TAP {x} {y}{' HOLD' if hold else ''}", "TAP<", timeout=timeout)

    def tftfill_ok(self, timeout: float = 8.0) -> bool:
        """``TFTFILL?`` drives the REAL full-panel fill path and reads pixels back from the far
        corners (GRAM readback), for red/green/blue. Returns True only if all three round-trip
        (every line ends ``OK``) - the honest "render reaches the frame" check, distinct from a
        golden (which only proves bytes match a capture). Raises on a torn/short reply."""
        self.drain(quiet=0.2)
        self.send("TFTFILL?")
        seen = {}
        for _ in range(3):
            m = self.expect_re(r"TFTFILL\s+(?P<name>\w+)\s+want=.*\s+(?P<verdict>OK|MISMATCH)\s*$", timeout=timeout)
            seen[m.group("name")] = m.group("verdict")
        return len(seen) == 3 and all(v == "OK" for v in seen.values())

    # -- boot capture + beacon ----------------------------------------------
    def wait_ready(self, timeout: float = 20.0, max_total: "Optional[float]" = None):
        """Read the boot stream after a reset; FAIL on a panic or reboot loop,
        SUCCEED on the ``READY mode=<n> ip=<..>`` beacon (or the older
        ``NSN ready`` / ``PROVISION READY`` markers). Returns (mode, ip) - mode is
        None for the legacy markers.

        SIZED FROM THE BOOT STREAM, not a fixed number: each boot line that arrives
        re-arms an idle window of ``timeout`` seconds, so a slow boot (an SD card
        plus a 500+ row episodic scan pushes 'orch: done' well past a flat 20 s -
        the L7 watchdog symptom on the bench) is not clipped mid-boot, while a
        SILENT board still fails after ``timeout`` of no output and a reboot LOOP
        (>= 2 rst: with no progress between) still fails fast. ``max_total`` bounds
        the absolute wait (default 6x the idle window, min 120 s) so even a chatty
        loop cannot hang the suite.

        Reboot-loop detector: see _BootScan (>= 2 ``rst:`` with no app progress)."""
        r = self._scan_boot(timeout, max_total)
        return r.mode, r.ip

    def _scan_boot(self, timeout: float = 20.0, max_total: "Optional[float]" = None) -> "_BootResult":
        """The boot-stream reader behind wait_ready, but REPORTING what the stream
        showed (a _BootResult) rather than only (mode, ip). Same failure contract:
        BootError on a panic / reboot loop, ExpectTimeout on a silent board. It
        records ``saw_reset`` (a ``rst:`` line arrived - the chip really restarted)
        and ``reset_at`` (when the first one did), so _confirm_boot can trust the
        stream over a STATUS read that the post-boot SD scan is still blocking."""
        if max_total is None:
            max_total = max(timeout * 6, 120.0)
        hard_deadline = time.time() + max_total
        scan = _BootScan()
        reset_at: Optional[float] = None
        while True:
            line = self._readline(min(time.time() + timeout, hard_deadline))
            if line is None:
                # No output within the idle window: either the beacon was already
                # CONSUMED by the reopen (the CDC reopen races the boot stream on
                # consecutive resets - a board that answers PING is ready regardless
                # of who read the beacon), or the board is silent/bricked.
                if self.ping():
                    return _BootResult(None, None, scan.saw_reset, True, reset_at)
                if time.time() >= hard_deadline:
                    raise ExpectTimeout("READY beacon (boot did not settle within the ceiling)", self._transcript)
                raise ExpectTimeout("READY beacon", self._transcript)
            verdict = scan.feed(line)
            if reset_at is None and scan.saw_reset:
                reset_at = time.time()
            if verdict == "panic":
                raise BootError(f"panic during boot: {line!r}")
            if verdict == "loop":
                raise BootError("reboot loop: >= 2 'rst:' lines with no boot progress between")
            if isinstance(verdict, tuple):  # ("ready"|"legacy", mode, ip)
                return _BootResult(verdict[1], verdict[2], scan.saw_reset, True, reset_at)
            # "progress": a boot line arrived; the top of the loop re-arms the idle
            # window from now, so an actively-booting board keeps its wait alive.

    # -- watchdog / reboot hooks --------------------------------------------
    def hang(self) -> None:
        """``HANG`` - test-only: wedge the loop to prove the watchdog (F12)."""
        self.send("HANG")

    def wait_reboot(self, timeout: float = 15.0):
        """Expect the device to reset and re-beacon within ``timeout`` (watchdog +
        reflash tests). Reopens across the re-enumeration first, then waits READY."""
        # A watchdog reset re-enumerates the CDC endpoint just like a manual reset.
        self.reopen_after_reenumerate()
        return self.wait_ready(timeout=timeout)

    # -- confirmed reboot (native USB-CDC) ----------------------------------
    def _uptime_or_none(self, timeout: float = 4.0) -> "Optional[int]":
        """Read STATUS uptime, or None if the console does not answer in time."""
        try:
            return int(self.status(timeout=timeout).group("up"))
        except (ExpectTimeout, DeviceError):
            return None

    def _settled_uptime(self, timeout: float = 25.0) -> "Optional[int]":
        """Poll STATUS until the console answers (the board may still be booting -
        a bare status() right after a reset RACES the boot stream, the CUM-418
        test_boots symptom) and return that uptime, else None on the deadline."""
        deadline = time.time() + timeout
        while time.time() < deadline:
            up = self._uptime_or_none(timeout=4.0)
            if up is not None:
                return up
            time.sleep(0.5)
        return None

    def esptool_hard_reset(self) -> bool:
        """Hard-reset the chip with esptool over native USB-CDC. Used two ways: as
        the escalation when a soft console REBOOT is a no-op (reboot_and_confirm),
        and as the second rung of the wedge recovery when a libusb bus reset alone
        does not bring the console back (wedge_guard). A DTR/RTS pulse would risk
        wedging the S3 (see _open_quiet), so esptool drives the chip's reset line
        instead. ``chip_id`` just drives a connect + the reset sequence (--before
        default-reset to enter, --after hard-reset to leave). Targets THIS device's
        own port (never the other board when two are attached), closes it first
        (esptool needs it), and reports whether any invocation succeeded.

        ALWAYS reopens the console afterwards - success OR failure. esptool holds
        the port exclusively (so this closes it first) and, on success, drives a
        re-enumerating hard reset; leaving the handle closed on the failure path is
        what cascaded into 15 'serial port not open' failures for every test after a
        failed escalation (bench rerun 2 item 2). The reopen is in a finally so a
        no-op escalation never poisons the rest of the session."""
        port = self.port or self._pinned_port
        if not port:
            return False
        self.close()
        base = ["--chip", "esp32s3", "--port", port, "--before", "default-reset", "--after", "hard-reset", "chip_id"]
        ok = False
        try:
            for prefix in (["esptool.py"], [sys.executable, "-m", "esptool"], ["esptool"]):
                try:
                    r = subprocess.run(prefix + base, capture_output=True, text=True, timeout=40)
                except (OSError, subprocess.SubprocessError):
                    continue
                if r.returncode == 0:
                    ok = True
                    break
        finally:
            # Bring the console back no matter what. drain_boot=False keeps the boot
            # stream intact for the caller's confirm. A board that genuinely did not
            # re-enumerate raises DeviceLostError, which the caller's confirm and the
            # wedge sentinel handle - but a reachable board is never left closed.
            try:
                self.reopen_after_reenumerate(drain_boot=False)
            except DeviceLostError:
                pass
        return ok

    def _confirm_boot(self, before: "Optional[int]", timeout: float) -> "Optional[int]":
        """Confirm a reboot actually took by reading the boot stream it produces.

        The bench-rerun-2 bug: the old confirm polled STATUS for a fresh uptime, but
        a console REBOOT on this board reliably prints ``rst:.. -> READY`` in ~12 s
        and then runs a heavy SD episodic scan that keeps STATUS from answering for
        many seconds - so a genuine reboot read back uptime=None, was wrongly
        escalated to a hard reset, and then declared a failure ("uptime 2857s ->
        None"). The boot stream is the honest oracle: a reset marker (rst:) followed
        by READY IS proof of a fresh boot, whatever STATUS says next.

        Returns a fresh uptime on a confirmed boot - the real value when STATUS
        answers promptly, else the small boot-recency wall clock since the reset
        marker - or None so the caller escalates. Propagates BootError on a panic /
        reboot loop."""
        try:
            boot = self._scan_boot(timeout=timeout)
        except ExpectTimeout:
            boot = None
        if boot is not None and boot.saw_reset:
            # The stream proved the restart. Report a fresh uptime WITHOUT blocking
            # on STATUS through the SD scan (that race is exactly what read None).
            up = self._uptime_or_none(timeout=4.0)
            if up is not None and up < FRESH_BOOT_CEILING_S:
                return up
            if boot.reset_at is not None:
                return max(0, int(time.time() - boot.reset_at))
            return 0
        # No reset marker in the stream: the beacon may have been eaten, or the soft
        # REBOOT was a no-op. Fall back to the uptime oracle - a no-op reboot leaves
        # uptime climbing (not fresh -> None -> the caller escalates).
        up = self._settled_uptime(timeout)
        return up if is_fresh_boot(before, up) else None

    def reboot_and_confirm(self, timeout: float = 25.0) -> int:
        """Reboot and PROVE a fresh boot on native USB-CDC, escalating if the soft
        path is a no-op.

        A plain reset()+status() cannot tell a real restart from a REBOOT the chip
        ignored (uptime keeps climbing) - the CUM-418 bench saw uptime=3254s "after
        a reboot". This does the soft console REBOOT (reset(), which also self-heals
        a wedged console with a bus reset), confirms the boot actually happened by
        reading the boot stream it produces (rst: -> READY, not a STATUS poll the SD
        scan blocks), and if it did not, escalates to an esptool hard reset and
        confirms again. Returns the fresh uptime; raises DeviceError if the board
        would not restart either way, and propagates BootError on a panic / loop."""
        before = self._uptime_or_none()
        self.reset()
        up = self._confirm_boot(before, timeout)
        if up is not None:
            return up
        # The soft REBOOT did not take (native USB-CDC no-op). Escalate to an esptool
        # hard reset, which drives the chip's reset line, then re-confirm.
        # esptool_hard_reset reopens the console itself (even on failure), so the
        # escalation never leaves the port closed for the rest of the session.
        if self.esptool_hard_reset():
            up = self._confirm_boot(before, timeout)
            if up is not None:
                return up
        raise DeviceError(
            f"reboot did not take a fresh boot: uptime {before}s -> {up}s after a "
            "console REBOOT and an esptool hard reset (device may need BOOT+RST)."
        )

    # -- flashing (DOUBLE-INTERLOCKED; dormant while board is single-owner) --
    def flash_env(self, env: str, allow_hardware: bool = False) -> None:
        """``pio run -e <env> -t upload``. GUARDED: refuses unless BOTH
        ``--allow-hardware`` is set AND ``NIMBUS_HIL_FLASH_OK=1`` is in the env.
        The constraints forbid flashing while the board is bricked/single-owner, so
        this stays dormant by default and never runs at collection time."""
        if not allow_hardware:
            raise DeviceError("flash refused: --allow-hardware not set")
        if os.environ.get("NIMBUS_HIL_FLASH_OK") != "1":
            raise DeviceError(
                "flash refused: set NIMBUS_HIL_FLASH_OK=1 to arm flashing "
                "(double interlock - board is single-owner/bricked)"
            )
        repo_root = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
        subprocess.run(["pio", "run", "-e", env, "-t", "upload"], cwd=repo_root, check=True)
        self.reopen_after_reenumerate()
        self.wait_ready()

    # -- guided BOOT+RESET recovery (F12/F13) -------------------------------
    def guided_recovery(self, manual, reason: str) -> None:
        """Print the BOOT+RESET download-mode runbook and block on operator
        confirmation via the manual mechanism. Unconfirmed -> LOUD fail (never
        proceed as if recovered). ``manual`` is a ManualStep."""
        runbook = (
            f"DEVICE RECOVERY NEEDED ({reason}).\n"
            ">>> 1) HOLD the BOOT button.\n"
            ">>> 2) TAP the RESET button.\n"
            ">>> 3) RELEASE BOOT - the device enumerates in download mode.\n"
            ">>> Confirm when the port has re-appeared"
        )
        manual.confirm(runbook, timeout=120)
        # Operator says it's back - re-establish the link or fail loud.
        self.reopen_after_reenumerate()
