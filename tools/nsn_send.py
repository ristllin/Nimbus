#!/usr/bin/env python3
"""E2E hardware validation: encode nsn frames with the REAL broker encoder
(nsnotify/notify/broker/frame.py, https://github.com/ristllin/nsnotify) and
send them to the Nimbus device over BLE, reading back the device's
NIMBUS_NOTIFIER_DEBUG status echo over serial (diagnostics only - USB carries
no frame data now; BLE is the sole nsn transport, see AGENTS.md).

Requires bleak with a working Bluetooth entitlement in THIS process (macOS:
run from a shell where `python3 -c "from bleak import BleakScanner"` already
works interactively - a bare sandboxed/headless process typically lacks the
Info.plist usage-description CoreBluetooth needs and will hard-crash).

Usage: nsn_send.py [serial-port]   (default /dev/cu.usbmodem101; only used for
                                     the optional status echo readback)
"""

import asyncio
import os
import sys
import time
from pathlib import Path

NOTIFY_HOST = Path(
    os.environ.get("NIMBUS_NOTIFY_HOST", str(Path(__file__).resolve().parent.parent.parent / "nsnotify"))
)
sys.path.insert(0, str(NOTIFY_HOST))

from bleak import BleakClient, BleakScanner  # noqa: E402
from notify.broker.frame import FrameSegment, encode_frame  # noqa: E402
from notify.state import Anim, State  # noqa: E402

try:
    import serial  # optional - only for the status-echo readback
except ImportError:
    serial = None

PORT = sys.argv[1] if len(sys.argv) > 1 else "/dev/cu.usbmodem101"

SVC = "e20b0001-9463-42a9-aaf8-8aa1fd518d52"
FRAME = "e20b0002-9463-42a9-aaf8-8aa1fd518d52"
STATUS = "e20b0003-9463-42a9-aaf8-8aa1fd518d52"

# Three scenarios the device should report distinctly.
SCENARIOS = [
    (
        "two running",
        [
            FrameSegment(State.Running, 170, Anim.Comet, 0),
            FrameSegment(State.Running, 85, Anim.Comet, 0),
        ],
        60,
    ),
    (
        "add an approval (attention)",
        [
            FrameSegment(State.Running, 170, Anim.Comet, 0),
            FrameSegment(State.Running, 85, Anim.Comet, 0),
            FrameSegment(State.AwaitingApproval, 32, Anim.Blink, 0),
        ],
        90,
    ),
    ("clear all", [], 0),
]


def _open_status_serial():
    """Quiet-open the serial console (diagnostics only - never asserts DTR/RTS,
    which can wedge the S3's USB-serial-JTAG peripheral; see AGENTS.md)."""
    if serial is None:
        return None
    try:
        s = serial.Serial()
        s.port, s.baudrate, s.timeout = PORT, 115200, 0.3
        s.dtr = False
        s.rts = False
        s.open()
        time.sleep(0.3)
        s.reset_input_buffer()
        return s
    except Exception as exc:  # serial console is optional; BLE is what matters
        print(f"(no serial status echo: {exc})")
        return None


async def main():
    print("scanning for 'Nimbus' (10s)...")
    dev = await BleakScanner.find_device_by_name("Nimbus", timeout=10.0)
    if not dev:
        dev = await BleakScanner.find_device_by_filter(lambda d, ad: SVC in (ad.service_uuids or []), timeout=10.0)
    if not dev:
        print("!! device not found over BLE - is it in Notifier mode with Bluetooth on (Settings > Connectivity)?")
        return
    print(f"found: {dev.address} {dev.name}")

    ser = _open_status_serial()

    async with BleakClient(dev) as c:
        print(f"connected. mtu={c.mtu_size}")
        seq = 0
        for name, segs, bright in SCENARIOS:
            print(f"\n>>> sending: {name}  ({len(segs)} segs, bright={bright})")
            pkt = encode_frame(segs, bright, seq)
            await c.write_gatt_char(FRAME, pkt, response=False)
            seq = (seq + 1) & 0xFF
            await asyncio.sleep(0.6)  # apply + status echo
            if ser is not None:
                deadline = time.time() + 1.0
                while time.time() < deadline:
                    line = ser.readline().decode("utf-8", "replace").strip()
                    if line:
                        print(f"    device: {line}")

    if ser is not None:
        ser.close()


if __name__ == "__main__":
    asyncio.run(main())
