#!/usr/bin/env python3
"""Build + sign the OTA release manifest.

Used by .github/workflows/release.yml (real key from the OTA_SIGNING_KEY
secret) and by tests/hil/test_ota.py (the committed test key). The canonical
signed message MUST stay byte-identical to nimbus::ota::buildSigMessage()
(lib/core/src/ota_logic.cpp - golden-tested in test/test_ota_logic):

    nimbus-ota-v<schema>\n<version>\n<variant>\n<sha256-hex-lowercase>\n

where <schema> is 2 for the typed manifest (default) and 1 for the legacy
transition manifest existing fielded firmware still consumes. In schema 2 the
variant key is the device TYPE (nimbus-tft, freenove-28/35/40); in schema 1 it
is the compile-time build tag (esp32s3, cyd, test).

Signature: ECDSA P-256 over SHA-256 of that message, DER, base64.

holdHeads (--hold-line e1|e2): the heads the release must not install itself on,
rendered from the head qualification table (the HQ_ROW lines of
lib/core/src/orch_head_qual.cpp; its C++ twin is headqual::renderHoldHeads, and
test/test_head_qual pins the same golden as tools/test_make_manifest.py). The field
is NOT signed: it is an availability control, not a security one (docs/ota.md), so
the signed message and every device before RB-OTA1 are unaffected.

Usage:
  make_manifest.py --version v4.3.0 --key priv.pem --out manifest.json \
      [--schema 2] [--notes "..."] [--min-version vX.Y.Z] [--build <describe>] \
      [--url-base https://github.com/OWNER/REPO/releases/download/v4.3.0] \
      [--hold-line e1] \
      nimbus-tft=firmware-nimbus-tft.bin freenove-28=firmware-freenove.bin

  make_manifest.py --print-hold-heads e1    # the holdHeads a line renders (no signing)

  make_manifest.py --print-message --schema 2 --version v4.3.0 \
      --variant nimbus-tft --sha <64-hex>    # golden cross-check (no signing)
"""

import argparse
import base64
import hashlib
import json
import re
import subprocess
import sys
import tempfile
from pathlib import Path


# The device reads `notes` into char[96] (lib/core ota_logic.h): 95 bytes survive.
NOTES_MAX_BYTES = 95

# The device's holdHeads limits (lib/core ota_logic.h kMaxHoldHeads, kHoldEntryMax,
# kHoldHeadsCap). A manifest past them is refused whole by the device, so never write one.
HOLD_MAX_ENTRIES = 24
HOLD_ENTRY_MAX = 63
HOLD_TOTAL_MAX = 511
HOLD_ENTRY_RE = re.compile(r"^!?(\*|[a-z0-9_-]+)/[!-~]*$")
HEAD_QUAL_CPP = Path(__file__).resolve().parent.parent / "lib" / "core" / "src" / "orch_head_qual.cpp"
HQ_ROW_RE = re.compile(r'^\s*HQ_ROW\("([^"]+)",\s*"([^"]+)",\s*\w+,\s*([QAH]),\s*([QAH]),')


def head_qual_rows(cpp: Path = HEAD_QUAL_CPP) -> list:
    """(host, family, e1 state, e2 state) per HQ_ROW line, in table order."""
    rows = [m.groups() for line in cpp.read_text().splitlines() if (m := HQ_ROW_RE.match(line))]
    if not rows:
        sys.exit(f"no HQ_ROW lines in {cpp}")
    return rows


def hold_entry_ok(e: str) -> bool:
    """The device's entry rule (ota_logic.cpp holdEntryOk)."""
    if not (0 < len(e) <= HOLD_ENTRY_MAX) or not HOLD_ENTRY_RE.match(e):
        return False
    model = e.split("/", 1)[1]
    bad = any(c in model for c in ' "\\/') or ("*" in model[:-1])
    return bool(model) and not bad


def render_hold_heads(line: str, rows: list | None = None) -> list:
    """headqual::renderHoldHeads: hold everything, then except each qualified row in
    reverse match order (later entries win on the device); a hold entry before any
    exception on its host changes no verdict and is left out."""
    if line not in ("e1", "e2"):
        sys.exit(f"--hold-line must be e1 or e2, not {line!r}")
    col = 2 if line == "e1" else 3
    out, excepted = ["*/*"], set()
    for row in reversed(rows if rows is not None else head_qual_rows()):
        host, family, qualified = row[0], row[1], row[col] == "Q"
        if not qualified and host not in excepted:
            continue
        if qualified:
            excepted.add(host)
        out.append(("!" if qualified else "") + f"{host}/{family}")
    return out


def hold_error(entries: list) -> str:
    """Return "" when the device accepts `entries` as holdHeads, else why not."""
    if len(entries) > HOLD_MAX_ENTRIES:
        return f"holdHeads has {len(entries)} entries; the device keeps {HOLD_MAX_ENTRIES}"
    if bad := [e for e in entries if not hold_entry_ok(e)]:
        return f"holdHeads entries the device refuses: {bad}"
    if len("\n".join(entries)) > HOLD_TOTAL_MAX:
        return "holdHeads is longer than the device's field"
    return ""


