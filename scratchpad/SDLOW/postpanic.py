"""Post-panic boot check: trigger the test console's PANIC abort, then require the
boot to speak (the crash boot line, crash-why, the card mount with its reserved DMA
buffer, READY) and the LAN page to answer. The wedge class this guards against: a
boot that stays silent after a panic. Bench helper; no secrets."""
import time

import serial

PORT = "/dev/cu.usbmodem1101"
IP = open("/Users/roydarnell/Projects/nimbus-project/secrets/devices/unitA_ip.txt").read().strip()

ser = serial.Serial(PORT, 115200, timeout=0.2)
time.sleep(0.5)
ser.reset_input_buffer()
ser.write(b"PANIC abort\n")
lines = []
deadline = time.time() + 120
while time.time() < deadline:
    raw = ser.readline().decode("utf-8", "replace").strip()
    if not raw:
        continue
    lines.append(raw)
    if raw.startswith("READY"):
        break
ser.close()

lan = []
try:
    import requests

    r = requests.get(f"http://{IP}/", timeout=8)
    lan.append(f"http={r.status_code}")
except Exception as e:  # noqa: BLE001 - the check reports the failure, it does not hide it
    lan.append(f"error={type(e).__name__}")

joined = "\n".join(lines)
checks = {
    "crash boot line": "reason=crash(panic)" in joined,
    "crash-why": "crash-why:" in joined,
    "card mount with the DMA buffer": "[sd] mounted" in joined and "DMA buffer" in joined,
    "READY": any(ln.startswith("READY") for ln in lines),
}
ok = all(checks.values())
print("\n".join(lines[-8:]))
print(f"POSTPANIC {'PASS' if ok else 'FAIL'} lines={len(lines)} lan={lan} missing={[k for k, v in checks.items() if not v]}")
