#!/usr/bin/env python3
"""Push a locally-built firmware image to a running Nimbus over the USB serial
cable (CUM-390). This is the host side of the third update path, beside signed
cloud OTA and a ROM esptool flash: the running production firmware receives the
.bin over the cable and installs it through its existing esp_ota A/B engine.

No auth. The physical cable is the trust boundary, matching esptool and the ROM
flash. A peer on Wi-Fi cannot reach the serial stream.

  python3 tools/push_firmware.py --port /dev/cu.usbmodemXXXX firmware.bin

Wire protocol (little-endian), matching src/sys/usb_updater.h:
  START : b"NIMBUSFW1" + size (u32) + sha256 (32 bytes)
  CHNK  : b"CHNK" + len (u16) + payload + crc32 (u32)   ->  device: NFWU ack N / resend N
  DONE  : b"DONE" + sha256 (32 bytes)                   ->  device: NFWU ok / err <reason>

The device replies are single lines prefixed "NFWU "; ordinary firmware log lines
on the same TX are ignored. Exit code is 0 only on a final "NFWU ok".
"""

from __future__ import annotations

import argparse
import hashlib
import struct
import sys
import time
import zlib

try:
    import serial  # pyserial
except ImportError:
    sys.exit("push_firmware: pyserial is required (pip install pyserial)")


MAGIC = b"NIMBUSFW1"
CHUNK_TAG = b"CHNK"
DONE_TAG = b"DONE"
REPLY_PREFIX = "NFWU "
CHUNK_SIZE = 4096  # must stay <= kMaxChunk in usb_updater.h
ACK_TIMEOUT_S = 15.0  # per-chunk / per-reply wait
MAX_RESENDS = 5  # bounded retries per chunk before giving up


def _wait_reply(ser, timeout_s):
    """Return the token list of the next 'NFWU ...' line, or None at the deadline.

    Ordinary firmware log lines are skipped so the handshake survives the shared
    TX stream.
    """
    deadline = time.time() + timeout_s
    while time.time() < deadline:
        ser.timeout = max(0.1, deadline - time.time())
        raw = ser.readline().decode("utf-8", "replace").strip()
        if not raw:
            continue
        idx = raw.find(REPLY_PREFIX)
        if idx >= 0:
            return raw[idx + len(REPLY_PREFIX) :].split()
    return None


def _open_port(port, baud):
    ser = serial.Serial()
    ser.port = port
    ser.baudrate = baud
    # Do NOT toggle reset on the ESP32-S3 native USB (that would reboot the board
    # mid-handshake); the CP2102 UART variants tolerate this too.
    ser.dtr = False
    ser.rts = False
    ser.timeout = 2
    ser.open()
    time.sleep(0.4)
    ser.reset_input_buffer()
    return ser


def _send_start(ser, size, sha):
    ser.write(MAGIC + struct.pack("<I", size) + sha)
    ser.flush()
    reply = _wait_reply(ser, ACK_TIMEOUT_S)
    if reply is None:
        sys.exit("push_firmware: no reply to START (is this a production build with the listener?)")
    if reply[0] != "ready":
        why = " ".join(reply[1:]) if len(reply) > 1 else reply[0]
        sys.exit(f"push_firmware: device refused the update: {reply[0]} {why}".strip())


def _send_chunk(ser, index, payload):
    frame = CHUNK_TAG + struct.pack("<H", len(payload)) + payload + struct.pack("<I", zlib.crc32(payload) & 0xFFFFFFFF)
    ser.write(frame)
    ser.flush()
    resends = 0
    # Wait for a verdict on THIS index. A stale reply for another index (or any
    # other NFWU line) is ignored WITHOUT re-writing, so a stale ack never causes a
    # chunk to be written twice; only an explicit "resend <index>" rewrites it.
    while True:
        reply = _wait_reply(ser, ACK_TIMEOUT_S)
        if reply is None:
            sys.exit(f"push_firmware: timed out waiting for chunk {index} ack")
        verb = reply[0]
        num = reply[1] if len(reply) > 1 else ""
        if verb == "ack" and num == str(index):
            return
        if verb == "resend" and num == str(index):
            if resends >= MAX_RESENDS:
                sys.exit(f"push_firmware: chunk {index} failed after {MAX_RESENDS} resends")
            resends += 1
            ser.write(frame)
            ser.flush()
            continue
        if verb == "err":
            sys.exit(f"push_firmware: device error on chunk {index}: {' '.join(reply[1:])}")
        # Stale ack/resend for another index, or an unrelated NFWU line: keep
        # waiting, do NOT rewrite the frame.


def _send_done(ser, sha):
    ser.write(DONE_TAG + sha)
    ser.flush()
    reply = _wait_reply(ser, ACK_TIMEOUT_S)
    if reply is None:
        sys.exit("push_firmware: no final verdict from the device")
    if reply[0] == "ok":
        print("push_firmware: device accepted the image. It verifies and restarts now.")
        return
    sys.exit(f"push_firmware: device rejected the image: {' '.join(reply)}")


def main():
    ap = argparse.ArgumentParser(description="Push a firmware .bin to a running Nimbus over USB serial.")
    ap.add_argument("image", help="path to the locally-built firmware .bin")
    ap.add_argument("--port", required=True, help="serial port, e.g. /dev/cu.usbmodemXXXX")
    ap.add_argument("--baud", type=int, default=115200, help="baud (ignored by native USB CDC; default 115200)")
    a = ap.parse_args()

    with open(a.image, "rb") as f:
        data = f.read()
    if not data:
        sys.exit("push_firmware: image is empty")
    sha = hashlib.sha256(data).digest()
    size = len(data)
    total = (size + CHUNK_SIZE - 1) // CHUNK_SIZE
    print(f"push_firmware: {a.image} is {size} bytes, sha256={sha.hex()[:16]}..., {total} chunks")

    ser = _open_port(a.port, a.baud)
    try:
        _send_start(ser, size, sha)
        print("push_firmware: device ready, streaming...")
        for index in range(total):
            payload = data[index * CHUNK_SIZE : (index + 1) * CHUNK_SIZE]
            _send_chunk(ser, index, payload)
            if index % 32 == 0 or index == total - 1:
                pct = int((index + 1) * 100 / total)
                print(f"  chunk {index + 1}/{total} ({pct}%)")
        _send_done(ser, sha)
    finally:
        ser.close()


if __name__ == "__main__":
    main()
