#!/usr/bin/env python3
"""The one forbidden-device guard: every tool that opens, resets, flashes or scans a USB port asks it first.

Two boards on this bench are never a target: the owner's other project and Lumi, the production device. A port
name is not an identity (the same usbmodem name has been a different board on different days), so the guard reads
the USB serial number, which equals the MAC on a native USB port. Reading it goes through the operating system's
USB listing and never opens the port, so asking the guard cannot reset or disturb any board.

What it decides:
  * a port or MAC naming a forbidden board: refused (ForbiddenDevice).
  * an Espressif native port with no serial string: refused (AnonymousPort). A wedged S3 reports no serial, so this
    could be a forbidden board. Once you have checked by eye that neither is plugged in, set NIMBUS_BENCH_ANON_OK=1.
  * a port the guard cannot identify because pyserial is missing: refused, not waved through.
  * a UART bridge (CP210x, CH34x): passed, its serial is the bridge's own. The MAC is only visible through esptool
    read-mac, so a flash path also calls check_mac on what esptool reports.

NIMBUS_BENCH_ALLOW_MAC=<one full MAC> lifts the refusal for exactly that board, for the owner working on it on
purpose. No bench script or test sets it; every use is printed to stderr.

Command line (nothing here opens a port):
  bench_guard.py list                   every USB serial node, its serial, and whether it is refused
  bench_guard.py check-port <port>      exit 0 if the port may be used, 3 if refused
  bench_guard.py check-mac <mac>        exit 0 if the MAC may be used, 3 if refused
  bench_guard.py port-for-mac <mac>     print the node whose USB serial is that MAC; exit 1 if absent, 3 if refused
"""

from __future__ import annotations

import importlib
import os
import re
import sys
from typing import List, NamedTuple, Optional

FORBIDDEN_MACS = {
    "2884854218FC": "the owner's other project",
    "14C19FD22E68": "Lumi, the production device",
}
ALLOW_ENV = "NIMBUS_BENCH_ALLOW_MAC"
ANON_ENV = "NIMBUS_BENCH_ANON_OK"
VID_ESPRESSIF = 0x303A
REFUSED_EXIT = 3


class ForbiddenDevice(RuntimeError):
    """This board is off limits, or its identity could not be read. Nothing was opened."""


class AnonymousPort(ForbiddenDevice):
    """An Espressif port with no serial string: a wedged board, possibly a forbidden one."""


class UsbPort(NamedTuple):
    device: str
    serial: str
    vid: Optional[int]
    pid: Optional[int]


def norm(text: Optional[str]) -> str:
    """A MAC or USB serial reduced to upper-case hex digits, so 28:84:... and 288485... compare equal."""
    return re.sub(r"[^0-9A-F]", "", (text or "").upper())


def _allowed_mac() -> str:
    return norm(os.environ.get(ALLOW_ENV))


def forbidden_label(serial: Optional[str]) -> Optional[str]:
    """Who this serial belongs to if it names a forbidden board, else None. A serial holding the MAC among other
    text still counts. The owner's override lifts one exact MAC and no other."""
    text = norm(serial)
    for mac, label in FORBIDDEN_MACS.items():
        if mac in text:
            if mac == _allowed_mac():
                print(f"bench_guard: {ALLOW_ENV} lifted the refusal for {label}", file=sys.stderr)
                return None
            return label
    return None


def _refusal(what: str, label: str, tool: str) -> str:
    return f"{tool}: refused, {what} is {label}. Nothing was opened or reset. See tools/bench_guard.py."


def check_mac(mac: Optional[str], tool: str = "bench tool") -> None:
    """Raise ForbiddenDevice if this MAC or serial names a forbidden board."""
    label = forbidden_label(mac)
    if label:
        raise ForbiddenDevice(_refusal(f"MAC {mac}", label, tool))


def comports():
    """pyserial's USB listing (a seam the host tests replace). Listing never opens a port."""
    return importlib.import_module("serial.tools.list_ports").comports()


def usb_ports() -> List[UsbPort]:
    """Every serial node with what the USB descriptor says about it. Raises ImportError without pyserial."""
    rows = []
    for info in comports():
        rows.append(
            UsbPort(
                device=info.device,
                serial=getattr(info, "serial_number", None) or "",
                vid=getattr(info, "vid", None),
                pid=getattr(info, "pid", None),
            )
        )
    return rows


def _canon(port: str) -> str:
    """Both macOS names of a node, and a symlink to one, compare equal."""
    return os.path.realpath(port).replace("/dev/tty.", "/dev/cu.")


