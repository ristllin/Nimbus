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

Usage:
  make_manifest.py --version v4.3.0 --key priv.pem --out manifest.json \
      [--schema 2] [--notes "..."] [--min-version vX.Y.Z] [--build <describe>] \
      [--url-base https://github.com/OWNER/REPO/releases/download/v4.3.0] \
      nimbus-tft=firmware-nimbus-tft.bin freenove-28=firmware-freenove.bin

  make_manifest.py --print-message --schema 2 --version v4.3.0 \
      --variant nimbus-tft --sha <64-hex>    # golden cross-check (no signing)
"""

import argparse
import base64
import hashlib
import json
import subprocess
import sys
import tempfile
from pathlib import Path


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


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--version", required=True)
    ap.add_argument("--key")
    ap.add_argument("--out")
    ap.add_argument("--schema", type=int, default=2, choices=(1, 2))
    ap.add_argument("--notes", default="")
    ap.add_argument("--min-version", default="")
    ap.add_argument("--build", default="")
    ap.add_argument("--url-base", default="")
    ap.add_argument("--print-message", action="store_true")
    ap.add_argument("--variant")
    ap.add_argument("--sha")
    ap.add_argument("pairs", nargs="*", metavar="variant=firmware.bin")
    a = ap.parse_args()

    if a.print_message:
        if not (a.variant and a.sha):
            sys.exit("--print-message needs --variant and --sha")
        sys.stdout.buffer.write(sig_message(a.version, a.variant, a.sha, a.schema))
        return

    if not (a.key and a.out and a.pairs):
        sys.exit("need --key, --out and at least one variant=file pair")
    url_base = a.url_base.rstrip("/") or (
        f"https://github.com/ristllin/nimbus-fw-releases/releases/download/{a.version}"
    )

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

    manifest = {"schema": a.schema, "version": a.version}
    if a.build:
        manifest["build"] = a.build
    if a.notes:
        manifest["notes"] = a.notes
    if a.min_version:
        manifest["minVersion"] = a.min_version
    manifest["variants"] = variants
    Path(a.out).write_text(json.dumps(manifest, indent=1) + "\n")
    print(f"wrote {a.out} ({len(variants)} variants)")


if __name__ == "__main__":
    main()
