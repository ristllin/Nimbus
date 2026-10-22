#!/usr/bin/env python3
"""Provision one bench board for the HIL suite: Wi-Fi over its test console, Orchestrator
mode, the web sign-in token read back; then over the LAN: sound off and the provider
keys pushed by name.

Wi-Fi comes from the same names the HIL suite reads (``NIMBUS_TEST_STA_SSID`` /
``NIMBUS_TEST_STA_PASS``, from the environment or the bench ``.env``); the older bench
names ``TESTING_WIFI_SSID`` / ``TESTING_WIFI_PASS`` are read only when those are absent,
so a board provisioned here joins the network the legs expect.

Usage (never prints a secret)::

    prov_board.py --label unitL --port /dev/cu.usbmodem2101 --mac 28:84:85:42:18:FC \\
        --out-dir ../secrets/devices [--env-file .env] [--keys anthropic]

Writes ``<out-dir>/<label>_webtok.txt`` and ``<out-dir>/<label>_ip.txt``.
"""

from __future__ import annotations

import argparse
import json
import os
import re
import sys
import time
import urllib.error
import urllib.parse
import urllib.request
from typing import Dict, List, Mapping, Optional, Tuple

HERE = os.path.dirname(os.path.abspath(__file__))
DEFAULT_ENV = os.path.join(os.path.dirname(os.path.dirname(HERE)), ".env")
WIFI_NAMES = (("NIMBUS_TEST_STA_SSID", "NIMBUS_TEST_STA_PASS"), ("TESTING_WIFI_SSID", "TESTING_WIFI_PASS"))
KEY_FIELDS = {
    "anthropic": ("antKey", "ANTHROPIC_API_KEY"),
    "openai": ("oaiKey", "OPENAI_API_KEY"),
    "mistral": ("mistKey", "MISTRAL_API_KEY"),
    "zai": ("zaiKey", "Z_AI_TOKEN"),
}
TOKEN_RE = re.compile(rb"WEBTOK ([0-9a-fA-F]{24})")
STATUS_IP_RE = re.compile(rb"wifi=(\d) ip=([0-9.]+)")


def read_env(path: str) -> Dict[str, str]:
    out: Dict[str, str] = {}
    try:
        with open(path, encoding="utf-8") as f:
            for line in f:
                if "=" in line and not line.lstrip().startswith("#"):
                    k, _, v = line.strip().partition("=")
                    out[k.strip()] = v.strip().strip('"').strip("'")
    except OSError:
        pass
    return out


def wifi_credentials(environ: Mapping[str, str], dotenv: Mapping[str, str]) -> Optional[Tuple[str, str, str]]:
    """(ssid, password, the variable name used): the HIL names first, from the
    environment, then the .env; the older bench names only when neither has them."""
    for ssid_name, pass_name in WIFI_NAMES:
        for source in (environ, dotenv):
            if source.get(ssid_name) and source.get(pass_name):
                return source[ssid_name], source[pass_name], ssid_name
    return None


def port_matches(port: str, mac: str) -> bool:
    """The USB serial number of ``port`` is the board's MAC (ESP32-S3 USB-JTAG)."""
    from serial.tools import list_ports

    return any(p.device == port and (p.serial_number or "").lower() == mac.lower() for p in list_ports.comports())


class Console:
    def __init__(self, port: str):
        import serial

        self.s = serial.Serial()
        self.s.port, self.s.baudrate, self.s.timeout = port, 115200, 0.3
        self.s.dtr = self.s.rts = False
        self.s.open()

    def read_for(self, secs: float, pat: Optional[bytes] = None) -> bytes:
        end, buf = time.time() + secs, b""
        while time.time() < end:
            buf += self.s.read(4096)
            if pat and re.search(pat, buf):
                break
        return buf

    def cmd(self, line: str, secs: float, pat: Optional[bytes] = None) -> bytes:
        self.s.write((line + "\n").encode())
        self.s.flush()
        return self.read_for(secs, pat)

    def close(self) -> None:
        self.s.close()