def _find_port(port: str, rows: List[UsbPort]) -> Optional[UsbPort]:
    want = _canon(port)
    return next((row for row in rows if _canon(row.device) == want), None)


def _rows_or_refuse(port: str, tool: str) -> List[UsbPort]:
    try:
        return usb_ports()
    except Exception as exc:  # noqa: BLE001 - pyserial missing or a listing failure: identity unknown
        if os.path.exists(port):
            raise ForbiddenDevice(
                f"{tool}: refused, cannot read the USB serial of {port} ({type(exc).__name__}). "
                "Install pyserial so the board can be identified without opening it."
            ) from exc
        return []  # no such node: nothing to protect, and the open fails by itself


def check_port(port: Optional[str], tool: str = "bench tool") -> Optional[UsbPort]:
    """Raise ForbiddenDevice unless this node may be opened, reset or flashed. Returns its listing row, or None for
    a node the USB listing does not show (a pty, or a port that has gone)."""
    if not port:
        return None
    row = _find_port(port, _rows_or_refuse(port, tool))
    if row is None:
        check_mac(port, tool)  # a node name can carry the MAC itself
        return None
    label = forbidden_label(row.serial)
    if label:
        raise ForbiddenDevice(_refusal(f"{port} (serial {row.serial})", label, tool))
    if not row.serial and row.vid == VID_ESPRESSIF and os.environ.get(ANON_ENV) != "1":
        raise AnonymousPort(
            f"{tool}: refused, {port} reports no USB serial, so it could be a wedged forbidden board. "
            f"Check by eye that neither forbidden board is plugged in, then set {ANON_ENV}=1."
        )
    return row


def split_ports(rows: Optional[List[UsbPort]] = None):
    """(usable, refused) over the USB listing, for a tool that scans: refused rows carry who they belong to."""
    usable, refused = [], []
    for row in usb_ports() if rows is None else rows:
        label = forbidden_label(row.serial)
        if label:
            refused.append((row, label))
        else:
            usable.append(row)
    return usable, refused


def forbidden_on_bus(rows: Optional[List[UsbPort]] = None) -> List[str]:
    """Node names of forbidden boards currently on the bus. A failed listing reads as none here: callers that act
    on a named port use check_port, which refuses instead."""
    try:
        return [row.device for row, _ in split_ports(rows)[1]]
    except Exception:  # noqa: BLE001
        return []


def port_for_mac(mac: str, tool: str = "port-for-mac") -> Optional[str]:
    """The node whose USB serial is exactly this MAC, or None. Refuses a forbidden MAC before looking."""
    check_mac(mac, tool)
    want = norm(mac)
    return next((row.device for row in usb_ports() if norm(row.serial) == want), None)


def _cmd_list(_arg: str) -> int:
    usable, refused = split_ports()
    for row in usable:
        anon = " NO SERIAL (refused unless checked by eye)" if not row.serial and row.vid == VID_ESPRESSIF else ""
        print(f"{row.device}\t{row.serial or '-'}{anon}")
    for row, label in refused:
        print(f"{row.device}\t{row.serial}\tREFUSED: {label}")
    return 0


def _cmd_check_port(arg: str) -> int:
    check_port(arg, "check-port")
    print(f"{arg}: allowed")
    return 0


def _cmd_check_mac(arg: str) -> int:
    check_mac(arg, "check-mac")
    print(f"{arg}: allowed")
    return 0


def _cmd_port_for_mac(arg: str) -> int:
    port = port_for_mac(arg)
    if port is None:
        print(f"no USB serial node reports {arg}", file=sys.stderr)
        return 1
    print(port)
    return 0


COMMANDS = {
    "list": _cmd_list,
    "check-port": _cmd_check_port,
    "check-mac": _cmd_check_mac,
    "port-for-mac": _cmd_port_for_mac,
}


def main(argv: Optional[List[str]] = None) -> int:
    args = list(sys.argv[1:] if argv is None else argv)
    if not args or args[0] not in COMMANDS or (args[0] != "list" and len(args) != 2):
        print(__doc__, file=sys.stderr)
        return 2
    try:
        return COMMANDS[args[0]](args[1] if len(args) > 1 else "")
    except ForbiddenDevice as exc:
        print(str(exc), file=sys.stderr)
        return REFUSED_EXIT
    except ImportError as exc:
        print(f"bench_guard: cannot read the USB listing ({exc}); install pyserial", file=sys.stderr)
        return REFUSED_EXIT


if __name__ == "__main__":
    raise SystemExit(main())
