#!/usr/bin/env python3
"""golden.py - 1-bit framebuffer <-> PNG tooling for the golden-image suite.

The native golden tests (test/test_golden) render every screen into the
portable 296x128 1-bit framebuffer (nimbus/epd_render/fb.h: 1 = black, rows
packed MSB-first, 37 bytes per row, 4736 bytes total) and byte-compare against
blessed buffers in test/golden/*.bin. This tool turns those buffers into PNGs
for human review and pixel-diffs two buffers when a test goes red:

    python3 tools/golden.py render test/golden/status_idle_empty.bin out.png
    python3 tools/golden.py diff test/golden/x.bin test/golden/out/x.bin d.png

`render` writes a 2x-upscaled grayscale PNG (592x256). `diff` writes an RGB
PNG with differing pixels in red and exits 1 on any mismatch (0 otherwise).

Python 3 stdlib only - the PNG is hand-rolled with zlib + struct, no Pillow.
"""

from __future__ import annotations

import struct
import sys
import zlib

W, H = 296, 128
STRIDE = W // 8  # 37 bytes per row
FB_BYTES = STRIDE * H  # 4736
SCALE = 2

USAGE = """usage:
  golden.py render <in.bin> <out.png>          1-bit buffer -> grayscale PNG (2x)
  golden.py diff <a.bin> <b.bin> <out.png>     red-diff PNG; exit 1 on mismatch
"""


def load_fb(path: str) -> bytes:
    with open(path, "rb") as f:
        data = f.read()
    if len(data) != FB_BYTES:
        sys.exit(f"golden.py: {path}: expected {FB_BYTES} bytes, got {len(data)}")
    return data


def pixel(fb: bytes, x: int, y: int) -> int:
    """1 = black, 0 = white (MSB-first rows, matching fb.h)."""
    return (fb[y * STRIDE + (x >> 3)] >> (7 - (x & 7))) & 1


def png_chunk(tag: bytes, data: bytes) -> bytes:
    body = tag + data
    return struct.pack(">I", len(data)) + body + struct.pack(">I", zlib.crc32(body) & 0xFFFFFFFF)


def write_png(path: str, width: int, height: int, rows: list[bytes], color_type: int) -> None:
    """rows: raw scanlines without filter bytes; color_type 0=gray8, 2=rgb8."""
    raw = b"".join(b"\x00" + r for r in rows)  # filter 0 (None) per scanline
    with open(path, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n")
        f.write(png_chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, color_type, 0, 0, 0)))
        f.write(png_chunk(b"IDAT", zlib.compress(raw, 9)))
        f.write(png_chunk(b"IEND", b""))


def cmd_render(src: str, dst: str) -> None:
    fb = load_fb(src)
    rows: list[bytes] = []
    for y in range(H):
        row = bytearray(W)
        for x in range(W):
            row[x] = 0x00 if pixel(fb, x, y) else 0xFF
        wide = bytes(v for v in row for _ in range(SCALE))
        for _ in range(SCALE):
            rows.append(wide)
    write_png(dst, W * SCALE, H * SCALE, rows, color_type=0)
    print(f"wrote {dst} ({W * SCALE}x{H * SCALE} grayscale)")


def cmd_diff(a_src: str, b_src: str, dst: str) -> None:
    a, b = load_fb(a_src), load_fb(b_src)
    ndiff = 0
    rows: list[bytes] = []
    for y in range(H):
        row = bytearray()
        for x in range(W):
            pa, pb = pixel(a, x, y), pixel(b, x, y)
            if pa != pb:
                ndiff += 1
                px = b"\xff\x00\x00"
            else:
                v = 0x00 if pa else 0xFF
                px = bytes((v, v, v))
            row += px * SCALE
        wide = bytes(row)
        for _ in range(SCALE):
            rows.append(wide)
    write_png(dst, W * SCALE, H * SCALE, rows, color_type=2)
    if ndiff:
        print(f"DIFF: {ndiff} pixels differ -> {dst}")
        sys.exit(1)
    print(f"identical -> {dst}")


def main() -> None:
    args = sys.argv[1:]
    if len(args) == 3 and args[0] == "render":
        cmd_render(args[1], args[2])
    elif len(args) == 4 and args[0] == "diff":
        cmd_diff(args[1], args[2], args[3])
    else:
        sys.stderr.write(USAGE)
        sys.exit(2)


if __name__ == "__main__":
    main()
