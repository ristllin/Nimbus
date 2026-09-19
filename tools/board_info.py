#!/usr/bin/env python3
"""board_info - non-destructively read a Nimbus board's state over USB serial.

Development often needs "what is on this board and what state is it in" without
flashing, erasing, or even rebooting it (a running field device, or a board you
must not disturb). This tool answers that:

  * DEFAULT is dead-safe. It opens the serial port with DTR and RTS deasserted,
    so the ESP32 auto-reset line is never pulsed - the board is not reset, not
    rebooted, and nothing is written to it. It only LISTENS, then summarizes.
  * It collapses the boot-log flood (e.g. a repeating "File system is not
    mounted") into one line with a count, and parses the known boot markers into
    a compact summary: firmware, mode, board, SD + filesystem state, device
    name, storage tier, memory/PSRAM, faults, and the provider fabric.
  * On a NIMBUS_TEST build (the `test` env) the firmware has a serial STATUS
    console; `--status` sends "STATUS" and parses the authoritative one-line
    reply. On a production build serial input is consumed by the USB-update
    frame reader, which discards non-frame bytes, so `--status` is a harmless
    no-op there (nothing is committed - the updater needs a full framed image).
  * `--reboot` is the ONE opt-in that disturbs the board: it pulses reset to
    capture a full fresh boot log. It is a reboot, never a flash or erase. Do
    NOT use it on a device you must not restart.

Usage:
  tools/board_info.py                 # summarize every /dev/cu.usbmodem* board
  tools/board_info.py --port /dev/cu.usbmodem101 --seconds 8
  tools/board_info.py --status        # also query STATUS (test builds)
  tools/board_info.py --reboot        # reset + capture a full boot log
  tools/board_info.py --json          # machine-readable summary (HIL/scripts)
  tools/board_info.py --raw           # also print the deduped capture

Needs pyserial (`pip install pyserial`); it is already in the HIL test deps.
"""

from __future__ import annotations

import argparse
import glob
import json
import re
import sys
import time

try:
    import serial  # pyserial
except ImportError:
    sys.stderr.write("board_info: pyserial not installed (pip install pyserial)\n")
    sys.exit(2)


def capture(port: str, seconds: float, reboot: bool, send_status: bool) -> str:
    """Read raw serial text. DTR/RTS stay deasserted unless `reboot` is set, so by
    default the board is never reset. Returns the decoded text (best-effort)."""
    s = serial.Serial()
    s.port = port
    s.baudrate = 115200
    s.timeout = 0.4
    # The safety boundary: do NOT assert DTR/RTS -> the auto-reset circuit is not
    # driven, so opening the port does not reboot the board.
    s.dtr = reboot
    s.rts = reboot
    s.open()
    try:
        if reboot:
            # Pulse the reset line: EN low via DTR, then release, to force a clean
            # boot log. This REBOOTS the board (never flashes/erases it).
            s.dtr = True
            s.rts = True
            time.sleep(0.05)
            s.dtr = False
            s.rts = False
        if send_status:
            try:
                s.write(b"STATUS\n")
            except Exception:
                pass
        end = time.time() + seconds
        chunks = []
        while time.time() < end:
            b = s.read(2048)
            if b:
                chunks.append(b)
        return b"".join(chunks).decode("utf-8", "replace")
    finally:
        s.close()


def dedup(text: str) -> list[str]:
    """Collapse consecutive identical lines (ignoring the [ 1234] ms timestamp
    prefix) into 'line  (xN)', so a repeating fault does not bury the signal."""
    out: list[tuple[str, str, int]] = []  # (key, display, count)
    ts = re.compile(r"^\[\s*\d+\]")
    for raw in text.splitlines():
        line = raw.rstrip()
        if not line:
            continue
        key = ts.sub("", line).strip()
        if out and out[-1][0] == key:
            out[-1] = (key, out[-1][1], out[-1][2] + 1)
        else:
            out.append((key, line, 1))
    return [d if c == 1 else f"{d}  (x{c})" for _, d, c in out]


