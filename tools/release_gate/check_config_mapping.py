#!/usr/bin/env python3
"""RELEASE GATE - hardware-config <-> image mapping (CUM-388 / CUM-392).

A wrong-variant image is this project's most expensive recurring failure: a
Solide image on a Freenove boots serial-healthy with black glass. The board
variant is compile-time, never runtime-detected, so the ONLY safeguards are the
two places a user is handed an image - the web installer (fresh flash) and the
typed OTA manifest (updates) - and they must be impossible to get wrong.

This gate owns the CANONICAL config table (CONFIG_TABLE below) and asserts that
EVERY source that maps a supported hardware config to an image agrees with it:

  * the web installer variant list          website/src/pages/flash.jsx
  * the release build + naming + manifest    .github/workflows/release.yml
  * the web-flash image/manifest builder     tools/release/make_webflash_manifest.py
  * the typed device-type slugs + families   lib/core/include/nimbus/ota/ota_logic.h
                                             lib/core/src/ota_logic.cpp
  * the PlatformIO envs (board + panel)      platformio.ini

For each config it checks: exactly one image (no missing / duplicate), the
correct PlatformIO env, the correct chip family, the correct merged-image
offsets, a unique device-TYPE key, and that the device-side type<->board family
partition refuses a manifest entry of another type. A NEW config wired into one
source but absent from the table (or wired inconsistently) FAILS the gate - that
is the class rule, not a point check on today's four configs.

This is a static cross-file consistency gate: no toolchain, no build, no network.
It complements the compile-time static_assert and the runtime type check in
src/sys/ota_update.cpp (both keyed on the SAME slugs) by catching drift at the
release step, before an image is ever published.

Usage:
    python3 tools/release_gate/check_config_mapping.py     # exit 1 blocks release
    python3 -m pytest tools/release_gate                   # tests the gate logic
"""

from __future__ import annotations

import os
import re
import sys
from dataclasses import dataclass

# --- the canonical table ----------------------------------------------------


@dataclass(frozen=True)
class Config:
    type_slug: str  # NVS otaType + typed-manifest key (frozen); the web-flash subdir
    env: str  # PlatformIO env that builds this image
    board: str  # SOLIDE_BOARD compile define (pinout family)
    ota_variant: str  # NIMBUS_OTA_VARIANT compile tag (dev/HIL fallback only)
    panel: "tuple[int, int] | None"  # (W,H) if pinned via -DNIMBUS_PANEL_W/H, else driver default
    chip: str  # ESP Web Tools chipFamily / esptool --chip
    ota_image: str  # the OTA release asset filename
    is_freenove: bool  # board family the device-side check uses


# The four supported configs (CUM-388 says four). Adding a fifth board/panel means
# adding a row HERE and wiring every source below; the gate fails until they agree.
CONFIG_TABLE: "tuple[Config, ...]" = (
    Config("nimbus-tft", "esp32s3", "solide_s3", "esp32s3", None, "ESP32-S3", "firmware-nimbus-tft.bin", False),
    Config("freenove-28", "esp32s3-cyd", "freenove_s3", "cyd", None, "ESP32-S3", "firmware-freenove.bin", True),
    Config("freenove-35", "esp32s3-cyd-35", "freenove_s3", "cyd-35", (480, 320), "ESP32-S3", "firmware-freenove-35.bin", True),
    Config("freenove-40", "esp32s3-cyd-40", "freenove_s3", "cyd-40", (480, 480), "ESP32-S3", "firmware-freenove-40.bin", True),
)

# The merged web-flash image layout (default_16MB.csv), asserted against the
# builder's own constants. Every config uses this identical layout.
EXPECTED_PARTS = ((0x0, "bootloader.bin"), (0x8000, "partitions.bin"), (0xE000, "boot_app0.bin"), (0x10000, "firmware.bin"))
EXPECTED_NVS_OFFSET = 0x9000
EXPECTED_NVS_SIZE = 0x5000

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

