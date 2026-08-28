#!/usr/bin/env python3
"""Stream a local file to an absolute SD path on a Nimbus TEST board via the
test-console FSPUT verb (base64 over serial, ACK-paced), then verify size+sha256
with FSTAT reading the bytes back off the card.

This is the host side of the smallest honest write path to SD /music (the file
API only writes /mem/files; the music player only reads /music). Test build only.

  python3 tools/fsput_stream.py --port /dev/cu.usbmodem101 \
      --src ".dev_assets/song.mp3" --dst "/music/long.mp3"
"""

from __future__ import annotations
import argparse
import base64
import hashlib
import sys
import time
import serial


_NOISE = ("[E][", "[W][", "[I][", "Preferences", "nvs_get_str")


def _is_noise(line: str) -> bool:
    return any(n in line for n in _NOISE)


def _readline(ser, deadline):
    ser.timeout = max(0.1, deadline - time.time())
    return ser.readline().decode("utf-8", "replace").strip()


def _readsig(ser, deadline):
    """Read the next non-noise (signal) line, or '' at the deadline."""
    while time.time() < deadline:
        line = _readline(ser, deadline)
        if line and not _is_noise(line):
            return line
    return ""


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", default="/dev/cu.usbmodem101")
    ap.add_argument("--src", required=True)
    ap.add_argument("--dst", required=True, help="absolute SD path, e.g. /music/long.mp3")
    ap.add_argument("--baud", type=int, default=115200)
    ap.add_argument("--window", type=int, default=64 * 1024, help="max decoded bytes in flight")
    a = ap.parse_args()

    data = open(a.src, "rb").read()
    sha = hashlib.sha256(data).hexdigest()
    b64 = base64.b64encode(data)
    print(f"src={a.src} bytes={len(data)} sha256={sha}")
    print(f"dst={a.dst} base64_len={len(b64)}")

    ser = serial.Serial()
    ser.port = a.port
    ser.baudrate = a.baud
    ser.dtr = False  # do not toggle reset on the ESP32-S3 native USB
    ser.rts = False
    ser.timeout = 2
    ser.open()
    time.sleep(0.5)
    ser.reset_input_buffer()

    # Opening the ESP32-S3 native-USB port reboots the board; wait for boot +
    # SD mount ("READY mode=..") before FSPUT so SD.open does not race the mount.
    boot_dl = time.time() + 20
    while time.time() < boot_dl:
        line = _readsig(ser, boot_dl)
        if line.startswith("READY mode="):
            print("<", line, "(boot complete)")
            break
    time.sleep(1.5)  # a touch more for the card mount to settle

    ready = False
    for attempt in range(3):
        ser.reset_input_buffer()
        ser.write(f"FSPUT {len(data)} {a.dst}\n".encode())
        ser.flush()
        dl = time.time() + 8
        while time.time() < dl:
            line = _readsig(ser, dl)
            if not line:
                continue
            print("<", line)
            if line.startswith("FSPUT READY"):
                ready = True
                break
            if line.startswith("FSPUT ERR"):
                print("ERROR:", line)
                return 2
        if ready:
            break
        print(f"(no READY, retry {attempt + 1})")
    if not ready:
        print("ERROR: no FSPUT READY")
        return 2

    acked = 0
    sent_dec = 0
    i = 0
    CH = 4096
    t0 = time.time()
    while i < len(b64):
        chunk = b64[i : i + CH]
        ser.write(chunk)
        i += len(chunk)
        sent_dec = i * 3 // 4
        # opportunistically drain any ACKs
        while ser.in_waiting:
            line = ser.readline().decode("utf-8", "replace").strip()
            if line.startswith("FSACK"):
                acked = int(line.split()[1])
        # window: block for ACKs if we are too far ahead
        guard = time.time() + 30
        while sent_dec - acked > a.window and time.time() < guard:
            line = _readline(ser, guard)
            if line.startswith("FSACK"):
                acked = int(line.split()[1])
    ser.flush()

    done = None
    dl = time.time() + 90
    while time.time() < dl:
        line = _readsig(ser, dl)
        if not line or line.startswith("FSACK"):
            continue
        print("<", line)
        if line.startswith("FSPUT DONE"):
            done = line
            break
    if not done:
        print("ERROR: no FSPUT DONE")
        return 3
    dt = time.time() - t0
    fields = dict(kv.split("=") for kv in done.split()[1:])
    ok_put = int(fields.get("bytes", -1)) == len(data) and fields.get("sha256") == sha
    print(f"FSPUT verify: {'OK' if ok_put else 'MISMATCH'} ({dt:.1f}s, {len(data) / max(dt, 0.01) / 1024:.1f} KB/s)")

    # independent re-read off the card
    ser.reset_input_buffer()
    ser.write(f"FSTAT {a.dst}\n".encode())
    ser.flush()
    dl = time.time() + 30
    stat = None
    while time.time() < dl:
        line = _readsig(ser, dl)
        if line.startswith("FSTAT "):
            stat = line
            print("<", line)
            break
    ser.close()
    if not stat:
        print("ERROR: no FSTAT reply")
        return 4
    sf = dict(kv.split("=") for kv in stat.split() if "=" in kv)
    ok_stat = int(sf.get("bytes", -1)) == len(data) and sf.get("sha256") == sha
    print(f"FSTAT verify (on-card): {'OK' if ok_stat else 'MISMATCH'}")
    return 0 if (ok_put and ok_stat) else 5


if __name__ == "__main__":
    sys.exit(main())