# Boot-log markers -> summary fields. Each is best-effort; a missing field is
# reported as such rather than guessed.
PATTERNS = {
    "firmware": re.compile(r"\bfw=([^\s]+)"),
    "build": re.compile(r"\bbuild=([^\s]+)"),
    "mode": re.compile(r"mode=(\w+)"),
    "board": re.compile(r"board=([^\s]+)"),
    "device_name": re.compile(r"name=([^\s]+)"),
    "storage_tier": re.compile(r"storage tier=([^\n]+)"),
    "psram_free": re.compile(r"psramFree=(\d+)"),
    "int_free": re.compile(r"intFree=(\d+)"),
    "fabric": re.compile(r"fabric:\s*\w+\s*->\s*(\w+)"),
}


def summarize(text: str) -> dict:
    info: dict = {}
    # A STATUS reply (test build) is authoritative for most fields.
    st = re.search(r"^STATUS .*$", text, re.MULTILINE)
    scope = st.group(0) if st else text
    for field, pat in PATTERNS.items():
        m = pat.search(scope) or pat.search(text)
        if m:
            info[field] = m.group(1)
    # SD: present (capacity line or sd=present) vs failed/absent.
    if re.search(r"\bsd=present\b", text) or re.search(r"SD:\s*SD(HC|XC|SC)?,", text):
        cap = re.search(r"SD:\s*\w+,\s*([\d]+ MB total[^\n]*)", text)
        info["sd"] = f"present ({cap.group(1).strip()})" if cap else "present"
    elif re.search(r"\bsd=absent\b|SD:\s*begin\(\) failed|Card Failed|no SD", text):
        info["sd"] = "absent / failed to mount"
    # Internal flash filesystem mount health (distinct from the SD card).
    fs_fail = len(re.findall(r"File system is not mounted", text))
    info["flash_fs"] = f"NOT mounted ({fs_fail} errors)" if fs_fail else "mounted / ok"
    if re.search(r"nimbus_name NOT_FOUND", text) and "device_name" not in info:
        info["device_name"] = "(unset - NVS default)"
    faults = re.search(r"faults=0x([0-9a-fA-F]+)", text)
    if faults:
        info["faults"] = f"0x{faults.group(1)}"
    info["is_test_build"] = bool(st)
    return info


def report(port: str, args) -> dict:
    text = capture(port, args.seconds, args.reboot, args.status)
    info = summarize(text)
    lines = dedup(text)
    result = {"port": port, "summary": info, "lines_captured": len(lines)}
    if not args.json:
        print(f"\n=== {port} ===")
        if not text.strip():
            print(
                "  (silent - no serial output; the board may be past boot and idle. "
                "Try --reboot for a fresh boot log, or --status on a test build.)"
            )
        order = [
            "firmware",
            "build",
            "board",
            "mode",
            "device_name",
            "sd",
            "flash_fs",
            "storage_tier",
            "int_free",
            "psram_free",
            "faults",
            "fabric",
            "is_test_build",
        ]
        for k in order:
            if k in info:
                print(f"  {k:14} {info[k]}")
        if args.raw:
            print("  --- deduped capture (tail) ---")
            for ln in lines[-40:]:
                print("  " + ln[:200])
    return result


def main() -> int:
    ap = argparse.ArgumentParser(description="Non-destructively read a Nimbus board over USB.")
    ap.add_argument("--port", help="serial port (default: every /dev/cu.usbmodem*)")
    ap.add_argument("--seconds", type=float, default=6.0, help="listen window (default 6)")
    ap.add_argument(
        "--status", action="store_true", help="also send STATUS (test builds; harmless no-op on production)"
    )
    ap.add_argument(
        "--reboot",
        action="store_true",
        help="pulse reset to capture a full boot log (REBOOTS the board; never flashes)",
    )
    ap.add_argument("--raw", action="store_true", help="print the deduped capture tail")
    ap.add_argument("--json", action="store_true", help="emit the parsed summary as JSON")
    args = ap.parse_args()

    ports = [args.port] if args.port else sorted(glob.glob("/dev/cu.usbmodem*"))
    if not ports:
        sys.stderr.write("board_info: no /dev/cu.usbmodem* ports found\n")
        return 1
    results = []
    for p in ports:
        try:
            results.append(report(p, args))
        except Exception as e:  # a busy or vanished port should not abort the rest
            msg = f"{type(e).__name__}: {e}"
            results.append({"port": p, "error": msg})
            if not args.json:
                print(f"\n=== {p} ===\n  ERROR {msg}")
    if args.json:
        print(json.dumps(results, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