def provision_console(port: str, ssid: str, password: str) -> Tuple[Optional[str], Optional[str]]:
    """Wi-Fi and Orchestrator mode over the console; returns (ip, token)."""
    con = Console(port)
    try:
        con.read_for(25)  # the boot opening the port caused
        con.cmd(f"WIFI {ssid}|{password}", 20, rb"WIFI provisioning|GOT_IP|wifi=1")
        ip, st = None, b""
        for _ in range(25):
            st = con.cmd("STATUS", 6, rb"STATUS .*?uptime")
            m = STATUS_IP_RE.search(st)
            if m and m.group(1) == b"1" and m.group(2) != b"0.0.0.0":
                ip = m.group(2).decode()
                break
            time.sleep(3)
        if b"mode=1" not in st:
            con.cmd("MODE 1", 35)
        token = None
        for _ in range(5):
            m = TOKEN_RE.search(con.cmd("WEBTOK?", 8, rb"WEBTOK [0-9a-fA-F]{24}"))
            if m:
                token = m.group(1).decode()
                break
        return ip, token
    finally:
        con.close()


def push_settings(ip: str, token: str, env: Mapping[str, str], providers: List[str]) -> Dict[str, object]:
    opener = urllib.request.build_opener(urllib.request.ProxyHandler({}))

    def post(data: dict) -> object:
        req = urllib.request.Request(
            f"http://{ip}/api/orch", data=urllib.parse.urlencode(data).encode(), headers={"X-Nimbus-Token": token}
        )
        try:
            with opener.open(req, timeout=30) as r:
                return r.status
        except urllib.error.HTTPError as e:
            return e.code
        except Exception as e:  # noqa: BLE001 - reported, never raised
            return type(e).__name__

    out: Dict[str, object] = {"sound": post({"sfxLvlO": "0", "sfxLvlN": "0", "sfxVol": "0", "ttsOn": "0"})}
    for p in providers:
        field, name = KEY_FIELDS[p]
        out[p] = post({field: env[name]}) if env.get(name) else f"no {name} in the env file"
    return out


def main(argv: Optional[List[str]] = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--label", required=True)
    ap.add_argument("--port", required=True)
    ap.add_argument("--mac", help="refuse unless the port's USB serial number is this MAC")
    ap.add_argument("--out-dir", required=True, help="where <label>_webtok.txt and <label>_ip.txt go")
    ap.add_argument("--env-file", default=DEFAULT_ENV)
    ap.add_argument("--keys", default="anthropic", help="comma-separated providers to push (default anthropic)")
    args = ap.parse_args(argv)
    if args.mac and not port_matches(args.port, args.mac):
        print(f"refusing: {args.port} is not the board {args.mac}")
        return 2
    env = read_env(args.env_file)
    wifi = wifi_credentials(os.environ, env)
    if not wifi:
        print("no Wi-Fi: set NIMBUS_TEST_STA_SSID and NIMBUS_TEST_STA_PASS (environment or env file)")
        return 2
    ssid, password, used = wifi
    print(f"{args.label}: Wi-Fi from {used}")
    ip, token = provision_console(args.port, ssid, password)
    print(f"{args.label}: ip {ip or 'none'}, token {'ok' if token else 'none'}")
    if not (ip and token):
        return 1
    os.makedirs(args.out_dir, exist_ok=True)
    for suffix, value in (("webtok", token), ("ip", ip)):
        with open(os.path.join(args.out_dir, f"{args.label}_{suffix}.txt"), "w", encoding="utf-8") as f:
            f.write(value)
    providers = [p.strip() for p in args.keys.split(",") if p.strip()]
    unknown = [p for p in providers if p not in KEY_FIELDS]
    if unknown:
        print(f"unknown providers {unknown}")
        return 2
    print(f"{args.label}: {json.dumps(push_settings(ip, token, env, providers))}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
