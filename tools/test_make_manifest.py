#!/usr/bin/env python3
"""Self-test for make_manifest.py - the byte-locked partner of the on-device signer.

Runnable directly (`python3 tools/test_make_manifest.py`) or under pytest; needs
no board. Uses openssl for the sign/verify round-trip (same dependency the tool
itself has).

Why this exists: the OTA signed message is a contract split across two languages.
tools/make_manifest.py signs it in Python; nimbus::ota::buildSigMessage() rebuilds
it in C++ on the device and verifies the signature over it. If the two ever
disagree by a single byte, every signature check fails in the field - silently, on
hardware nobody is watching. So the exact golden bytes are pinned HERE and, in
lockstep, in test/test_ota_logic/main.cpp::test_sig_message_golden. Change one, you
must change the other; this test and that Unity test are the two ends of the lock.
"""

from __future__ import annotations

import importlib.util
import pathlib
import subprocess
import sys
import tempfile

ROOT = pathlib.Path(__file__).resolve().parent.parent
KEY = ROOT / "test" / "ota_test_key.pem"
PUB = ROOT / "test" / "ota_test_key.pub.pem"


def _load():
    p = pathlib.Path(__file__).with_name("make_manifest.py")
    spec = importlib.util.spec_from_file_location("make_manifest", p)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


M = _load()

SHA64 = "a" * 64


def _cli(*args: str) -> bytes:
    """Run make_manifest.py as a subprocess, return stdout bytes."""
    return subprocess.run(
        [sys.executable, str(ROOT / "tools" / "make_manifest.py"), *args],
        check=True,
        capture_output=True,
    ).stdout


def test_print_message_golden_v2():
    """schema 2 default: exact bytes must equal the C++ golden."""
    want = b"nimbus-ota-v2\nv4.3.0\nnimbus-tft\n" + b"a" * 64 + b"\n"
    # In-process (the function the signer calls).
    assert M.sig_message("v4.3.0", "nimbus-tft", SHA64) == want
    assert M.sig_message("v4.3.0", "nimbus-tft", SHA64, 2) == want
    # Through the CLI (--print-message), the exact path CI/goldens cross-check.
    got = _cli("--print-message", "--version", "v4.3.0", "--variant", "nimbus-tft", "--sha", SHA64)
    assert got == want, f"CLI print-message drifted: {got!r} != {want!r}"


def test_print_message_schema1_transition():
    """schema 1 stays available for the transition manifest, with the v1 prefix."""
    want = b"nimbus-ota-v1\nv4.2.0\nesp32s3\n" + b"a" * 64 + b"\n"
    assert M.sig_message("v4.2.0", "esp32s3", SHA64, 1) == want
    got = _cli("--print-message", "--schema", "1", "--version", "v4.2.0", "--variant", "esp32s3", "--sha", SHA64)
    assert got == want


def test_sign_verify_roundtrip():
    """A full signed manifest verifies against the public key - end to end.

    Proves the bytes make_manifest signs are the bytes that verify: build a typed
    manifest over a synthetic in-range firmware image with the committed test key,
    then check each variant's signature with openssl over the rebuilt message.
    """
    import base64
    import json

    with tempfile.TemporaryDirectory() as d:
        dd = pathlib.Path(d)
        # A firmware image inside [kMinFwBytes=512KB, kMaxFwBytes=6.5MB].
        fw = dd / "firmware-nimbus-tft.bin"
        fw.write_bytes(b"\x00" * (3 * 1024 * 1024))
        out = dd / "manifest.json"
        _cli(
            "--version",
            "v4.3.0",
            "--key",
            str(KEY),
            "--out",
            str(out),
            "--url-base",
            "https://example.invalid/v4.3.0",
            f"nimbus-tft={fw}",
        )
        man = json.loads(out.read_text())
        assert man["schema"] == 2, man
        assert set(man["variants"]) == {"nimbus-tft"}, man
        v = man["variants"]["nimbus-tft"]
        msg = M.sig_message(man["version"], "nimbus-tft", v["sha256"])
        sig = dd / "sig.der"
        sig.write_bytes(base64.b64decode(v["sig"]))
        msgf = dd / "msg.bin"
        msgf.write_bytes(msg)
        # openssl exit 0 == "Verified OK".
        r = subprocess.run(
            ["openssl", "dgst", "-sha256", "-verify", str(PUB), "-signature", str(sig), str(msgf)],
            capture_output=True,
        )
        assert r.returncode == 0, r.stdout + r.stderr


if __name__ == "__main__":
    test_print_message_golden_v2()
    test_print_message_schema1_transition()
    test_sign_verify_roundtrip()
    print("make_manifest: v2 golden byte-locked; v1 transition intact; sign/verify round-trip OK")
