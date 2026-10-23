"""Read the durable log tail over the LAN and print the lines the lane grades on
(card failures, demotions, crash boots, the AP hold). Bench helper; no secrets."""
import os
import sys

import requests

ip = open("/Users/roydarnell/Projects/nimbus-project/secrets/devices/unitA_ip.txt").read().strip()
r = requests.get(f"http://{ip}/api/errlog/recent?bytes=24576", timeout=15)
text = r.text if r.status_code == 200 else ""
lines = text.splitlines()
needles = ("not enough mem", "SD lost", "crash(panic)", "SoftAP restore waits", "held during the card outage")
hits = [ln for ln in lines if any(n in ln for n in needles)]
print(f"== current: {len(lines)} lines, {len(hits)} matching")
for ln in hits:
    print(ln)
