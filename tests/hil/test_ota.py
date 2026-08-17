"""OTA update HIL suite (src/sys/ota_update + lib/core nimbus::ota).

Drives the REAL device OTA pipeline against a LOCAL TLS server: a self-signed
cert (the suite flips ``tlsVerify`` off for its duration and restores it), a
manifest signed with the COMMITTED test key (trusted only by NIMBUS_TEST
builds - include/ota_pubkey.h), and the device's own current ``env:test``
firmware.bin as the payload. The dry-run flow exercises download → inactive-slot
flash write → streaming SHA-256 → ECDSA verify END TO END without flipping the
boot partition, so a green run leaves the device exactly as it started.

Negatives: a bit-flipped binary must die with ``sha-fail``; a manifest signed by
a WRONG key must die with ``sig-fail``; the binary URL goes through a 302
redirect hop (the GitHub asset-URL shape).

Markers: ``net`` (LAN + serial console). The real-GitHub install path is the
Phase-5 manual acceptance (docs/ota.md), not this suite.
"""

from __future__ import annotations

import http.server
import os
import socket
import ssl
import subprocess
import threading
import time
from pathlib import Path

import pytest

from test_l4_network import lan_ip_or_skip
from test_l9_resilience import _webtok

pytestmark = [pytest.mark.net]

REPO = Path(__file__).resolve().parents[2]
TEST_KEY = REPO / "test" / "ota_test_key.pem"
FW_BIN = REPO / ".pio" / "build" / "test" / "firmware.bin"
MAKE_MANIFEST = REPO / "tools" / "make_manifest.py"
SERVED_VERSION = "v99.0.0"  # always newer than the running firmware
PORT = 8443


# ---- local TLS fixture server ----------------------------------------------


class _Handler(http.server.SimpleHTTPRequestHandler):
    def do_GET(self):  # noqa: N802 - http.server API
        # /redir/<name> 302s to /<name> - the GitHub asset-URL shape the device
        # client must follow (cross-path here; cross-host in production).
        if self.path.startswith("/redir/"):
            self.send_response(302)
            self.send_header("Location", "/" + self.path[len("/redir/") :])
            self.end_headers()
            return
        super().do_GET()

    def log_message(self, *a):  # keep pytest output clean
        pass


def _host_ip_toward(device_ip: str) -> str:
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        s.connect((device_ip, 80))
        return s.getsockname()[0]
    finally:
        s.close()


def _make_cert(tmp: Path) -> tuple[Path, Path]:
    crt, key = tmp / "srv.crt", tmp / "srv.key"
    subprocess.run(
        [
            "openssl",
            "req",
            "-x509",
            "-newkey",
            "ec",
            "-pkeyopt",
            "ec_paramgen_curve:prime256v1",
            "-keyout",
            str(key),
            "-out",
            str(crt),
            "-days",
            "2",
            "-nodes",
            "-subj",
            "/CN=nimbus-ota-hil",
        ],
        check=True,
        capture_output=True,
    )
    return crt, key


def _sign_manifest(tmp: Path, key: Path, url_base: str, fw: Path) -> Path:
    out = tmp / "manifest.json"
    subprocess.run(
        [
            "python3",
            str(MAKE_MANIFEST),
            "--version",
            SERVED_VERSION,
            "--key",
            str(key),
            "--out",
            str(out),
            "--url-base",
            url_base,
            "--notes",
            "hil fixture",
            f"test={fw}",
        ],
        check=True,
        capture_output=True,
    )
    return out


def _reachable_ip_or_skip(device, net, secrets, require_secret) -> str:
    """Resolve the device LAN IP, patient about the CH34x rejoin race.

    ``lan_ip_or_skip``'s NIMBUS_TEST_IP fast path only polls ~30 s, which the
    session-start serial reset can outlast (the board drops WiFi with reason=8
    and rejoins slowly - see AGENTS.md). That skipped the early OTA tests while
    the board was still coming back. Here we poll the override IP up to 90 s
    before delegating, so a slow rejoin waits instead of skipping."""
    override = os.environ.get("NIMBUS_TEST_IP")
    if override:
        deadline = time.time() + 90.0
        while time.time() < deadline:
            try:
                net.get_json("/api/state", ip=override, timeout=4.0)
                return override
            except Exception:  # noqa: BLE001 - still rejoining; retry
                time.sleep(3.0)
    return lan_ip_or_skip(device, net, secrets, require_secret)


