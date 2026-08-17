#!/usr/bin/env python3
"""Programmatic unbrick for the wedged ESP32-S3 USB-serial-JTAG.

The S3's USB peripheral can wedge (serial silent, esptool AND built-in JTAG
dead) - historically only a physical replug recovered it. A libusb bus-level
reset (protocol-equivalent to unplug/replug) does the same thing in software:

    python3 tools/usb_reset.py                  # reset THE ONLY S3, or fail if >1
    python3 tools/usb_reset.py --skip AA:BB     # reset the S3 that is NOT ...AA:BB
    python3 tools/usb_reset.py --serial CC:DD   # reset the S3 whose serial ~matches
    python3 tools/usb_reset.py --all            # reset every S3 on the bus

⚠ With TWO boards attached (same VID:PID), a bare reset used to hit the FIRST
match - the WRONG board - leaving the target wedged (a WEDGED board reports NO
serial string, so the target is often the one you must skip/pick BY the other's
MAC). Always disambiguate with --skip/--serial when >1 board is present.

Then immediately catch it into the bootloader if you need to flash:
    esptool --chip esp32s3 --port /dev/cu.usbmodem101 \
        --before default-reset --after no-reset chip-id

Needs pyusb + Homebrew libusb (brew install libusb; pip install pyusb).
"""

import argparse
import sys
import time

import usb.backend.libusb1
import usb.core
import usb.util

LIBUSB = "/opt/homebrew/lib/libusb-1.0.dylib"
VID, PID = 0x303A, 0x1001  # Espressif USB-serial-JTAG (the ROM / a silent-serial app)

# ⚠ An S3 does NOT always present 303a:1001. A board running an app with its own
# TinyUSB CDC enumerates under a different product id (e.g. 303a:4001, iface
# class 0x02 "Espressif CDC Device"), and a bare `find(idProduct=0x1001)` then
# reports "no device on the bus" - which reads like NOTHING is attached and has
# cost real hours of chasing the wrong problem. Match on the VENDOR and say what
# was actually found.
#
# ⚠ And know what a bus reset does: it resets the USB LINK (the documented
# unwedge for a silent CDC), NOT the CPU. It does not reboot the chip and cannot
# put it into download mode. A foreign app that implements no bootloader entry
# (no DTR/RTS or 1200-baud handler) still needs BOOT+RESET or the DevKit's other
# USB-C port.


def _serial(dev) -> str:
    try:
        return usb.util.get_string(dev, dev.iSerialNumber) or ""
    except Exception:  # noqa: BLE001 - a wedged board has no readable serial
        return ""


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--serial", help="reset the S3 whose serial contains this (case-insensitive)")
    ap.add_argument("--skip", help="reset the S3 whose serial does NOT contain this")
    ap.add_argument("--all", action="store_true", help="reset every S3 on the bus")
    ap.add_argument(
        "--any",
        action="store_true",
        help="also reset an S3 enumerating under a non-JTAG product id "
        "(an app's own USB CDC) - re-enumerates it, does NOT reboot it",
    )
    args = ap.parse_args()

    be = usb.backend.libusb1.get_backend(find_library=lambda x: LIBUSB)
    every = list(usb.core.find(find_all=True, idVendor=VID, backend=be))
    devs = [d for d in every if d.idProduct == PID]
    if not devs:
        if every:
            # Espressif silicon IS attached, just not under the JTAG product id.
            print(f"no device {VID:04x}:{PID:04x} on the bus, but found:")
            for d in every:
                print(f"  {d.idVendor:04x}:{d.idProduct:04x} serial={_serial(d)!r} bus={d.bus} addr={d.address}")
            print("  -> that is an APP's USB (not the ROM). A bus reset will re-enumerate")
            print("     it but will NOT reboot the chip; use --any to reset it anyway.")
            if args.any:
                devs = every
        if not devs:
            if not every:
                print(f"no device {VID:04x}:* on the bus")
            return 1

    if args.all:
        targets = devs
    elif args.serial:
        want = args.serial.upper().replace(":", "")
        targets = [d for d in devs if want in _serial(d).upper().replace(":", "")]
    elif args.skip:
        skip = args.skip.upper().replace(":", "")
        targets = [d for d in devs if skip not in _serial(d).upper().replace(":", "")]
    elif len(devs) == 1:
        targets = devs
    else:
        print(f"{len(devs)} S3 boards on the bus - disambiguate with --serial/--skip/--all:")
        for d in devs:
            print(f"  bus={d.bus} addr={d.address} serial={_serial(d)!r}")
        return 2

    if not targets:
        print("no S3 matched the filter")
        return 1
    rc = 0
    for d in targets:
        try:
            d.reset()
            print(f"bus reset OK - bus={d.bus} addr={d.address} serial={_serial(d)!r}")
        except Exception as exc:  # noqa: BLE001 - report and fail loud
            print(f"bus reset FAILED (bus={d.bus} addr={d.address}): {type(exc).__name__}: {exc}")
            rc = 1
    time.sleep(1.5)  # let the CDC node(s) re-enumerate
    return rc


if __name__ == "__main__":
    sys.exit(main())