# Repo-relative paths of every mapping source (so a moved file fails loudly).
FLASH_JSX_REL = "website/src/pages/flash.jsx"
RELEASE_YML_REL = ".github/workflows/release.yml"
WEBFLASH_REL = "tools/release/make_webflash_manifest.py"
OTA_HDR_REL = "lib/core/include/nimbus/ota/ota_logic.h"
OTA_CPP_REL = "lib/core/src/ota_logic.cpp"
PIO_INI_REL = "platformio.ini"


# --- parsers (pure: take file text, return what the file declares) -----------


def parse_flash_variants(jsx: str) -> "list[str]":
    """The installer's VARIANTS slugs (website/src/pages/flash.jsx)."""
    block = re.search(r"const VARIANTS\s*=\s*\[(.*?)\];", jsx, re.DOTALL)
    if not block:
        return []
    return re.findall(r"slug:\s*'([^']+)'", block.group(1))


def parse_release_yml(yml: str) -> dict:
    """Extract the release build's config->image and config->env wiring.

    Returns dict with:
      built_envs   - envs passed to `pio run -e`
      image_of_env - {env: image} from the `cp .pio/build/<env>/firmware.bin <image>` lines
      manifest_pairs - {type: image} from the make_manifest `type=image` args
      webflash_dir_of_type - {type: env} from the webflash `case "$v"` arms
      webflash_lists - list of the `for v in ...` variant lists (must all agree)
    """
    built = re.search(r"pio run((?:\s+-e\s+[\w-]+)+)", yml)
    built_envs = re.findall(r"-e\s+([\w-]+)", built.group(1)) if built else []

    image_of_env = {}
    for env, image in re.findall(r"cp\s+\.pio/build/([\w-]+)/firmware\.bin\s+([\w.\-]+\.bin)", yml):
        image_of_env[env] = image

    manifest_pairs = dict(re.findall(r"^\s*([a-z0-9-]+)=([\w.\-]+\.bin)\s*\\?\s*$", yml, re.MULTILINE))

    webflash_dir_of_type = {}
    for slug, env in re.findall(r"([\w-]+)\)\s*dir=\.pio/build/([\w-]+)\s*;;", yml):
        webflash_dir_of_type[slug] = env

    webflash_lists = [m.split() for m in re.findall(r"for v in ([\w\s-]+?);\s*do", yml)]

    return {
        "built_envs": built_envs,
        "image_of_env": image_of_env,
        "manifest_pairs": manifest_pairs,
        "webflash_dir_of_type": webflash_dir_of_type,
        "webflash_lists": webflash_lists,
    }


def parse_webflash_builder(py: str) -> dict:
    """KNOWN_VARIANTS, PARTS, NVS offset/size, chipFamily, and the otaType seed."""
    kv = re.search(r"KNOWN_VARIANTS\s*=\s*\(([^)]*)\)", py)
    known = re.findall(r'"([^"]+)"', kv.group(1)) if kv else []

    parts = []
    pblock = re.search(r"PARTS\s*=\s*\[(.*?)\]", py, re.DOTALL)
    if pblock:
        for off, name in re.findall(r"\(\s*(0x[0-9A-Fa-f]+)\s*,\s*\"([^\"]+)\"\s*\)", pblock.group(1)):
            parts.append((int(off, 16), name))

    def _hex(name: str) -> "int | None":
        m = re.search(rf"{name}\s*=\s*(0x[0-9A-Fa-f]+)", py)
        return int(m.group(1), 16) if m else None

    chips = re.findall(r'"chipFamily":\s*"([^"]+)"', py)
    seeds_ota_type = bool(re.search(r'"otaType"[^\n]*variant|\(\s*"otaType"\s*,[^\n]*variant', py))
    return {
        "known": known,
        "parts": tuple(parts),
        "nvs_offset": _hex("NVS_OFFSET"),
        "nvs_size": _hex("NVS_SIZE"),
        "chips": chips,
        "seeds_ota_type": seeds_ota_type,
    }