@pytest.fixture
def ota_server(tmp_path, device, net, secrets, require_secret):
    """Local HTTPS fixture server + device pointed at it. Yields (ip, tok, base,
    tmp_path); restores tlsVerify + the manifest-URL override afterwards."""
    if not FW_BIN.is_file():
        pytest.skip("no env:test firmware.bin - run `pio run -e test` first")
    ip = _reachable_ip_or_skip(device, net, secrets, require_secret)
    tok = _webtok(device)

    host = _host_ip_toward(ip)
    serve_dir = tmp_path / "serve"
    serve_dir.mkdir()
    (serve_dir / "firmware-test.bin").write_bytes(FW_BIN.read_bytes())

    crt, key = _make_cert(tmp_path)
    # SO_REUSEADDR + an explicit server_close() below. Without both, this
    # function-scoped fixture leaks its listening socket to the next test and
    # every subsequent OTA test dies with "Address already in use" - two errors
    # in every full run, which is exactly how a suite teaches people to stop
    # reading its errors.
    http.server.ThreadingHTTPServer.allow_reuse_address = True
    httpd = http.server.ThreadingHTTPServer((host, PORT), lambda *a, **kw: _Handler(*a, directory=str(serve_dir), **kw))
    ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    ctx.load_cert_chain(certfile=str(crt), keyfile=str(key))
    httpd.socket = ctx.wrap_socket(httpd.socket, server_side=True)
    t = threading.Thread(target=httpd.serve_forever, daemon=True)
    t.start()

    # Self-signed cert: drop TLS validation for the suite (restored below).
    r = net.post("/api/orch", {"tlsVerify": "0", "t": tok}, ip=ip)
    assert r.status_code == 200
    base = f"https://{host}:{PORT}"
    try:
        yield ip, tok, base, serve_dir
    finally:
        try:
            # In a LAN-only run these raise pytest.skip - inside TEARDOWN, which
            # pytest reports as an ERROR on an otherwise-passing test. The
            # device-state restore genuinely cannot run without the console, so
            # swallow it here and let the test body's own skip speak.
            try:
                device.send("OTAURL clear")  # unknown URL -> next check refuses fast
                device.send("OTASIM clear")
            except BaseException:  # noqa: BLE001 - includes pytest's Skipped
                pass
            net.post("/api/orch", {"tlsVerify": "1", "t": tok}, ip=ip)
        finally:
            httpd.shutdown()
            httpd.server_close()  # release the port for the next test


def _ota_state(device) -> dict:
    m = device.cmd_re(
        "OTA?",
        r"OTA state=(?P<state>\S+) err=(?P<err>\S+) pct=(?P<pct>-?\d+) latest=(?P<latest>\S*) "
        r"notes=(?P<notes>.*?) lastOta=(?P<last>.*?) pend=(?P<pend>\d+) "
        r"boots=(?P<boots>\d+) prev=(?P<prev>\S*) slot=(?P<slot>\S+)",
        timeout=6.0,
    )
    return m.groupdict()


def _wait_state(device, want, timeout=300.0, forbid=("error",)):
    deadline = time.time() + timeout
    last = {}
    while time.time() < deadline:
        last = _ota_state(device)
        if last["state"] == want:
            return last
        if last["state"] in forbid:
            pytest.fail(
                f"OTA entered {last['state']} (err={last.get('err')} lastOta={last['last']}) while waiting for {want}"
            )
        time.sleep(3.0)
    pytest.fail(f"timeout waiting for OTA state {want}; last={last}")


def _point_at(device, base: str, name="manifest.json"):
    device.cmd_re(f"OTAURL {base}/{name}", r"OTAURL ", timeout=5.0)


def _start(device, cmd: str):
    m = device.cmd_re(cmd, cmd.split()[0] + r" (started|refused.*)", timeout=6.0)
    assert m.group(1) == "started", f"{cmd} -> {m.group(1)}"


# ---- tests ------------------------------------------------------------------


def test_check_finds_release(ota_server, device):
    """Manifest fetch + parse + eligibility: served v99.0.0 must become
    state=available with the version + notes surfaced."""
    ip, tok, base, serve_dir = ota_server
    _sign_manifest(serve_dir.parent, TEST_KEY, base, serve_dir / "firmware-test.bin")
    os.replace(serve_dir.parent / "manifest.json", serve_dir / "manifest.json")
    _point_at(device, base)
    _start(device, "OTACHECK")
    st = _wait_state(device, "available", timeout=60.0)
    assert st["latest"] == SERVED_VERSION


