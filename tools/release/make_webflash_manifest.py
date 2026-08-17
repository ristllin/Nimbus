#!/usr/bin/env python3
"""make_webflash_manifest - build the browser-flasher artifacts for a release.

Produces two files for ESP Web Tools (https://esphome.github.io/esp-web-tools/):

  nimbus-webflash.bin   one merged, complete flash image (bootloader + partition
                        table + otadata/boot_app0 + app), flashable at offset 0
  manifest.json         the ESP Web Tools manifest pointing at that image

The merged image is produced with `esptool merge-bin` from the artifacts of a
PlatformIO build of [env:esp32s3]. The offsets are the standard Arduino-ESP32
layout for this board, verified against boards' default_16MB.csv partition
table (otadata @ 0xe000, app0 @ 0x10000) and the factory image the pioarduino
builder itself emits:

    0x0      bootloader.bin      (ESP32-S3 boots the 2nd-stage loader at 0x0)
    0x8000   partitions.bin
    0xe000   boot_app0.bin       (otadata: boot from app0 - framework package)
    0x10000  firmware.bin

When the build dir already contains `firmware.factory.bin` (pioarduino emits
one with this exact layout), the merged output is byte-compared against it as
a sanity gate.

Usage:
  python3 tools/release/make_webflash_manifest.py \
      --build-dir .pio/build/esp32s3 --version v4.2.0 --out-dir webflash \
      [--base-url https://raw.githubusercontent.com/.../latest]

With --base-url the manifest's part path is absolute (needed when the manifest
and the image are fetched cross-origin, e.g. from raw.githubusercontent.com);
without it the path is relative ("nimbus-webflash.bin").
"""

from __future__ import annotations

import argparse
import glob
import hashlib
import json
import os
import subprocess
import sys
from pathlib import Path

# (offset, filename) - see the module docstring for why these values.
PARTS = [
    (0x0, "bootloader.bin"),
    (0x8000, "partitions.bin"),
    (0xE000, "boot_app0.bin"),
    (0x10000, "firmware.bin"),
]

IMAGE_NAME = "nimbus-webflash.bin"


def find_boot_app0() -> Path:
    """boot_app0.bin ships in the Arduino framework package, not the build dir."""
    home = Path(os.path.expanduser("~/.platformio/packages"))
    candidates = glob.glob(str(home / "framework-arduinoespressif32*" / "tools" / "partitions" / "boot_app0.bin"))
    if not candidates:
        sys.exit(
            "FAIL: boot_app0.bin not found under ~/.platformio/packages "
            "(run `pio run -e esp32s3` first so the framework package exists)"
        )
    # With multiple framework installs, pick the most recently used (the one the
    # pinned platform resolved to) rather than a lexicographic guess.
    chosen = max(candidates, key=lambda p: os.path.getmtime(p))
    if len(candidates) > 1:
        print(f"NOTE: {len(candidates)} framework packages found; using {chosen}")
    return Path(chosen)


def find_esptool() -> list[str]:
    """Return an argv prefix that runs esptool (module, PATH, or the pio package)."""
    try:
        import esptool  # noqa: F401

        return [sys.executable, "-m", "esptool"]
    except ImportError:
        pass
    import shutil

    for name in ("esptool", "esptool.py"):
        exe = shutil.which(name)
        if exe:
            return [exe]
    pio_py = Path(os.path.expanduser("~/.platformio/penv/bin/python"))
    pio_tool = Path(os.path.expanduser("~/.platformio/packages/tool-esptoolpy/esptool.py"))
    if pio_py.exists() and pio_tool.exists():
        return [str(pio_py), str(pio_tool)]
    sys.exit("FAIL: no esptool found (pip install esptool, or install PlatformIO)")


