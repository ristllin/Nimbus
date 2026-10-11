#!/usr/bin/env python3
# /// script
# dependencies = ["pyusb==1.3.1"]
# ///
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

Runs on macOS and Linux. Needs pyusb in the Python that runs it and the libusb 1.0
library (macOS: brew install libusb; Debian/Ubuntu: sudo apt install libusb-1.0-0;
Fedora: sudo dnf install libusb1). The inline script metadata above lets
`pipx run tools/usb_reset.py` (pipx 1.4.2 or newer) or `uv run tools/usb_reset.py`
bring pyusb along. A missing prerequisite or another platform exits 2 with the fix,
never a traceback. On Linux a reset needs permission on the USB device (root, or a
udev rule for vendor 303a).

Exit codes: 0 reset done; 1 no matching board, or a reset failed; 2 cannot run here
(unsupported platform, pyusb or libusb missing) or several boards need --serial/--skip.
"""

import argparse
import ctypes.util
import os
import sys
import time

# pyusb (usb.*) is imported lazily (load_usb_backend), so the pure select_targets()
# below - and the host tests that cover its MAC-safety and the setup error paths - can
# import this module on a box without pyusb / libusb installed.

VID, PID = 0x303A, 0x1001  # Espressif USB-serial-JTAG (the ROM / a silent-serial app)

# Where Homebrew puts libusb on macOS: Apple Silicon first (that prefix is not on the
# default library search path, and a leftover Intel copy in /usr/local would not load
# in an arm64 Python), then Intel.
HOMEBREW_LIBUSB = ("/opt/homebrew/lib/libusb-1.0.dylib", "/usr/local/lib/libusb-1.0.dylib")
LIBUSB_INSTALL = {
    "darwin": "brew install libusb",
    "linux": "sudo apt install libusb-1.0-0 (Debian/Ubuntu) or sudo dnf install libusb1 (Fedora)",
}

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


class SetupError(Exception):
    """A prerequisite is missing or the platform is unsupported; the message says how to fix it."""


def current_platform() -> str:
    """sys.platform ('darwin', 'linux', ...), behind a seam the host tests patch."""
    return sys.platform


def find_libusb(platform: str, find_library=ctypes.util.find_library, exists=os.path.isfile) -> "str | None":
    """Locate the libusb 1.0 shared library, or None.

    macOS: the Homebrew prefixes, then the system search, and only an absolute
    path from it (find_library returns a bare name for a file in the current
    directory, and loading that would run whatever a checkout or download folder
    planted there). Linux: the system search, which yields a soname that the loader
    resolves from the system library paths only."""
    if platform != "darwin":
        return find_library("usb-1.0")
    found = find_library("usb-1.0")
    candidates = (*HOMEBREW_LIBUSB, found if found and os.path.isabs(found) else None)
    return next((path for path in candidates if path and exists(path)), None)


def pyusb_missing_message(platform: str) -> str:
    python = sys.executable or "python3"
    script = os.path.abspath(__file__)
    return (
        f"pyusb is not installed for this Python ({python}). Install it, then run this again:\n"
        f"  {python} -m pip install pyusb\n"
        "If pip refuses (externally-managed-environment), let pipx 1.4.2+ or uv bring pyusb along:\n"
        f"  pipx run {script}\n"
        f"It also needs the libusb library: {LIBUSB_INSTALL[platform]}."
    )


def _reset_failed_hint(exc: Exception, platform: str) -> str:
    """The next step for a failed reset: on Linux an access error means permissions."""
    if platform == "linux" and getattr(exc, "errno", None) == 13:
        return "  -> no permission on the USB device. Run it with sudo, or add a udev rule for vendor 303a."
    return ""


def load_usb_backend(platform: str):
    """Import pyusb lazily and open the libusb backend: (usb.core, backend).

    Raises SetupError, with the install command, when the platform is unsupported,
    pyusb is missing, or libusb cannot be found or loaded."""
    if platform not in LIBUSB_INSTALL:
        raise SetupError(
            f"usb_reset.py runs on macOS and Linux only (this system is {platform!r}). "
            "Unplug the board and plug it back in instead."
        )
    try:
        import usb.backend.libusb1
        import usb.core
    except ImportError as exc:
        raise SetupError(pyusb_missing_message(platform)) from exc
    library = find_libusb(platform)
    if library is None:
        raise SetupError(
            f"The libusb 1.0 library was not found. Install it, then run this again: {LIBUSB_INSTALL[platform]}."
        )
    backend = usb.backend.libusb1.get_backend(find_library=lambda _name: library)
    if backend is None:
        raise SetupError(
            f"libusb was found ({library}) but could not be loaded. Reinstall it: {LIBUSB_INSTALL[platform]}."
        )
    return usb.core, backend


def _serial(dev) -> str:
    import usb.util

    try:
        return usb.util.get_string(dev, dev.iSerialNumber) or ""
    except Exception:  # noqa: BLE001 - a wedged board has no readable serial
        return ""


def _norm(s: str) -> str:
    return (s or "").upper().replace(":", "")


def select_targets(serials, *, all_=False, serial=None, skip=None):
    """MAC-safe target selection (pure, host-tested). ``serials`` is the serial
    string of each attached S3, in order (a WEDGED board reports '' - no readable
    serial). Returns ``(indices, error)``: ``indices`` are the positions to reset;
    ``error`` is a message when selection refuses.

    The safety rule (two boards attached, same VID:PID): NEVER touch the board that
    is not the target. A bare reset with >1 board refuses rather than hitting the
    first match (which used to be the WRONG, healthy board). Because a wedged board
    has no serial, the target is usually chosen by SKIPPING the healthy board's MAC:
    ``skip`` returns every board whose serial does not contain it, so the healthy
    board is never in the result; ``serial`` returns only boards whose serial
    matches it."""
    idxs = list(range(len(serials)))
    if not serials:
        return [], "no S3 on the bus"
    if all_:
        return idxs, None
    if serial is not None:
        want = _norm(serial)
        sel = [i for i in idxs if want and want in _norm(serials[i])]
        return (sel, None) if sel else ([], f"no S3 serial contains {serial!r}")
    if skip is not None:
        s = _norm(skip)
        # An empty --skip would match nothing to skip (and reset everything), which
        # is the opposite of safe - refuse it.
        if not s:
            return [], "empty --skip would not protect any board"
        sel = [i for i in idxs if s not in _norm(serials[i])]
        return (sel, None) if sel else ([], f"every S3 serial contains {skip!r} (nothing to reset)")
    if len(serials) == 1:
        return [0], None
    return [], (f"{len(serials)} S3 boards on the bus - disambiguate with --serial/--skip/--all")


def main(argv: "list[str] | None" = None) -> int:
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
    args = ap.parse_args(argv)

    try:
        usb_core, be = load_usb_backend(current_platform())
    except SetupError as exc:
        print(f"Stopped: {exc}", file=sys.stderr)
        return 2
    every = list(usb_core.find(find_all=True, idVendor=VID, backend=be))
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

    serials = [_serial(d) for d in devs]
    sel, err = select_targets(serials, all_=args.all, serial=args.serial, skip=args.skip)
    if err:
        print(err)
        if len(devs) > 1 and not (args.serial or args.skip or args.all):
            for d in devs:
                print(f"  bus={d.bus} addr={d.address} serial={_serial(d)!r}")
            return 2
        return 1
    targets = [devs[i] for i in sel]
    rc = 0
    for d in targets:
        try:
            d.reset()
            print(f"bus reset OK - bus={d.bus} addr={d.address} serial={_serial(d)!r}")
        except Exception as exc:  # noqa: BLE001 - report and fail loud
            print(f"bus reset FAILED (bus={d.bus} addr={d.address}): {type(exc).__name__}: {exc}")
            hint = _reset_failed_hint(exc, current_platform())
            if hint:
                print(hint)
            rc = 1
    time.sleep(1.5)  # let the CDC node(s) re-enumerate
    return rc


if __name__ == "__main__":
    sys.exit(main())
