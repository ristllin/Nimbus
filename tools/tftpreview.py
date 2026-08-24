#!/usr/bin/env python3
"""tftpreview.py - RGB565 framebuffer <-> PNG tooling for the TFT golden suite.

The colour counterpart of the removed e-ink golden tool. The native TFT tests
(test/test_tft_render) render every screen into the portable 240x320 RGB565
framebuffer (nimbus/tft_render/fb565.h: big-endian 5-6-5, 2 bytes per pixel,
row-major) and byte-compare against blessed buffers in test/golden_tft/*.bin.
Each screen also emits its tap-target list as <case>.regions.json.

This tool turns those buffers into reviewable PNGs:

    python3 tools/tftpreview.py render  test/golden_tft/status_home.bin out.png
    python3 tools/tftpreview.py diff    a.bin b.bin d.png
    python3 tools/tftpreview.py contact test/golden_tft sheet.png
    python3 tools/tftpreview.py regions fb.bin fb.regions.json overlay.png

`render` writes a 2x-upscaled true-colour PNG. `diff` writes an RGB PNG with
differing pixels in magenta and exits 1 on any mismatch. `contact` tiles every
blessed screen into one labelled sheet so the WHOLE UI can be reviewed at a
glance after a layout change. `regions` overlays the tap rectangles and FAILS
on targets below the 44px minimum or on overlaps - the two bugs a pixel diff
cannot see.

Python 3 stdlib only - the PNG is hand-rolled with zlib + struct, no Pillow
(same constraint as the former e-ink golden tool).
"""

from __future__ import annotations

import json
import os
import struct
import sys
import zlib

W, H = 320, 240
BPP = 2
FB_BYTES = W * H * BPP  # 153600
SCALE = 2
MIN_TAP = 44  # minimum tap-target edge, in panel px (see plan / a11y rule)

USAGE = """usage:
  tftpreview.py render  <in.bin> <out.png>                RGB565 -> PNG (2x)
  tftpreview.py diff    <a.bin> <b.bin> <out.png>         magenta-diff; exit 1 on mismatch
  tftpreview.py contact <dir> <out.png>                   all *.bin tiled into one sheet
  tftpreview.py regions <fb.bin> <regions.json> <out.png> tap-target overlay; exit 1 if invalid
"""


# ---- framebuffer ------------------------------------------------------------


def load_fb(path: str) -> bytes:
    with open(path, "rb") as f:
        data = f.read()
    if len(data) != FB_BYTES:
        sys.exit(f"tftpreview.py: {path}: expected {FB_BYTES} bytes, got {len(data)}")
    return data


def rgb(fb: bytes, x: int, y: int) -> tuple[int, int, int]:
    """RGB565 big-endian -> 8-bit RGB, with the low bits replicated so that
    full-scale channels reach 255 (a plain shift would cap white at 0xF8)."""
    i = (y * W + x) * BPP
    v = (fb[i] << 8) | fb[i + 1]
    r5, g6, b5 = (v >> 11) & 0x1F, (v >> 5) & 0x3F, v & 0x1F
    return (r5 << 3) | (r5 >> 2), (g6 << 2) | (g6 >> 4), (b5 << 3) | (b5 >> 2)


# ---- PNG (hand-rolled, stdlib only) -----------------------------------------


def png_chunk(tag: bytes, data: bytes) -> bytes:
    body = tag + data
    return struct.pack(">I", len(data)) + body + struct.pack(">I", zlib.crc32(body) & 0xFFFFFFFF)


def write_png(path: str, width: int, height: int, rows: list[bytes]) -> None:
    """rows: raw RGB8 scanlines without filter bytes."""
    raw = b"".join(b"\x00" + r for r in rows)  # filter 0 (None) per scanline
    with open(path, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n")
        f.write(png_chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0)))
        f.write(png_chunk(b"IDAT", zlib.compress(raw, 9)))
        f.write(png_chunk(b"IEND", b""))


def scale_rows(px: list[list[tuple[int, int, int]]], scale: int) -> list[bytes]:
    """Nearest-neighbour upscale a pixel grid into PNG scanlines."""
    out: list[bytes] = []
    for row in px:
        line = b"".join(bytes(c) * scale for c in row)
        for _ in range(scale):
            out.append(line)
    return out


def fb_pixels(fb: bytes) -> list[list[tuple[int, int, int]]]:
    return [[rgb(fb, x, y) for x in range(W)] for y in range(H)]


