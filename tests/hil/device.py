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
    "TokenDetail",
    "TouchCal",
)


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
    def bus_reset() -> bool:
        """Programmatic unplug/replug: libusb bus reset of the S3 (tools/
        usb_reset.py). Clears the stale host-side CDC state behind every
        'wedged' episode (verified live 2026-07-02). ~2 s; device reboots."""
        script = os.path.join(os.path.dirname(__file__), "..", "..", "tools", "usb_reset.py")
        try:
            r = subprocess.run(["python3", script], capture_output=True, text=True, timeout=20)
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

    def ensure_status_idle(self, timeout: float = 25.0) -> None:
        """Soft precondition for menu tests: get the panel to StatusIdle WITHOUT a
        reset (consecutive resets race the CDC reopen + WiFi rejoin - the
        documented flake). Taps Back (top-left header) to back out of an open
        menu, taps the panel center to dismiss the screensaver/other screens,
        then waits for screen 0."""
        deadline = time.time() + timeout
        while time.time() < deadline:
            r = self.render()
            if r.screen == 0:
                return
            if r.screen == 3:  # Menu: a Back tap (top-left header) backs out
                self.cmd("TAP 20 22", "TAP<", timeout=5.0)
            else:  # Screensaver/detail/...: a tap wakes to status
                self.cmd("TAP 160 120", "TAP<", timeout=5.0)
            time.sleep(self.MENU_SETTLE)
        raise ExpectTimeout("StatusIdle precondition", self._transcript)

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

    # -- boot capture + beacon ----------------------------------------------
    def wait_ready(self, timeout: float = 20.0):
        """Read the boot stream after a reset; FAIL on a panic or reboot-loop,
        SUCCEED on the ``READY mode=<n> ip=<..>`` beacon (or the older
        ``NSN ready`` / ``PROVISION READY`` markers). Returns (mode, ip) - mode is
        None for the legacy markers.

        Reboot-loop detector: >= 2 ``rst:`` lines within the window is a loop."""
        deadline = time.time() + timeout
        rst_count = 0
        while True:
            line = self._readline(deadline)
            if line is None:
                # The beacon can be CONSUMED before this expect starts (the CDC
                # reopen races the boot stream on consecutive resets - torn
                # serial is the documented reality). A device that already
                # answers PING is ready regardless of who read the beacon.
                if self.ping():
                    return (None, None)
                raise ExpectTimeout("READY beacon", self._transcript)
            if any(marker in line for marker in PANIC_MARKERS):
                raise BootError(f"panic during boot: {line!r}")
            if line.startswith("rst:"):
                rst_count += 1
                if rst_count >= 2:
                    raise BootError(f"reboot loop: >= {rst_count} 'rst:' lines in boot window")
            m = re.search(r"READY\s+mode=(?P<mode>\d+)\s+ip=(?P<ip>\S+)", line)
            if m:
                return int(m.group("mode")), m.group("ip")
            # Legacy beacons (builds without NIMBUS_TEST): mode unknown.
            if "NSN ready" in line or "PROVISION READY" in line:
                return None, None

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
