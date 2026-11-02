#!/usr/bin/env python3
"""The update rehearsal (bench row B01), as one command: a unit on v4.6.4, its settings store at Lumi's level, updates
over the air to the candidate, on the path Lumi will take.

    ota_rehearsal.py --ip IP --token-file F --port P --mac MAC --out DIR --url https://HOST:8443/manifest.json \
        --want-version v5.0.0 --want-build <git describe of the candidate>

It starts only from --start-version (default v4.6.4) on slot app0, fills the settings store to --target-free (default
3936 B; run again at 4704 B, Lumi's 1 Nov reading), turns TLS checking off for the local server, and holds ONE console
session for the install, because OTAURL lives in RAM and a console open can reset the board. After the flip it polls
over HTTP, grades, and only then reopens the console for the restores. The server and manifest are staged beforehand
as in the plan (--serve does it for you). Prints no token; evidence is state-*.json, soak.tsv, ota-result.json.
"""

from __future__ import annotations

import argparse
import http.server
import json
import os
import socket
import ssl
import subprocess
import sys
import threading
import time
from pathlib import Path
from typing import Any, Callable, Dict, List, Optional

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import bench_driver as bd
import port_guard

SETTINGS = (
    "orchHost",
    "provPrio",
    "tgAllow",
    "tgOwners",
    "compactKB",
    "fetchPol",
    "loopDeadline",
    "theme",
    "ttsProv",
    "sttProv",
    "capProbe",
    "spendCeil",
)
NVS_ENTRY_B = 32
MIN_LOW_WATER = 8000
REPO = Path(__file__).resolve().parents[2]