# ---- 5x7 label font (contact sheet only) ------------------------------------
# Minimal glyph set: enough to caption filenames. Unknown chars render blank.
_GLYPHS = {
    "A": ("01110", "10001", "10001", "11111", "10001", "10001", "10001"),
    "B": ("11110", "10001", "11110", "10001", "10001", "10001", "11110"),
    "C": ("01110", "10001", "10000", "10000", "10000", "10001", "01110"),
    "D": ("11110", "10001", "10001", "10001", "10001", "10001", "11110"),
    "E": ("11111", "10000", "11110", "10000", "10000", "10000", "11111"),
    "F": ("11111", "10000", "11110", "10000", "10000", "10000", "10000"),
    "G": ("01110", "10001", "10000", "10111", "10001", "10001", "01111"),
    "H": ("10001", "10001", "11111", "10001", "10001", "10001", "10001"),
    "I": ("11111", "00100", "00100", "00100", "00100", "00100", "11111"),
    "J": ("00111", "00010", "00010", "00010", "00010", "10010", "01100"),
    "K": ("10001", "10010", "11100", "10010", "10001", "10001", "10001"),
    "L": ("10000", "10000", "10000", "10000", "10000", "10000", "11111"),
    "M": ("10001", "11011", "10101", "10001", "10001", "10001", "10001"),
    "N": ("10001", "11001", "10101", "10011", "10001", "10001", "10001"),
    "O": ("01110", "10001", "10001", "10001", "10001", "10001", "01110"),
    "P": ("11110", "10001", "10001", "11110", "10000", "10000", "10000"),
    "Q": ("01110", "10001", "10001", "10001", "10101", "10010", "01101"),
    "R": ("11110", "10001", "10001", "11110", "10100", "10010", "10001"),
    "S": ("01111", "10000", "10000", "01110", "00001", "00001", "11110"),
    "T": ("11111", "00100", "00100", "00100", "00100", "00100", "00100"),
    "U": ("10001", "10001", "10001", "10001", "10001", "10001", "01110"),
    "V": ("10001", "10001", "10001", "10001", "10001", "01010", "00100"),
    "W": ("10001", "10001", "10001", "10101", "10101", "11011", "10001"),
    "X": ("10001", "10001", "01010", "00100", "01010", "10001", "10001"),
    "Y": ("10001", "10001", "01010", "00100", "00100", "00100", "00100"),
    "Z": ("11111", "00001", "00010", "00100", "01000", "10000", "11111"),
    "0": ("01110", "10001", "10011", "10101", "11001", "10001", "01110"),
    "1": ("00100", "01100", "00100", "00100", "00100", "00100", "01110"),
    "2": ("01110", "10001", "00001", "00110", "01000", "10000", "11111"),
    "3": ("11111", "00010", "00100", "00010", "00001", "10001", "01110"),
    "4": ("00010", "00110", "01010", "10010", "11111", "00010", "00010"),
    "5": ("11111", "10000", "11110", "00001", "00001", "10001", "01110"),
    "6": ("00110", "01000", "10000", "11110", "10001", "10001", "01110"),
    "7": ("11111", "00001", "00010", "00100", "01000", "01000", "01000"),
    "8": ("01110", "10001", "10001", "01110", "10001", "10001", "01110"),
    "9": ("01110", "10001", "10001", "01111", "00001", "00010", "01100"),
    "_": ("00000", "00000", "00000", "00000", "00000", "00000", "11111"),
    "-": ("00000", "00000", "00000", "11111", "00000", "00000", "00000"),
    ".": ("00000", "00000", "00000", "00000", "00000", "01100", "01100"),
    " ": ("00000",) * 7,
}


def draw_text(px: list[list[tuple[int, int, int]]], x0: int, y0: int, s: str, color: tuple[int, int, int]) -> None:
    for ci, ch in enumerate(s.upper()):
        g = _GLYPHS.get(ch)
        if not g:
            continue
        for gy, bits in enumerate(g):
            for gx, bit in enumerate(bits):
                if bit != "1":
                    continue
                x, y = x0 + ci * 6 + gx, y0 + gy
                if 0 <= y < len(px) and 0 <= x < len(px[0]):
                    px[y][x] = color


# ---- commands ---------------------------------------------------------------


def cmd_render(src: str, dst: str) -> None:
    rows = scale_rows(fb_pixels(load_fb(src)), SCALE)
    write_png(dst, W * SCALE, H * SCALE, rows)
    print(f"wrote {dst} ({W * SCALE}x{H * SCALE} RGB)")