def parse_ota_header_slugs(hdr: str) -> "list[str]":
    """The frozen kType* device-type slug string literals."""
    return re.findall(r'kType\w+\s*=\s*"([^"]+)"', hdr)


def parse_type_allowed_families(cpp: str) -> dict:
    """From ota_logic.cpp typeAllowedForBoard: {slug: 'freenove'|'solide'}.

    The freenove branch (inside `if (isFreenove)`) returns the freenove-* slugs;
    the trailing `return strcmp(type, kTypeNimbusTft)...` is the solide branch.
    Maps each referenced kType* constant to its family so the gate can confirm
    the device-side check partitions types by board exactly as the table says.
    """
    fn = re.search(r"typeAllowedForBoard\([^)]*\)\s*\{(.*?)\n\}", cpp, re.DOTALL)
    body = fn.group(1) if fn else cpp
    # The freenove branch is the `if (isFreenove) return <...>;` statement; every other
    # strcmp in the function belongs to the solide (else) branch.
    fbranch = re.search(r"if\s*\(isFreenove\)\s*return(.*?);", body, re.DOTALL)
    freenove_consts = set(re.findall(r"strcmp\(type,\s*(kType\w+)\)", fbranch.group(1))) if fbranch else set()
    fam = {}
    for m in re.finditer(r"strcmp\(type,\s*(kType\w+)\)", body):
        const = m.group(1)
        fam[const] = "freenove" if const in freenove_consts else "solide"
    return fam


def parse_pio_envs(ini: str) -> dict:
    """{env: {board, ota_variant, panel}} from platformio.ini, honoring build_unflags.

    An env inherits `-DSOLIDE_BOARD=solide_s3` from the [s3] base; a Freenove env
    build_unflags that and re-adds freenove_s3. Resolve the effective values.
    """
    base = re.search(r"\n\[s3\]\n(.*?)(?=\n\[)", "\n" + ini, re.DOTALL)
    base_board = None
    if base:
        m = re.search(r"-DSOLIDE_BOARD=(\w+)", base.group(1))
        base_board = m.group(1) if m else None

    # Strip ';' comment lines: platformio.ini documents each env's SIBLINGS in a comment
    # block that mentions their -D flags, and those must not be read as this env's flags.
    def _decomment(text: str) -> str:
        return "\n".join(ln for ln in text.splitlines() if not ln.lstrip().startswith(";"))

    envs = {}
    for m in re.finditer(r"\n\[env:([\w-]+)\]\n(.*?)(?=\n\[|\Z)", "\n" + ini, re.DOTALL):
        name, body = m.group(1), _decomment(m.group(2))
        # Effective SOLIDE_BOARD: a -DSOLIDE_BOARD= in this env's build_flags wins (the last
        # one, matching the compiler); else the [s3] base value - UNLESS the env build_unflags
        # the base board without re-adding one, which is a bug we surface as board=None.
        unflag_line = re.search(r"build_unflags\s*=[^\n]*", body)
        unflagged_board = bool(unflag_line and "-DSOLIDE_BOARD=" in unflag_line.group(0))
        flags = re.findall(r"-DSOLIDE_BOARD=(\w+)", body)
        if flags:
            board = flags[-1]
        elif unflagged_board:
            board = None
        else:
            board = base_board
        ov = re.search(r'-DNIMBUS_OTA_VARIANT=\\?"([^"\\]+)\\?"', body)
        pw = re.search(r"-DNIMBUS_PANEL_W=(\d+)", body)
        ph = re.search(r"-DNIMBUS_PANEL_H=(\d+)", body)
        panel = (int(pw.group(1)), int(ph.group(1))) if pw and ph else None
        envs[name] = {"board": board, "ota_variant": ov.group(1) if ov else None, "panel": panel}
    return envs


