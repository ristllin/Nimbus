#!/usr/bin/env python3
"""ring_check - light the LED ring for a few seconds to split hardware from firmware.

Runs the firmware's LEDTEST drill: the driver paints the whole ring SOLID
RED -> GREEN -> BLUE, one second each, at LOW brightness (40/255, ~16%), then
restores normal control. Nothing else on the device is touched.

    python3 tools/ring_check.py                      # default serial port
    python3 tools/ring_check.py --port /dev/cu.usbmodemXXX

How to read the result (your eyes are the instrument):
  * Ring shows red, then green, then blue  -> ring + data path + 5 V rail are
    all GOOD. Any earlier "LEDs don't work" was firmware state, now fixed.
  * Ring stays dark but the script prints "LEDTEST red/green/blue/done"
    -> the firmware drove the data line; the RING has no power or no signal.
       Check, in order: the 5 V bus (the ring is fed from the DC-DC BEHIND the
       battery - with no pack fitted, a charger on the BMS input may not power
       it at all), the ring's GND to system GND, and DIN really on GPIO 21
       (first pixel end of the strip, not the DOUT end).
  * Script errors out -> port/console problem, not a ring verdict. Re-run once;
    if it persists, check the port name and that nothing else holds it open.

⚠ Needs an [env:test]-family build (the LEDTEST console command).
⚠ Opening the serial port RESETS the board (normal on this hardware) - the
  script waits out the reboot before driving the test. Don't run it while a
  flash or another serial session is in flight.
"""

import argparse
import sys
import time

try:
    import serial
except ImportError:
    sys.exit("pyserial is required: pip install pyserial (or run with ~/.platformio/penv/bin/python3)")

DEFAULT_PORT = "/dev/cu.usbmodem101"  # default bench port - verify the board by MAC first


def main() -> int:
    ap = argparse.ArgumentParser(description="Flash the LED ring red/green/blue at low brightness.")
    ap.add_argument("--port", default=DEFAULT_PORT, help=f"serial port (default {DEFAULT_PORT})")
    args = ap.parse_args()

    # Quiet-open per AGENTS.md: NEVER assert DTR/RTS on this board.
    s = serial.Serial()
    s.port = args.port
    s.baudrate = 115200
    s.timeout = 0.25
    s.dtr = False
    s.rts = False
    try:
        s.open()
    except serial.SerialException as e:
        sys.exit(f"could not open {args.port}: {e}")

    def drain(seconds: float) -> str:
        end = time.time() + seconds
        buf = b""
        while time.time() < end:
            chunk = s.read(4096)
            if chunk:
                buf += chunk
        return buf.decode("utf-8", "replace")

    print("Port opened - the board reboots on open; waiting it out (~18 s)…")
    drain(18)

    print("Driving LEDTEST - WATCH THE RING: solid red, green, blue (1 s each, dim).")
    s.write(b"LEDTEST\n")
    s.flush()
    out = drain(9)
    s.close()

    echoed = [l for l in out.splitlines() if "LEDTEST" in l]
    for l in echoed:
        print(" ", l.strip())

    if any("LEDTEST done" in l for l in echoed):
        print("\nFirmware drove the full sweep and resumed normal control.")
        print("If the ring LIT red/green/blue: everything is healthy.")
        print("If it stayed DARK: the ring has no power/signal - check the 5 V")
        print("bus (needs the battery/DC-DC path), ring GND, and DIN on GPIO 21.")
        return 0
    print("\nNo LEDTEST acknowledgement - console did not respond (wrong port,")
    print("non-test firmware, or another process holds the port). Not a ring verdict.")
    return 1


if __name__ == "__main__":
    sys.exit(main())