def cmd_diff(a_src: str, b_src: str, dst: str) -> None:
    a, b = load_fb(a_src), load_fb(b_src)
    ndiff = 0
    px: list[list[tuple[int, int, int]]] = []
    for y in range(H):
        row: list[tuple[int, int, int]] = []
        for x in range(W):
            ca, cb = rgb(a, x, y), rgb(b, x, y)
            if ca != cb:
                ndiff += 1
                row.append((255, 0, 255))  # magenta: unmissable on this palette
            else:
                # Dim the matching pixels so differences pop.
                row.append(tuple(v // 3 for v in ca))  # type: ignore[arg-type]
        px.append(row)
    write_png(dst, W * SCALE, H * SCALE, scale_rows(px, SCALE))
    if ndiff:
        print(f"DIFF: {ndiff} pixels differ -> {dst}")
        sys.exit(1)
    print(f"identical -> {dst}")


def cmd_contact(src_dir: str, dst: str) -> None:
    """Tile every *.bin in src_dir into one labelled sheet (thumbnails, 1x)."""
    names = sorted(f for f in os.listdir(src_dir) if f.endswith(".bin"))
    if not names:
        sys.exit(f"tftpreview.py: no .bin files in {src_dir}")

    thumb_div = 2  # 120x160 thumbnails
    tw, th = W // thumb_div, H // thumb_div
    pad, label_h = 8, 10
    cell_w, cell_h = tw + pad, th + pad + label_h
    cols = min(6, len(names))
    rows_n = (len(names) + cols - 1) // cols
    sheet_w, sheet_h = cols * cell_w + pad, rows_n * cell_h + pad

    bg = (20, 21, 24)  # --bg #141518
    ink = (236, 238, 242)  # --ink #eceef2
    px = [[bg for _ in range(sheet_w)] for _ in range(sheet_h)]

    for i, name in enumerate(names):
        fb = load_fb(os.path.join(src_dir, name))
        cx = pad + (i % cols) * cell_w
        cy = pad + (i // cols) * cell_h
        for y in range(th):
            for x in range(tw):
                px[cy + y][cx + x] = rgb(fb, x * thumb_div, y * thumb_div)
        draw_text(px, cx, cy + th + 2, name[:-4][:19], ink)

    write_png(dst, sheet_w, sheet_h, [b"".join(bytes(c) for c in r) for r in px])
    print(f"wrote {dst} ({sheet_w}x{sheet_h}, {len(names)} screens)")


def _rects(regions: object, path: str) -> list[dict]:
    """Accept either a bare list or {"regions": [...]}."""
    if isinstance(regions, dict):
        regions = regions.get("regions", [])
    if not isinstance(regions, list):
        sys.exit(f"tftpreview.py: {path}: expected a list of regions")
    return regions


def cmd_regions(fb_src: str, json_src: str, dst: str) -> None:
    fb = load_fb(fb_src)
    with open(json_src) as f:
        rects = _rects(json.load(f), json_src)

    px = fb_pixels(fb)
    accent = (90, 214, 196)  # --teal
    bad = (240, 104, 122)  # --crit
    problems: list[str] = []

    boxes = []
    for r in rects:
        try:
            x, y, w, h = int(r["x"]), int(r["y"]), int(r["w"]), int(r["h"])
        except (KeyError, TypeError, ValueError):
            sys.exit(f"tftpreview.py: {json_src}: region missing x/y/w/h: {r!r}")
        label = str(r.get("action", r.get("id", "?")))
        boxes.append((x, y, w, h, label))

        if w < MIN_TAP or h < MIN_TAP:
            problems.append(f"target '{label}' is {w}x{h}, below the {MIN_TAP}px minimum")
        if x < 0 or y < 0 or x + w > W or y + h > H:
            problems.append(f"target '{label}' ({x},{y},{w},{h}) falls outside {W}x{H}")

    for i, (x1, y1, w1, h1, l1) in enumerate(boxes):
        for x2, y2, w2, h2, l2 in boxes[i + 1 :]:
            if x1 < x2 + w2 and x2 < x1 + w1 and y1 < y2 + h2 and y2 < y1 + h1:
                problems.append(f"targets '{l1}' and '{l2}' overlap")

    for x, y, w, h, label in boxes:
        color = bad if (w < MIN_TAP or h < MIN_TAP) else accent
        for t in range(2):  # 2px border so it survives the thumbnail
            for xx in range(max(0, x), min(W, x + w)):
                for yy in (y + t, y + h - 1 - t):
                    if 0 <= yy < H:
                        px[yy][xx] = color
            for yy in range(max(0, y), min(H, y + h)):
                for xx in (x + t, x + w - 1 - t):
                    if 0 <= xx < W:
                        px[yy][xx] = color
        draw_text(px, x + 3, y + 3, label[:14], color)

    write_png(dst, W * SCALE, H * SCALE, scale_rows(px, SCALE))
    print(f"wrote {dst} ({len(boxes)} tap targets)")
    if problems:
        for p in problems:
            print(f"REGION: {p}")
        sys.exit(1)


def main() -> None:
    args = sys.argv[1:]
    if len(args) == 3 and args[0] == "render":
        cmd_render(args[1], args[2])
    elif len(args) == 4 and args[0] == "diff":
        cmd_diff(args[1], args[2], args[3])
    elif len(args) == 3 and args[0] == "contact":
        cmd_contact(args[1], args[2])
    elif len(args) == 4 and args[0] == "regions":
        cmd_regions(args[1], args[2], args[3])
    else:
        sys.stderr.write(USAGE)
        sys.exit(2)


if __name__ == "__main__":
    main()
