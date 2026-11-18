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

import base64
import importlib.util
import json
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


def test_notes_ride_the_manifest_and_must_fit_the_device_field():
    """QW5 (owner decision D5): the release line lands in `notes`, and a line the
    device's char[96] field would cut (or a multi-line or non-ASCII one) is refused."""
    import json

    ok = "First public release: guided setup, honest errors, a firm monthly spend limit."
    assert M.notes_error(ok) == ""
    assert M.notes_error("x" * 95) == ""
    assert "96 bytes" in M.notes_error("x" * 96)
    assert M.notes_error("two\nlines") != ""
    assert M.notes_error("caf\u00e9") != ""
    with tempfile.TemporaryDirectory() as d:
        dd = pathlib.Path(d)
        fw = dd / "firmware-nimbus-tft.bin"
        fw.write_bytes(b"\x00" * (1024 * 1024))
        out = dd / "manifest.json"
        _cli("--version", "v5.0.0", "--key", str(KEY), "--out", str(out), "--notes", ok, f"nimbus-tft={fw}")
        assert json.loads(out.read_text())["notes"] == ok
        r = subprocess.run(
            [
                sys.executable,
                str(ROOT / "tools" / "make_manifest.py"),
                "--version",
                "v5.0.0",
                "--key",
                str(KEY),
                "--out",
                str(dd / "bad.json"),
                "--notes",
                "x" * 96,
                f"nimbus-tft={fw}",
            ],
            capture_output=True,
        )
        assert r.returncode != 0 and not (dd / "bad.json").exists()


# The holdHeads each line renders. The SAME literals are pinned in
# test/test_head_qual/main.cpp (kGoldenE1/kGoldenE2), rendered there by the C++
# table: change a row, change both goldens.
GOLDEN_HOLD = {
    "e1": ["*/*", "!anthropic/claude-haiku-5-5-*", "!anthropic/claude-haiku-5-5", "!anthropic/claude-sonnet-5-5-*", "!anthropic/claude-sonnet-5-5"],
    "e2": ["*/*"],
}


def test_hold_heads_golden():
    """The Python render of the HQ_ROW lines equals the C++ golden, line by line."""
    for line, want in GOLDEN_HOLD.items():
        assert M.render_hold_heads(line) == want, line
        got = _cli("--print-hold-heads", line).decode().strip()
        assert json.loads(got) == want


def _held(entries, host, model):
    """The device's reading (ota_logic.cpp headHeld): the last matching entry wins."""
    if not entries:
        return False
    if not host or not model:
        return True
    held = False
    for e in entries:
        exc = e.startswith("!")
        h, pat = e.lstrip("!").split("/", 1)
        hit_host = h in ("*", host)
        hit = pat == "*" or (pat.endswith("*") and model.startswith(pat[:-1])) or pat == model
        if hit_host and hit:
            held = not exc
    return held


def test_render_keeps_first_match_semantics():
    """Over synthetic tables with overlapping families, the rendered hold says what the
    table's first-match lookup says, for every row and both lines."""
    tables = [
        [("openai", "gpt-5.6-luna*", "Q", "A"), ("openai", "gpt-5.6*", "A", "Q"), ("openai", "*", "H", "H")],
        [("x", "a*", "A", "Q"), ("x", "*", "Q", "H"), ("y", "*", "Q", "Q")],
    ]
    for rows in tables:
        for line, col in (("e1", 2), ("e2", 3)):
            hold = M.render_hold_heads(line, rows)
            for host, fam, *_ in rows:
                model = fam[:-1] + "z" if fam.endswith("*") else fam
                first = next(r for r in rows if r[0] == host and _held([f"{r[0]}/{r[1]}"], host, model))
                assert _held(hold, host, model) == (first[col] != "Q"), (rows, line, host, model)
            assert _held(hold, "unknown-host", "m")


def test_hold_entry_rule_matches_the_device():
    """The entries ota_logic.cpp refuses are refused here too, so no manifest the
    device would reject whole is ever written."""
    bad = [
        "mistral",
        "/x",
        "mistral/",
        "Mistral/*",
        "mi*/*",
        "mistral/a*b",
        "mistral/a b",
        "mistral/a/b",
        "!",
        "",
        "mistral/" + "a" * 60,
        'mistral/a"b',
    ]
    for e in bad:
        assert not M.hold_entry_ok(e), e
    for e in ["*/*", "!anthropic/claude-sonnet-5-5*", "zai/glm-5.3", "open_ai/x-1"]:
        assert M.hold_entry_ok(e), e
    assert M.hold_error([f"h{i}/*" for i in range(24)]) == ""
    assert "entries" in M.hold_error([f"h{i}/*" for i in range(25)])
    assert M.hold_error(["Mistral/*"]) != ""


def test_hold_line_rides_the_manifest_unsigned():
    """--hold-line adds holdHeads; the signed bytes and the signature verify exactly as
    without it, so v5.1.1 and v5.1.2 devices read the manifest unchanged."""
    with tempfile.TemporaryDirectory() as d:
        dd = pathlib.Path(d)
        fw = dd / "firmware-nimbus-tft.bin"
        fw.write_bytes(b"\x01" * (1024 * 1024))
        plain, held = dd / "plain.json", dd / "held.json"
        _cli("--version", "v5.2.0", "--key", str(KEY), "--out", str(plain), f"nimbus-tft={fw}")
        _cli("--version", "v5.2.0", "--key", str(KEY), "--out", str(held), "--hold-line", "e1", f"nimbus-tft={fw}")
        p, h = json.loads(plain.read_text()), json.loads(held.read_text())
        assert "holdHeads" not in p
        assert h["holdHeads"] == GOLDEN_HOLD["e1"]
        assert {k: v for k, v in h.items() if k != "holdHeads"}.keys() == p.keys()
        v = h["variants"]["nimbus-tft"]
        assert v["sha256"] == p["variants"]["nimbus-tft"]["sha256"]
        msg = M.sig_message(h["version"], "nimbus-tft", v["sha256"])
        (dd / "sig.der").write_bytes(base64.b64decode(v["sig"]))
        (dd / "msg.bin").write_bytes(msg)
        r = subprocess.run(
            ["openssl", "dgst", "-sha256", "-verify", str(PUB), "-signature", str(dd / "sig.der"), str(dd / "msg.bin")],
            capture_output=True,
        )
        assert r.returncode == 0, r.stdout + r.stderr


if __name__ == "__main__":
    test_print_message_golden_v2()
    test_print_message_schema1_transition()
    test_sign_verify_roundtrip()
    test_notes_ride_the_manifest_and_must_fit_the_device_field()
    test_hold_heads_golden()
    test_render_keeps_first_match_semantics()
    test_hold_entry_rule_matches_the_device()
    test_hold_line_rides_the_manifest_unsigned()
    print("make_manifest: v2 golden byte-locked; v1 transition intact; sign/verify round-trip OK")