def merge(esptool_argv: list[str], parts: list[tuple[int, Path]], out: Path) -> None:
    """Run esptool merge-bin; v5 renamed merge_bin/--flash_mode to dashed forms."""
    part_args: list[str] = []
    for off, p in parts:
        part_args += [hex(off), str(p)]
    attempts = []
    for cmd, fm, fs in (
        (["merge-bin"], "--flash-mode", "--flash-size"),  # esptool >= 5
        (["merge_bin"], "--flash_mode", "--flash_size"),  # esptool 4.x
    ):
        argv = (
            esptool_argv
            + ["--chip", "esp32s3"]
            + cmd
            + [
                "-o",
                str(out),
                fm,
                "dio",
                fs,
                "16MB",
            ]
            + part_args
        )
        res = subprocess.run(argv, capture_output=True, text=True)
        if res.returncode == 0:
            return
        attempts.append(f"$ {' '.join(argv)}\n  rc={res.returncode}\n{res.stdout}{res.stderr}")
    # Both invocations failed - surface BOTH, so an esptool-v5 error isn't hidden
    # behind the v4 fallback's "unrecognized arguments".
    sys.exit("FAIL: esptool merge failed (both arg styles tried):\n\n" + "\n\n".join(attempts))


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument(
        "--build-dir", default=".pio/build/esp32s3", help="PlatformIO build dir with bootloader/partitions/firmware.bin"
    )
    ap.add_argument("--version", required=True, help="release version, e.g. v4.2.0")
    ap.add_argument("--out-dir", required=True, help="directory for the two outputs")
    ap.add_argument(
        "--base-url",
        default="",
        help="absolute URL prefix for the image in manifest.json (no trailing slash); empty = relative path",
    )
    args = ap.parse_args()

    build = Path(args.build_dir)
    out_dir = Path(args.out_dir)
    out_dir.mkdir(parents=True, exist_ok=True)

    parts: list[tuple[int, Path]] = []
    for off, name in PARTS:
        p = find_boot_app0() if name == "boot_app0.bin" else build / name
        if not p.is_file():
            sys.exit(f"FAIL: missing {p} - run `pio run -e esp32s3` first")
        parts.append((off, p))

    image = out_dir / IMAGE_NAME
    merge(find_esptool(), parts, image)

    # Shape sanity: a complete image must be at least app-offset + app-size, and
    # must start with the ESP image magic (0xE9) of the 2nd-stage bootloader.
    fw_size = (build / "firmware.bin").stat().st_size
    img = image.read_bytes()
    min_size = 0x10000 + fw_size
    if len(img) < min_size:
        sys.exit(f"FAIL: merged image {len(img)} B < expected {min_size} B")
    if img[0] != 0xE9:
        sys.exit("FAIL: merged image does not start with the ESP image magic 0xE9")
    if img[0x10000 : 0x10000 + 16] != (build / "firmware.bin").read_bytes()[:16]:
        sys.exit("FAIL: firmware.bin is not at 0x10000 in the merged image")

    # Cross-check against the factory image pioarduino emits with the same layout.
    factory = build / "firmware.factory.bin"
    if factory.is_file():
        fb = factory.read_bytes()
        if len(fb) == len(img):
            if fb != img:
                sys.exit(
                    "FAIL: merged image differs from firmware.factory.bin "
                    "(same size, different bytes - offset/flash-arg drift?)"
                )
        else:
            # Different padding is expected across esptool versions; the
            # cross-check can't run, so say so rather than silently skipping.
            n = min(len(fb), len(img))
            prefix_ok = fb[:n] == img[:n]
            print(
                f"NOTE: firmware.factory.bin size {len(fb)} != merged {len(img)}; "
                f"cross-check skipped (common {n}-byte prefix "
                f"{'matches' if prefix_ok else 'DIFFERS'})"
            )
            if not prefix_ok:
                sys.exit("FAIL: merged image prefix differs from firmware.factory.bin")

    path = f"{args.base_url.rstrip('/')}/{IMAGE_NAME}" if args.base_url else IMAGE_NAME
    manifest = {
        "name": "Nimbus",
        "version": args.version,
        "new_install_prompt_erase": True,
        "builds": [
            {
                "chipFamily": "ESP32-S3",
                "parts": [{"path": path, "offset": 0}],
            }
        ],
    }
    (out_dir / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")

    # Integrity sidecar. The browser flash path is unsigned (ESP Web Tools writes
    # whatever the manifest points at - same trust model as any pre-secure-boot
    # first flash), so publish the image's SHA-256 for a manual check: a cautious
    # user can compare it against the /flash page before flashing.
    digest = hashlib.sha256(img).hexdigest()
    (out_dir / (IMAGE_NAME + ".sha256")).write_text(f"{digest}  {IMAGE_NAME}\n")

    print(f"OK: {image} ({len(img)} bytes, firmware {fw_size} bytes)")
    print(f"OK: {out_dir / 'manifest.json'} -> {path}")
    print(f"OK: sha256 {digest}")


if __name__ == "__main__":
    main()