# --- the judgment (pure: takes all parsed inputs, returns ok + messages) -----


def judge(
    table: "tuple[Config, ...]",
    flash_variants: "list[str]",
    rel: dict,
    web: dict,
    hdr_slugs: "list[str]",
    fam: dict,
    envs: dict,
) -> "tuple[bool, list[str]]":
    errs: "list[str]" = []
    types = [c.type_slug for c in table]

    # 1. unique device-TYPE key.
    dupes = {t for t in types if types.count(t) > 1}
    if dupes:
        errs.append(f"duplicate device-type key(s): {sorted(dupes)}")

    # 2. web-flash image layout: offsets + NVS window + it seeds otaType.
    if web["parts"] != EXPECTED_PARTS:
        errs.append(f"web-flash PARTS {web['parts']} != expected {EXPECTED_PARTS} (wrong flash offsets)")
    if web["nvs_offset"] != EXPECTED_NVS_OFFSET:
        errs.append(f"web-flash NVS_OFFSET {web['nvs_offset']:#x} != {EXPECTED_NVS_OFFSET:#x}")
    if web["nvs_size"] != EXPECTED_NVS_SIZE:
        errs.append(f"web-flash NVS_SIZE {web['nvs_size']} != {EXPECTED_NVS_SIZE:#x}")
    if not web["seeds_ota_type"]:
        errs.append("web-flash builder no longer seeds otaType into NVS (board would boot untyped)")

    # 3. chip family: every config's chip is the one the builder writes into every manifest.
    for chip in set(web["chips"]):
        if chip not in {c.chip for c in table}:
            errs.append(f"web-flash manifest chipFamily {chip!r} is not a chip in the config table")
    for c in table:
        if web["chips"] and c.chip not in web["chips"]:
            errs.append(f"{c.type_slug}: table chip {c.chip!r} never appears in the web-flash manifest chipFamily")

    # 4. the four sources' type/slug sets must equal the table exactly (no extra, none missing).
    def _cmp(label: str, got: "list[str]") -> None:
        missing = [t for t in types if t not in got]
        extra = [g for g in got if g not in types]
        if missing:
            errs.append(f"{label}: missing config(s) {missing} (a supported config has no mapping here)")
        if extra:
            errs.append(f"{label}: unmapped extra config(s) {extra} (wired here but not in the canonical table)")

    _cmp("flash.jsx VARIANTS", flash_variants)
    _cmp("make_webflash_manifest KNOWN_VARIANTS", web["known"])
    _cmp("ota_logic.h kType slugs", hdr_slugs)
    _cmp("release.yml manifest pairs", list(rel["manifest_pairs"].keys()))
    _cmp("release.yml webflash case arms", list(rel["webflash_dir_of_type"].keys()))
    for lst in rel["webflash_lists"]:
        _cmp("release.yml `for v in` list", lst)

    # 5. per-config: exactly one image, correct env, correct board+panel, device-side family.
    images = [c.ota_image for c in table]
    img_dupes = {i for i in images if images.count(i) > 1}
    if img_dupes:
        errs.append(f"duplicate OTA image name(s) across configs: {sorted(img_dupes)}")

    for c in table:
        # 5a. type -> image (make_manifest pair)
        pair_img = rel["manifest_pairs"].get(c.type_slug)
        if pair_img != c.ota_image:
            errs.append(f"{c.type_slug}: release manifest pair image {pair_img!r} != table {c.ota_image!r}")
        # 5b. type -> env via the webflash case arm
        wf_env = rel["webflash_dir_of_type"].get(c.type_slug)
        if wf_env != c.env:
            errs.append(f"{c.type_slug}: webflash case build dir env {wf_env!r} != table env {c.env!r}")
        # 5c. image -> env via the cp line, and it must be built
        cp_image = rel["image_of_env"].get(c.env)
        if cp_image != c.ota_image:
            errs.append(f"{c.type_slug}: release `cp` for env {c.env} produces {cp_image!r} != table image {c.ota_image!r}")
        if c.env not in rel["built_envs"]:
            errs.append(f"{c.type_slug}: env {c.env} is not built by the release `pio run -e` step")
        # 5d. env board + panel from platformio.ini
        e = envs.get(c.env)
        if e is None:
            errs.append(f"{c.type_slug}: env {c.env} not found in platformio.ini")
        else:
            if e["board"] != c.board:
                errs.append(f"{c.type_slug}: env {c.env} SOLIDE_BOARD {e['board']!r} != table {c.board!r} (wrong pinout / fell through to default)")
            if e["panel"] != c.panel:
                errs.append(f"{c.type_slug}: env {c.env} panel {e['panel']} != table {c.panel} (renderer geometry mismatch)")
            if e["ota_variant"] != c.ota_variant:
                errs.append(f"{c.type_slug}: env {c.env} NIMBUS_OTA_VARIANT {e['ota_variant']!r} != table {c.ota_variant!r}")

    # 6. device-side OTA refuses a manifest entry of ANOTHER type: the family partition
    #    (typeAllowedForBoard) must place each slug in exactly its board's family, and the
    #    two families must be disjoint. A slug a board does not own is refused at runtime.
    return _judge_families(table, fam, errs)


