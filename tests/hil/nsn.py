"""nsn frame injection for the notifier/render HIL tests (the HIL test spec).

BLE-only (2026-07): the device's USB serial no longer carries nsn frames (power
+ flashing only - see AGENTS.md); Bluetooth LE is the sole status transport.
This module drives the SAME GATT path the real nsnotify broker uses -
``notify.transport.ble_tx.BleTransport`` (https://github.com/ristllin/nsnotify)
- rather than reinventing BLE connection management here: same scan/connect/
retry/backoff the product actually ships, so a harness bug can't paper over a
broker bug or vice versa. Frames are encoded with the REAL broker encoder
(``notify.broker.frame``), the same path ``tools/nsn_send.py`` uses.

The notify-host import is DEFERRED to call time (``_load_broker``) so importing
this module - and collecting the suite - never requires the notify host package
or a working Bluetooth stack on the box. A test that needs it and can't get it
gets a LOUD skip, never a silent pass.

NOTE: bleak's CoreBluetooth backend on macOS hard-crashes (SIGABRT) a process
whose Info.plist lacks NSBluetoothAlwaysUsageDescription - this happens BEFORE
any Python exception can be caught, so a box without that entitlement can't be
guarded against in-process. Run this suite from an environment where a bare
`python3 -c "from bleak import BleakScanner"` round-trip already works (verified
once interactively) - not from a sandboxed/headless process lacking it.
"""

from __future__ import annotations

import os
import sys
import time
from pathlib import Path

# The notify host package (sibling repo, public). Overridable for CI.
NOTIFY_HOST = Path(os.environ.get("NIMBUS_NOTIFY_HOST", str(Path.home() / "Projects" / "nsnotify")))

# How long to wait for the BLE worker thread to scan + connect before the first
# send. BleTransport's own SCAN_TIMEOUT_S/CONNECT_TIMEOUT_S bound the attempt;
# this is the outer wait so a caller doesn't have to poll.
CONNECT_WAIT_S = 20.0


class BrokerUnavailable(RuntimeError):
    """The nsn broker encoder/BLE transport could not be imported; tests turn
    this into a loud skip with a reason - never a silent pass."""


def _load_broker():
    """Import (FrameSegment, encode_frame, State, Anim, BleTransport) from the
    notify host, or raise BrokerUnavailable. Import is deferred so collection
    stays clean."""
    if str(NOTIFY_HOST) not in sys.path:
        sys.path.insert(0, str(NOTIFY_HOST))
    try:
        from notify.broker.frame import FrameSegment, encode_frame
        from notify.state import Anim, State
        from notify.transport.ble_tx import BleTransport
    except ImportError as exc:  # notify host not installed / not at expected path
        raise BrokerUnavailable(
            f"nsn broker encoder/BLE transport not importable from {NOTIFY_HOST} "
            f"({exc}); set NIMBUS_NOTIFY_HOST to the nsnotify repo dir"
        ) from exc
    return FrameSegment, encode_frame, State, Anim, BleTransport


class NsnInjector:
    """Encode + write nsn frames to the device over BLE.

    Connects lazily on first use (not in __init__) so constructing the fixture
    never blocks collection; the first send() call pays the scan+connect cost
    (~5-15s cold). Callers that need certainty a frame was actually applied
    should follow send_frame()/send_raw() with a device-side assertion
    (RENDER?/STATUS/an expect_re on a debug echo) - this class only guarantees
    the write reached a connected peripheral, not that the device parsed it
    (mirrors the production broker's own best-effort send() contract).
    """

    def __init__(self, device):
        self.device = device
        self._seq = 0
        (self.FrameSegment, self._encode, self.State, self.Anim, BleTransport) = _load_broker()
        self._ble = BleTransport()  # starts scanning/connecting immediately (bg thread)
        self._connected_once = False

    def _ensure_connected(self, timeout: float = CONNECT_WAIT_S) -> None:
        if self._connected_once:
            return
        # BleTransport exposes connection state via a threading.Event; there is
        # no public wait_connected() on the sibling package (deliberately not
        # widening its API for this harness), so we poll the internal Event
        # directly - acceptable for test code that already pins the sibling
        # repo's protocol/GATT UUIDs by hand.
        deadline = time.time() + timeout
        while time.time() < deadline:
            if self._ble._connected.is_set():  # noqa: SLF001 - see docstring above
                self._connected_once = True
                return
            time.sleep(0.2)
        raise TimeoutError(
            f"BLE did not connect within {timeout}s - is the device in Notifier "
            "mode with Bluetooth on (Settings > Connectivity)?"
        )

    def close(self) -> None:
        self._ble.close()

    def _write_raw(self, data: bytes) -> None:
        self._ensure_connected()
        ok = self._ble.send(data)
        # send() is fire-and-forget (LATEST-WINS mailbox): a second send() before
        # the worker loop's write cycle has drained the first can silently
        # supersede it - nothing wrong, but a test firing several send_raw()
        # calls back-to-back (e.g. malformed-frame fuzzing) needs enough settle
        # per call that each one gets a real shot at going out over the air
        # before the next overwrites it, not just enough to avoid a local error.
        time.sleep(1.0)
        if not ok:
            raise RuntimeError("BLE send() reported not-connected - link dropped between connect and send")

    def encode(self, segments, brightness: int, seq: int) -> bytes:
        """Encode a frame to raw wire bytes WITHOUT sending (used by the
        malformed-frame test to build a well-formed frame then corrupt its CRC)."""
        return self._encode(segments, brightness, seq)

    def send_frame(self, segments, brightness: int) -> None:
        """Encode + send one frame with the next sequence number."""
        self._write_raw(self._encode(segments, brightness, self._seq))
        self._seq = (self._seq + 1) & 0xFF

    def send_raw(self, data: bytes) -> None:
        """Send arbitrary bytes (malformed-frame robustness test, F19/L7)."""
        self._write_raw(data)

    # -- convenience constructors mirroring tools/nsn_send.py scenarios ---------
    def running(self, hue: int, span: int = 0):
        return self.FrameSegment(self.State.Running, hue, self.Anim.Comet, span)

    def awaiting_approval(self, hue: int = 32, span: int = 0):
        return self.FrameSegment(self.State.AwaitingApproval, hue, self.Anim.Blink, span)