def notes_error(notes: str) -> str:
    """Return "" when `notes` fits the device's notes field intact, else why not.

    The release gate (tools/release_gate/check_release_notes.py) holds the full rule;
    this is the last check before the line is written into a manifest. The line is
    not part of the signed message (sig_message): the device treats it as plain text.
    """
    if "\n" in notes or "\r" in notes:
        return "notes must be one line"
    b = notes.encode("utf-8")
    if len(b) > NOTES_MAX_BYTES:
        return f"notes are {len(b)} bytes; the device keeps {NOTES_MAX_BYTES}"
    if any(c < 0x20 or c > 0x7E for c in b):
        return "notes must be printable ASCII"
    return ""


def sig_message(version: str, variant: str, sha_hex: str, schema: int = 2) -> bytes:
    return f"nimbus-ota-v{schema}\n{version}\n{variant}\n{sha_hex}\n".encode()


def sign(key_pem: Path, message: bytes) -> str:
    with tempfile.NamedTemporaryFile(suffix=".msg") as m:
        m.write(message)
        m.flush()
        der = subprocess.run(
            ["openssl", "dgst", "-sha256", "-sign", str(key_pem), m.name], check=True, capture_output=True
        ).stdout
    if not (8 <= len(der) <= 80):
        sys.exit(f"unexpected signature length {len(der)} (want DER ECDSA <= 80)")
    return base64.b64encode(der).decode()


def parse_args() -> argparse.Namespace:
    ap = argparse.ArgumentParser()
    ap.add_argument("--version")
    ap.add_argument("--key")
    ap.add_argument("--out")
    ap.add_argument("--schema", type=int, default=2, choices=(1, 2))
    ap.add_argument("--notes", default="")
    ap.add_argument("--min-version", default="")
    ap.add_argument("--build", default="")
    ap.add_argument("--url-base", default="")
    ap.add_argument("--hold-line", default="", help="render holdHeads for this release line (e1|e2)")
    ap.add_argument("--print-hold-heads", metavar="LINE", help="print the holdHeads LINE renders and exit")
    ap.add_argument("--print-message", action="store_true")
    ap.add_argument("--variant")
    ap.add_argument("--sha")
    ap.add_argument("pairs", nargs="*", metavar="variant=firmware.bin")
    return ap.parse_args()


def print_only(a: argparse.Namespace) -> bool:
    """The no-signing modes. True when one ran."""
    if a.print_hold_heads:
        print(json.dumps(render_hold_heads(a.print_hold_heads), separators=(",", ":")))
        return True
    if not a.version:
        sys.exit("--version is required")
    if not a.print_message:
        return False
    if not (a.variant and a.sha):
        sys.exit("--print-message needs --variant and --sha")
    sys.stdout.buffer.write(sig_message(a.version, a.variant, a.sha, a.schema))
    return True


def signed_variants(a: argparse.Namespace, url_base: str) -> dict:
    variants = {}
    for pair in a.pairs:
        variant, _, path = pair.partition("=")
        p = Path(path)
        if not (variant and p.is_file()):
            sys.exit(f"bad pair {pair!r}")
        blob = p.read_bytes()
        sha_hex = hashlib.sha256(blob).hexdigest()
        variants[variant] = {
            "url": f"{url_base}/{p.name}",
            "size": len(blob),
            "sha256": sha_hex,
            "sig": sign(Path(a.key), sig_message(a.version, variant, sha_hex, a.schema)),
        }
        print(f"{variant}: {p.name} {len(blob)} bytes sha256={sha_hex[:16]}…")
    return variants


def main() -> None:
    a = parse_args()
    if print_only(a):
        return
    if not (a.key and a.out and a.pairs):
        sys.exit("need --key, --out and at least one variant=file pair")
    if err := notes_error(a.notes):
        sys.exit(err)
    hold = render_hold_heads(a.hold_line) if a.hold_line else None
    if hold is not None and (err := hold_error(hold)):
        sys.exit(err)
    url_base = a.url_base.rstrip("/") or (
        f"https://github.com/ristllin/nimbus-fw-releases/releases/download/{a.version}"
    )
    variants = signed_variants(a, url_base)

    manifest = {"schema": a.schema, "version": a.version}
    if a.build:
        manifest["build"] = a.build
    if a.notes:
        manifest["notes"] = a.notes
    if a.min_version:
        manifest["minVersion"] = a.min_version
    if hold is not None:
        manifest["holdHeads"] = hold
    manifest["variants"] = variants
    Path(a.out).write_text(json.dumps(manifest, indent=1) + "\n")
    print(f"wrote {a.out} ({len(variants)} variants)")


if __name__ == "__main__":
    main()