def test_dry_run_verifies_end_to_end(ota_server, device):
    """The big one: full download into the inactive slot + SHA-256 + ECDSA
    verify, then Update.abort() - no flip, no reboot. Also covers the 302
    redirect hop (the binary URL goes through /redir/)."""
    ip, tok, base, serve_dir = ota_server
    _sign_manifest(serve_dir.parent, TEST_KEY, f"{base}/redir", serve_dir / "firmware-test.bin")
    os.replace(serve_dir.parent / "manifest.json", serve_dir / "manifest.json")
    _point_at(device, base)
    _start(device, "OTACHECK")
    _wait_state(device, "available", timeout=60.0)

    _start(device, "OTAAPPLY dry")
    st = _wait_state(device, "available", timeout=420.0)  # ~3 MB over LAN TLS
    assert st["last"].startswith(f"dryrun ok {SERVED_VERSION}"), st
    assert st["pend"] == "0"  # never armed the rollback guard


def test_bitflip_binary_sha_fails(ota_server, device):
    """A corrupted binary (same size, one byte flipped AFTER signing) must be
    rejected by the streaming SHA check - and never arm the guard."""
    ip, tok, base, serve_dir = ota_server
    _sign_manifest(serve_dir.parent, TEST_KEY, base, serve_dir / "firmware-test.bin")
    os.replace(serve_dir.parent / "manifest.json", serve_dir / "manifest.json")
    blob = bytearray((serve_dir / "firmware-test.bin").read_bytes())
    blob[len(blob) // 2] ^= 0xFF
    (serve_dir / "firmware-test.bin").write_bytes(bytes(blob))

    _point_at(device, base)
    _start(device, "OTACHECK")
    _wait_state(device, "available", timeout=60.0)
    _start(device, "OTAAPPLY dry")
    st = _wait_state(device, "error", timeout=420.0, forbid=())
    assert st["last"].startswith("sha-fail"), st
    assert st["pend"] == "0"


def test_wrong_key_sig_fails(ota_server, device, tmp_path):
    """A manifest signed by an attacker key (valid sha, wrong signer) must be
    rejected by the ECDSA verify - the sha alone is NOT trusted. The signature is
    verified at CHECK time (over the signed version+variant+sha), so a forged
    manifest never even reaches 'available' - it can't spoof the notification or
    arm auto-install. No download happens."""
    ip, tok, base, serve_dir = ota_server
    rogue = tmp_path / "rogue.pem"
    subprocess.run(
        ["openssl", "ecparam", "-name", "prime256v1", "-genkey", "-noout", "-out", str(rogue)],
        check=True,
        capture_output=True,
    )
    _sign_manifest(serve_dir.parent, rogue, base, serve_dir / "firmware-test.bin")
    os.replace(serve_dir.parent / "manifest.json", serve_dir / "manifest.json")

    _point_at(device, base)
    _start(device, "OTACHECK")
    st = _wait_state(device, "error", timeout=60.0, forbid=("available",))
    assert st["err"] == "sig-fail", st
    assert st["pend"] == "0"


def test_web_api_check_token_gated(ota_server, device, net):
    """POST /api/ota/check without the token -> 401; with it -> 202/409."""
    ip, tok, base, serve_dir = ota_server
    # auth=False is REQUIRED, not decoration: net.post() attaches the device
    # token by default, and net.token() prefers NIMBUS_TEST_TOKEN from the
    # environment - so in a LAN-only run this "unauthenticated" call was sending
    # a valid token and getting a correct 202, which the assertion then reported
    # as an auth hole. Every other 401 test in this suite already passes
    # auth=False; this one did not. The device was verified correct by hand:
    # no token -> 401, wrong token -> 401, right token -> 202.
    r = net.post("/api/ota/check", {}, ip=ip, auth=False)
    assert r.status_code == 401
    r = net.post("/api/ota/check", {"t": tok}, ip=ip)
    assert r.status_code in (202, 409)
    state = net.get_json(f"/api/state?t={tok}", ip=ip, timeout=5.0)
    assert "ota" in state and "lastOta" in state