def _judge_families(table, fam, errs):
    # Resolve kType* constant -> family (from cpp) into slug -> family using the known
    # constant->slug identities (frozen in ota_logic.h). Kept explicit so a renamed
    # constant is caught rather than silently skipped.
    const_to_slug = {
        "kTypeNimbusTft": "nimbus-tft",
        "kTypeFreenove28": "freenove-28",
        "kTypeFreenove35": "freenove-35",
        "kTypeFreenove40": "freenove-40",
    }
    slug_family = {}
    for const, family in fam.items():
        slug = const_to_slug.get(const)
        if slug is None:
            errs.append(f"typeAllowedForBoard references unknown constant {const} (rename? update the gate + table)")
            continue
        slug_family[slug] = family
    for c in table:
        want = "freenove" if c.is_freenove else "solide"
        got = slug_family.get(c.type_slug)
        if got is None:
            errs.append(f"{c.type_slug}: not accepted by typeAllowedForBoard for any board (device would refuse its own image)")
        elif got != want:
            errs.append(f"{c.type_slug}: device-side family {got!r} != table family {want!r} (wrong board would accept it)")
    return (not errs, errs)


# --- driver ------------------------------------------------------------------


def _read(rel_path: str) -> str:
    with open(os.path.join(ROOT, rel_path), encoding="utf-8") as fh:
        return fh.read()


def run(table: "tuple[Config, ...]" = CONFIG_TABLE) -> "tuple[bool, list[str]]":
    flash_variants = parse_flash_variants(_read(FLASH_JSX_REL))
    rel = parse_release_yml(_read(RELEASE_YML_REL))
    web = parse_webflash_builder(_read(WEBFLASH_REL))
    hdr_slugs = parse_ota_header_slugs(_read(OTA_HDR_REL))
    fam = parse_type_allowed_families(_read(OTA_CPP_REL))
    envs = parse_pio_envs(_read(PIO_INI_REL))
    return judge(table, flash_variants, rel, web, hdr_slugs, fam, envs)


def main(argv: "list[str] | None" = None) -> int:
    print("RELEASE GATE - hardware-config <-> image mapping (CUM-388)")
    ok, errs = run()
    for e in errs:
        print(f"  FAIL: {e}")
    if ok:
        print(f"  PASS: all {len(CONFIG_TABLE)} configs map consistently across every source.")
        return 0
    print(f"\nMAPPING GATE FAILED: {len(errs)} inconsistency(ies) - a config could receive the wrong image.")
    return 1


if __name__ == "__main__":
    sys.exit(main())