class Rehearsal:
    def __init__(self, args, api: bd.Api, ev: bd.Evidence, open_console: Callable[[], Any]):
        self.a, self.api, self.ev, self.open_console = args, api, ev, open_console
        self.console = None
        self.res: Dict[str, Any] = {"at": bd.utc_iso(), "url": args.url, "startVersion": args.start_version}
        self.checks: Dict[str, bool] = {}

    # -- reading the unit -------------------------------------------------------------------------
    def state(self, tries: int = 30) -> Dict[str, Any]:
        for _ in range(tries):
            value = self.api.state(6.0)
            if value:
                return value
            time.sleep(self.a.wait_s)
        return {}

    def snap(self, tag: str) -> Dict[str, Any]:
        state, orch = self.state(), self.api.get_json("/api/orch")
        providers = orch.get("providers") or {}
        settings = {k: orch.get(k) for k in SETTINGS if k in orch}
        settings["providers"] = {
            name: {"hasKey": (v or {}).get("hasKey"), "orchModel": (v or {}).get("orchModel")} for name, v in providers.items()
        }
        row = {
            "fw": state.get("fw"),
            "build": state.get("build"),
            "otaSlot": state.get("otaSlot"),
            "lastOta": state.get("lastOta"),
            "uptimeS": (state.get("uptimeMs") or 0) // 1000,
            "boot": state.get("boot"),
            "nvs": state.get("nvs"),
            "heap": state.get("heap"),
            "mem": state.get("mem"),
            "settings": settings,
        }
        self.ev.write(f"state-{tag}.json", row)
        self.ev.note(f"[{tag}] build={row['build']} slot={row['otaSlot']} lastOta={row['lastOta']!r} nvs={row['nvs']}")
        return row

    def model_stamp(self) -> Any:
        reply = self.api.get_json(f"/api/models?provider={self.a.list_provider}", 20.0)
        return ((reply.get("providers") or {}).get(self.a.list_provider) or {}).get("fetchedAt")

    # -- the phases: each returns a reason to stop, or None -------------------------------------------
    def start_state(self) -> Optional[str]:
        self.console = self.open_console()
        for _ in range(40):
            if self.console.cmd("PING", "PONG", 3.0):
                break
            time.sleep(self.a.wait_s)
        pre = self.snap("pre-fill")
        self.res["pre"] = pre
        if not str(pre["fw"]).startswith(self.a.start_version) or pre["otaSlot"] != "app0":
            return f"the unit is on {pre['fw']} {pre['otaSlot']}, the rehearsal starts from {self.a.start_version} app0"
        return None

    def fill(self) -> Optional[str]:
        free = int((self.res["pre"]["nvs"] or {}).get("free") or 0)
        entries = max(0, (free - self.a.target_free) // NVS_ENTRY_B)
        self.ev.note(f"NVSFILL {entries}: {self.console.cmd(f'NVSFILL {entries}', 'NVSFILL', 15.0)}")
        self.api.post_form("/api/orch", {"tlsVerify": "0"})
        self.res["preInstall"] = self.snap("pre-install")
        self.res["modelsBefore"] = self.model_stamp()
        return None

    def check_available(self) -> Optional[str]:
        self.ev.note(f"OTAURL: {self.console.cmd('OTAURL ' + self.a.url, 'OTAURL', 8.0)}")
        self.console.cmd("OTACHECK", "OTACHECK", 8.0)
        seen = ""
        for _ in range(30):
            seen = self.console.cmd("OTA?", "OTA state=", 6.0)
            if "state=available" in seen or "state=error" in seen:
                break
            time.sleep(self.a.wait_s)
        self.res["check"] = seen
        if "state=available" not in seen or f"latest={self.a.want_version}" not in seen:
            return f"the check did not find {self.a.want_version} available: {seen}"
        return None

    def install(self) -> Optional[str]:
        applied = self.console.cmd("OTAAPPLY force", "OTAAPPLY", 10.0)
        self.res["apply"] = applied
        if "started" not in applied:
            return f"OTAAPPLY said {applied!r}"
        self.ev.note("installing; the console stays closed to new opens until the restores")
        started, build = time.time(), ""
        while time.time() - started < self.a.install_timeout:
            build = str(self.api.state(6.0).get("build") or "")
            if build.startswith(self.a.want_build):
                break
            time.sleep(self.a.wait_s)
        self.res["downloadToBootS"] = round(time.time() - started, 1)
        self.res["postFlip"] = self.snap("post-flip-t0")
        if not build.startswith(self.a.want_build):
            return f"the unit runs {build!r} after the install, not {self.a.want_build!r}"
        return None

    def validated(self) -> Optional[str]:
        started, last = time.time(), ""
        while time.time() - started < self.a.validate_timeout:
            last = str(self.api.state(6.0).get("lastOta") or "")
            if last.startswith("ok "):
                break
            time.sleep(self.a.wait_s)
        self.res["lastOta"] = last
        self.res["okConfirmed"] = self.snap("ok-confirmed")
        return None

    def one_turn(self) -> Optional[str]:
        result = bd.chat_turn(self.api, "Reply with exactly one word: ok", 180.0, self.a.wait_s)
        self.res["turn"] = {"ok": result.ok, "reply": result.reply[:200], "seconds": round(result.seconds, 1), "error": result.error}
        return None

    def soak(self) -> Optional[str]:
        polls, failed, boots, started = 0, 0, set(), time.time()
        path = self.ev.path("soak.tsv")
        with open(path, "w", encoding="utf-8") as f:
            f.write("t\thttp\tms\tuptimeS\tbootN\tlowWater\theap\n")
            while time.time() - started < self.a.soak_s:
                reply = self.api.get("/api/state", 5.0)
                polls += 1
                body = reply.json() if reply.code == 200 else {}
                failed += reply.code != 200
                boot_n = (body.get("boot") or {}).get("n")
                if boot_n is not None:
                    boots.add(boot_n)
                f.write(
                    f"{bd.utc_iso()}\t{reply.code}\t{int(reply.seconds * 1000)}\t{(body.get('uptimeMs') or 0) // 1000}\t"
                    f"{boot_n}\t{(body.get('mem') or {}).get('lowWater')}\t{body.get('heap')}\n"
                )
                time.sleep(self.a.poll_s)
        self.res["soak"] = {"seconds": self.a.soak_s, "polls": polls, "failed": failed, "bootNs": sorted(boots)}
        self.res["soakEnd"] = self.snap("soak-end")
        self.res["modelsAfter"] = self.model_stamp()
        log = self.api.get("/api/errlog", 60.0)
        self.ev.write("errlog-after-soak.txt", log.text)
        self.res["modelsStoredLine"] = "models stored" in log.text
        return None

    # -- the grade --------------------------------------------------------------------------------
    def grade(self) -> None:
        r, a = self.res, self.a
        pre, post, end = r["preInstall"], r["postFlip"], r["soakEnd"]
        ok_snap = r["okConfirmed"]
        diffs = {k: {"before": v, "after": ok_snap["settings"].get(k)} for k, v in pre["settings"].items() if ok_snap["settings"].get(k) != v}
        r["settingsDiff"] = diffs
        soak = r["soak"]
        low = (end.get("mem") or {}).get("lowWater")
        self.checks = {
            "bootsOtherSlot": post["otaSlot"] != pre["otaSlot"],
            "runsCandidate": str(post["build"]).startswith(a.want_build),
            "validates": str(r["lastOta"]).startswith("ok "),
            "settingsPreserved": not diffs,
            "nvsUnchanged": (pre["nvs"] or {}).get("free") == (ok_snap["nvs"] or {}).get("free"),
            "turnAnswers": bool(r["turn"]["ok"] and "ok" in r["turn"]["reply"].lower()),
            "soakClean": soak["failed"] == 0 and len(soak["bootNs"]) == 1 and not ((end["boot"] or {}).get("crash") or {}).get("thisReset"),
            "modelListRestamped": (r["modelsAfter"] or 0) > (r["modelsBefore"] or 0) or r["modelsStoredLine"],
            "lowWaterHolds": isinstance(low, (int, float)) and low > MIN_LOW_WATER,
        }
        r["checks"] = self.checks
        r["grade"] = "PASS" if all(self.checks.values()) else "FAIL"

    def restore(self) -> None:
        """Back to where it started. If the flip closed the console this reopen can reset the board, which is why it
        waits for the end of the run."""
        try:
            if self.console is None or self.console.dead:
                if self.console:
                    self.console.close()
                self.console = self.open_console()
            self.ev.note(f"restore OTAURL: {self.console.cmd('OTAURL clear', 'OTAURL', 8.0)}")
            self.ev.note(f"restore NVSFILL: {self.console.cmd('NVSFILL 0', 'NVSFILL', 15.0)}")
        except port_guard.ForbiddenDevice:
            raise
        except Exception as exc:  # noqa: BLE001 - the result must still be written
            self.ev.note(f"restore error: {type(exc).__name__}: {exc}")
        self.api.post_form("/api/orch", {"tlsVerify": "1"})
        self.res["final"] = self.snap("final")
        if self.console:
            self.console.close()

    def run(self) -> Dict[str, Any]:
        phases = (self.start_state, self.fill, self.check_available, self.install, self.validated, self.one_turn, self.soak)
        try:
            for phase in phases:
                reason = phase()
                if reason:
                    # The wrong start state touched nothing, so it is BLOCKED; a failed update is a finding.
                    self.res["grade"] = "BLOCKED" if phase == self.start_state else "FAIL"
                    self.res["reason"] = reason
                    self.ev.note(f"{self.res['grade']}: {reason}")
                    return self.res
            self.grade()
            self.ev.note(f"GRADE {self.res['grade']}: {self.checks}")
            return self.res
        finally:
            if "pre" in self.res and self.res.get("grade") != "BLOCKED":
                self.restore()
            elif self.console:
                self.console.close()
            self.ev.write("ota-result.json", self.res)


# ---- staging the server (--serve) --------------------------------------------------------------


def host_ip_toward(device_ip: str) -> str:
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        s.connect((device_ip.split(":")[0], 80))
        return s.getsockname()[0]
    finally:
        s.close()


def manifest_argv(version: str, key: str, out: str, url_base: str, images: Dict[str, str]) -> List[str]:
    """make_manifest.py for one image under each OTA variant it must answer to."""
    base = [sys.executable, str(REPO / "tools" / "make_manifest.py"), "--version", version, "--key", key]
    return base + ["--out", out, "--url-base", url_base, "--notes", "bench rehearsal"] + [f"{k}={v}" for k, v in images.items()]


def serve(image: str, version: str, key: str, device_ip: str, srv_dir: str, port: int = 8443) -> str:
    """Sign a manifest for the test image and serve it over TLS from this host. Returns the manifest's URL."""
    srv = Path(srv_dir)
    srv.mkdir(parents=True, exist_ok=True)
    served = srv / "firmware.bin"
    served.write_bytes(Path(image).read_bytes())
    crt, pem = srv / "srv.crt", srv / "srv.key"
    subprocess.run(
        ["openssl", "req", "-x509", "-newkey", "ec", "-pkeyopt", "ec_paramgen_curve:prime256v1", "-keyout", str(pem)]
        + ["-out", str(crt), "-days", "2", "-nodes", "-subj", "/CN=nimbus-ota-bench"],
        check=True,
        capture_output=True,
    )
    host = host_ip_toward(device_ip)
    base = f"https://{host}:{port}"
    argv = manifest_argv(version, key, str(srv / "manifest.json"), base, {"test-cyd": str(served), "freenove-28": str(served)})
    subprocess.run(argv, check=True, capture_output=True)
    handler = lambda *a, **kw: http.server.SimpleHTTPRequestHandler(*a, directory=str(srv), **kw)  # noqa: E731
    http.server.ThreadingHTTPServer.allow_reuse_address = True
    httpd = http.server.ThreadingHTTPServer((host, port), handler)
    ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    ctx.load_cert_chain(certfile=str(crt), keyfile=str(pem))
    httpd.socket = ctx.wrap_socket(httpd.socket, server_side=True)
    threading.Thread(target=httpd.serve_forever, daemon=True).start()
    return f"{base}/manifest.json"


def parse_args(argv=None) -> argparse.Namespace:
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--ip", required=True)
    ap.add_argument("--token-file", required=True)
    ap.add_argument("--port", required=True)
    ap.add_argument("--mac", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--url", default="", help="the manifest's https URL; or use --serve")
    ap.add_argument("--serve", metavar="FIRMWARE_BIN", help="sign and serve this image from here with the test key")
    ap.add_argument("--want-version", default="v5.0.0", help="the version the manifest offers")
    ap.add_argument("--want-build", required=True, help="the candidate's git describe; /api/state .build must start with it")
    ap.add_argument("--start-version", default="v4.6.4", help="the version the unit must be on before the update")
    ap.add_argument("--target-free", type=int, default=3936)
    ap.add_argument("--soak-s", type=float, default=900.0)
    ap.add_argument("--poll-s", type=float, default=5.0)
    ap.add_argument("--wait-s", type=float, default=3.0)
    ap.add_argument("--install-timeout", type=float, default=300.0)
    ap.add_argument("--validate-timeout", type=float, default=420.0)
    ap.add_argument("--list-provider", default="anthropic", help="whose model list must be restamped after the update")
    args = ap.parse_args(argv)
    if not (args.url or args.serve):
        ap.error("pass --url or --serve")
    return args


def main(argv=None) -> int:
    args = parse_args(argv)
    try:
        port_guard.check_mac(args.mac, "ota_rehearsal")
        port_guard.check_port(args.port, "ota_rehearsal")
    except port_guard.ForbiddenDevice as exc:
        print(exc, file=sys.stderr)
        return port_guard.REFUSED_EXIT
    if port_guard.port_for_mac(args.mac, "ota_rehearsal") != args.port:
        print(f"ota_rehearsal: {args.port} is not the board {args.mac}", file=sys.stderr)
        return port_guard.REFUSED_EXIT
    api = bd.Api(args.ip, bd.read_token(args.token_file))
    ev = bd.Evidence(args.out, api.scrub)
    if args.serve:
        key = str(REPO / "test" / "ota_test_key.pem")
        args.url = serve(args.serve, args.want_version, key, args.ip, ev.path("srv"))
    result = Rehearsal(args, api, ev, lambda: bd.Console(args.port, ev.path("console.log"), "ota_rehearsal")).run()
    print(json.dumps({"grade": result.get("grade"), "reason": result.get("reason"), "checks": result.get("checks")}))
    return 0 if result.get("grade") == "PASS" else 1


if __name__ == "__main__":
    raise SystemExit(main())
