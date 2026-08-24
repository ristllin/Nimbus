#!/usr/bin/env python3
"""make_webflash_manifest - build the browser-flasher artifacts for a release.

Produces, PER BOARD VARIANT, into <out-dir>/<variant>/:

  nimbus-webflash.bin   one merged, complete flash image (bootloader + partition
                        table + a seeded NVS + otadata/boot_app0 + app), at offset 0
  nimbus-webflash.bin.sha256
  manifest.json         the ESP Web Tools manifest pointing at that image
  nvs-seed-<variant>.{csv,bin}   the generated NVS seed (kept for audit)

The merged image is produced with `esptool merge-bin` from a PlatformIO build
(esp32s3 for nimbus-tft, esp32s3-cyd for the freenove sizes). The offsets are
the standard Arduino-ESP32 layout for this board, verified against
default_16MB.csv (nvs @ 0x9000, otadata @ 0xe000, app0 @ 0x10000). The NVS at
0x9000 is SEEDED (scrModel/tftFlip/mode/otaType) so the board comes up on the
right panel and knows its update type - it never boots to a blank/white screen:

    0x0      bootloader.bin      (ESP32-S3 boots the 2nd-stage loader at 0x0)
    0x8000   partitions.bin
    0x9000   nvs-seed-<variant>.bin   (fitted panel + OTA type)
    0xe000   boot_app0.bin       (otadata: boot from app0 - framework package)
    0x10000  firmware.bin

The NON-NVS regions are byte-compared against the build's firmware.factory.bin
when present (the seeded NVS window is expected to differ).

Needs `esp-idf-nvs-partition-gen` (pip) for the NVS seed. Usage:
  python3 tools/release/make_webflash_manifest.py \
      --variant nimbus-tft --build-dir .pio/build/esp32s3 \
      --version v4.3.0 --out-dir webflash \
      [--base-url https://raw.githubusercontent.com/.../latest/nimbus-tft]

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

# The NVS partition (default_16MB.csv: nvs @ 0x9000, size 0x5000) is seeded into
# the merged image so a freshly web-flashed board comes up on the RIGHT panel and
# already knows its typed-OTA slug - it never boots to a blank/white screen.
NVS_OFFSET = 0x9000
NVS_SIZE = 0x5000

# Board variant -> the settings seeded into its NVS. All shipping boards are TFT;
# the Nimbus board mounts its panel 180-degrees (tftFlip), the Freenove owns its
# own orientation. nimbus_mode defaults to Orchestrator (1); the setup wizard can
# change it. Ints are i32 to match Arduino Preferences putInt / getInt.
KNOWN_VARIANTS = ("nimbus-tft", "freenove-28", "freenove-35", "freenove-40")


def seed_rows(variant: str) -> list[tuple[str, str, str]]:
    """(key, encoding, value) rows for the 'solide' NVS namespace."""
    rows = [("scrModel", "string", "tft")]
    if variant == "nimbus-tft":
        rows.append(("tftFlip", "i32", "1"))
    rows.append(("nimbus_mode", "i32", "1"))
    rows.append(("otaType", "string", variant))
    return rows


def find_nvs_gen() -> list[str]:
    """argv prefix that runs the esp-idf NVS partition generator. Prefer the pip
    module (portable, `pip install esp-idf-nvs-partition-gen`), then the copy that
    ships in the PlatformIO espidf framework package."""
    try:
        import esp_idf_nvs_partition_gen  # noqa: F401

        return [sys.executable, "-m", "esp_idf_nvs_partition_gen"]
    except ImportError:
        pass
    home = Path(os.path.expanduser("~/.platformio/packages"))
    cands = glob.glob(
        str(
            home / "framework-espidf*" / "components" / "nvs_flash" / "nvs_partition_generator" / "nvs_partition_gen.py"
        )
    )
    if cands:
        py = Path(os.path.expanduser("~/.platformio/penv/bin/python"))
        return [str(py) if py.exists() else sys.executable, cands[0]]
    sys.exit("FAIL: NVS generator not found (pip install esp-idf-nvs-partition-gen)")


def build_nvs_seed(variant: str, out_dir: Path) -> Path:
    """Generate the NVS seed partition binary for `variant`."""
    csv = out_dir / f"nvs-seed-{variant}.csv"
    lines = ["key,type,encoding,value", "solide,namespace,,"]
    for key, enc, val in seed_rows(variant):
        lines.append(f"{key},data,{enc},{val}")
    csv.write_text("\n".join(lines) + "\n")
    seed = out_dir / f"nvs-seed-{variant}.bin"
    argv = find_nvs_gen() + ["generate", str(csv), str(seed), hex(NVS_SIZE)]
    res = subprocess.run(argv, capture_output=True, text=True)
    if res.returncode != 0 or not seed.is_file():
        sys.exit(f"FAIL: NVS seed generation failed:\n$ {' '.join(argv)}\n{res.stdout}{res.stderr}")
    if seed.stat().st_size != NVS_SIZE:
        sys.exit(f"FAIL: NVS seed {seed.stat().st_size} B != partition size {NVS_SIZE} B")
    return seed


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
    ap.add_argument("--out-dir", required=True, help="directory for the outputs (a <variant>/ subdir is created)")
    ap.add_argument(
        "--variant",
        required=True,
        choices=KNOWN_VARIANTS,
        help="typed-OTA device slug; sets the NVS seed and the output subdir",
    )
    ap.add_argument(
        "--base-url",
        default="",
        help="absolute URL prefix for the image in manifest.json (no trailing slash); empty = relative path",
    )
    args = ap.parse_args()

    build = Path(args.build_dir)
    # Each variant gets its own subdir so the site fetches latest/<variant>/manifest.json.
    out_dir = Path(args.out_dir) / args.variant
    out_dir.mkdir(parents=True, exist_ok=True)

    seed = build_nvs_seed(args.variant, out_dir)

    parts: list[tuple[int, Path]] = []
    for off, name in PARTS:
        p = find_boot_app0() if name == "boot_app0.bin" else build / name
        if not p.is_file():
            sys.exit(f"FAIL: missing {p} - run the matching `pio run` first")
        parts.append((off, p))
    parts.append((NVS_OFFSET, seed))  # seed the fitted-panel + OTA-type settings
    parts.sort(key=lambda x: x[0])

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

    # The NVS seed must be present, byte-exact, at 0x9000.
    seed_bytes = seed.read_bytes()
    if img[NVS_OFFSET : NVS_OFFSET + NVS_SIZE] != seed_bytes:
        sys.exit("FAIL: NVS seed is not at 0x9000 in the merged image")
    if args.variant.encode() not in img[NVS_OFFSET : NVS_OFFSET + NVS_SIZE]:
        sys.exit(f"FAIL: otaType slug {args.variant!r} not found in the seeded NVS region")

    # Cross-check the NON-NVS regions against the factory image pioarduino emits
    # (the seed intentionally differs from the factory's blank NVS, so skip that
    # window). Confirms the bootloader/partitions/app landed at the right offsets.
    factory = build / "firmware.factory.bin"
    if factory.is_file() and len(factory.read_bytes()) == len(img):
        fb = factory.read_bytes()
        before = fb[:NVS_OFFSET] == img[:NVS_OFFSET]
        after = fb[NVS_OFFSET + NVS_SIZE :] == img[NVS_OFFSET + NVS_SIZE :]
        if not (before and after):
            sys.exit("FAIL: merged image differs from firmware.factory.bin outside the NVS seed window")
    else:
        print("NOTE: firmware.factory.bin absent or a different size; non-NVS cross-check skipped")

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
