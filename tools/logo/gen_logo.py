#!/usr/bin/env python3
"""Nimbus logo generator - the dotted teal ring (owner-approved variant I, 2026-07-16).

The logo is a ring of teal dots in crisp concentric rows: geometry is regular
(touching rows, even angular spacing, tiny jitter) and ALL the life comes from
per-dot variance - a flat random draw across the 8-shade teal palette, with
opacity tied to shade (darks stay solid, lights may fade further). That
combination is what matches the reference art; clump fields and positional
wobble were tried and rejected (looked scribbly / mid-teal mush).

Everything is SEEDED - re-running regenerates the same assets byte-for-byte,
so the SVGs checked into the repo are a build product of this file (edit here,
re-run, never hand-edit the outputs).

Outputs (repo-relative):
  assets/logo.svg                 master, 1024 viewBox, ~800 dots
  assets/logo-mark.svg            simplified 128 viewBox - favicon/header scale
  assets/logo-wordmark.svg        mark + "Nimbus" wordmark (README, banners)
  website/static/img/logo.svg     = mark (Docusaurus navbar)
  website/static/img/favicon.svg  = mark
  website/static/img/social-card.svg  1200x630 card (ring + name)
  include/web/ui_logo.h           PROGMEM minified mark for the device web UI
  assets/icon-512.png             rasterized mark (PIL, supersampled)
  assets/icon-180.png             apple-touch-icon size
  assets/social-card.png          1200x630 raster card
  website/static/img/social-card.png  = raster card (PNG og:image for crawlers)

Run from the repo root:  python3 tools/logo/gen_logo.py
"""

from __future__ import annotations

import math
import os
import random
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

MASTER_SEED = 5  # variant I - the owner-approved draw
MARK_SEED = 7

# Teal palette, dark -> pale (sampled to match the reference art).
PALETTE = [
    "#0B7285",  # 0 deep
    "#0F8A9D",  # 1
    "#15AABF",  # 2 mid
    "#22B8CF",  # 3
    "#3BC9DB",  # 4
    "#66D9E8",  # 5 soft
    "#99E9F2",  # 6 pale
    "#C5F6FA",  # 7 faint
]
# Flat-ish shade distribution: every shade shows up, mids slightly favored.
FLAT_WEIGHTS = [12, 12, 14, 14, 14, 12, 12, 10]


def dot_ring(
    *,
    cx,
    cy,
    r_in,
    r_out,
    rows,
    dot_r,
    seed,
    spacing=2.0,
    jitter=0.2,
    size_jitter=0.08,
    weights=FLAT_WEIGHTS,
    palette=PALETTE,
    op_dark=(0.82, 1.0),
    op_light=(0.60, 1.0),
):
    """Yield (x, y, r, color, opacity) - variant I recipe.

    Rows touch (row step ~= dot diameter when rows/dot_r are chosen that way),
    dots sit on regular angular grid with only slight radial jitter. Shade is
    an independent weighted draw per dot; opacity depends on shade so dark
    dots read solid while light dots can fade toward the background.
    """
    rng = random.Random(seed)
    idxs = list(range(len(palette)))
    dots = []
    step = (r_out - r_in) / (rows - 1) if rows > 1 else 0
    for row in range(rows):
        rbase = r_in + row * step
        n = max(6, int((2 * math.pi * rbase) / (spacing * dot_r)))
        phase = rng.uniform(0, 2 * math.pi)
        for k in range(n):
            a = phase + k * 2 * math.pi / n
            r = rbase + rng.uniform(-jitter, jitter) * dot_r
            x = cx + r * math.cos(a)
            y = cy + r * math.sin(a)
            idx = rng.choices(idxs, weights=weights)[0]
            lo, hi = op_dark if idx <= 2 else op_light
            op = round(rng.uniform(lo, hi), 2)
            rr = dot_r * (1 + rng.uniform(-size_jitter, size_jitter))
            dots.append((x, y, rr, palette[idx], op))
    return dots


def master_dots():
    return dot_ring(cx=512, cy=512, r_in=290, r_out=470, rows=8, dot_r=12.0, seed=MASTER_SEED)


def mark_dots():
    # Small-scale variant: fewer, larger dots (a 1:1 shrink of the master is
    # unreadable at 16-32 px), palest tone dropped for contrast, opacity floor
    # raised so the ring stays visible at favicon size.
    return dot_ring(
        cx=64,
        cy=64,
        r_in=37,
        r_out=56,
        rows=3,
        dot_r=4.8,
        seed=MARK_SEED,
        spacing=2.0,
        weights=FLAT_WEIGHTS[:7],
        palette=PALETTE[:7],
        op_dark=(0.9, 1.0),
        op_light=(0.7, 1.0),
    )


def svg_dots(dots, digits=1):
    parts = []
    for x, y, r, c, op in dots:
        o = "" if op >= 0.99 else f' fill-opacity="{op}"'
        parts.append(f'<circle cx="{x:.{digits}f}" cy="{y:.{digits}f}" r="{r:.{digits}f}" fill="{c}"{o}/>')
    return "".join(parts)


def svg_doc(view, body, title="Nimbus"):
    return f'<svg xmlns="http://www.w3.org/2000/svg" viewBox="{view}" role="img" aria-label="{title}">{body}</svg>\n'


def write(relpath, content):
    path = os.path.join(ROOT, relpath)
    os.makedirs(os.path.dirname(path), exist_ok=True)
    mode = "wb" if isinstance(content, bytes) else "w"
    with open(path, mode) as f:
        f.write(content)
    size = len(content if isinstance(content, bytes) else content.encode())
    print(f"  {relpath}  ({size / 1024:.1f} KB)")


def card_ring():
    return dot_ring(cx=280, cy=315, r_in=118, r_out=196, rows=6, dot_r=8.6, seed=MASTER_SEED)


def gen_svgs():
    master = svg_doc("0 0 1024 1024", svg_dots(master_dots()))
    mark = svg_doc("0 0 128 128", svg_dots(mark_dots()))
    wordmark = svg_doc(
        "0 0 420 128",
        svg_dots(mark_dots()) + '<text x="140" y="82" font-family="Avenir Next, Segoe UI, system-ui, sans-serif" '
        'font-size="56" font-weight="600" letter-spacing="1" fill="#0B7285">Nimbus</text>',
    )
    card = svg_doc(
        "0 0 1200 630",
        '<rect width="1200" height="630" fill="#ffffff"/>'
        + svg_dots(card_ring())
        + '<text x="540" y="330" font-family="Avenir Next, Segoe UI, system-ui, sans-serif" '
        'font-size="110" font-weight="600" fill="#0B7285">Nimbus</text>'
        + '<text x="544" y="392" font-family="Avenir Next, Segoe UI, system-ui, sans-serif" '
        'font-size="34" fill="#15AABF">Your desk-side Agent</text>',
    )

    write("assets/logo.svg", master)
    write("assets/logo-mark.svg", mark)
    write("assets/logo-wordmark.svg", wordmark)
    write("website/static/img/logo.svg", mark)
    write("website/static/img/favicon.svg", mark)
    write("website/static/img/social-card.svg", card)
    return mark


def gen_progmem_header(mark_svg):
    """include/web/ui_logo.h - the mark served at GET /logo.svg by the device."""
    minified = mark_svg.strip()
    esc = minified.replace('"', '\\"')
    header = (
        "// GENERATED by tools/logo/gen_logo.py - do not hand-edit (edit the\n"
        "// generator + re-run; the SVG assets and this header stay in lockstep).\n"
        "// The Nimbus dotted-ring mark, served at GET /logo.svg (open route: the\n"
        "// identify gate needs it pre-auth; a logo is not sensitive) and referenced\n"
        "// as the favicon + header mark by ui_shell.h.\n"
        "#pragma once\n"
        "#include <pgmspace.h>\n\n"
        f'static const char UI_LOGO_SVG[] PROGMEM = "{esc}";\n'
    )
    write("include/web/ui_logo.h", header)


def gen_pngs():
    try:
        from PIL import Image, ImageDraw, ImageFont
    except ImportError:
        print("  (PIL unavailable - skipping PNG exports)")
        return
    import io

    def raster(dots, w, h, scale=4, bg=(255, 255, 255, 0)):
        img = Image.new("RGBA", (w * scale, h * scale), bg)
        d = ImageDraw.Draw(img, "RGBA")
        for x, y, r, c, op in dots:
            rgb = tuple(int(c[i : i + 2], 16) for i in (1, 3, 5))
            d.ellipse([(x - r) * scale, (y - r) * scale, (x + r) * scale, (y + r) * scale], fill=rgb + (int(op * 255),))
        return img.resize((w, h), Image.LANCZOS)

    icon = raster([(x * 4, y * 4, r * 4, c, o) for x, y, r, c, o in mark_dots()], 512, 512)
    buf = io.BytesIO()
    icon.save(buf, "PNG")
    write("assets/icon-512.png", buf.getvalue())
    buf = io.BytesIO()
    icon.resize((180, 180), Image.LANCZOS).save(buf, "PNG")
    write("assets/icon-180.png", buf.getvalue())

    card = raster(card_ring(), 1200, 630, scale=2, bg=(255, 255, 255, 255))
    d = ImageDraw.Draw(card)
    font = sub = None
    for fp in ("/System/Library/Fonts/Supplemental/Avenir Next.ttc", "/System/Library/Fonts/Helvetica.ttc"):
        try:
            font = ImageFont.truetype(fp, 110)
            sub = ImageFont.truetype(fp, 34)
            break
        except OSError:
            continue
    if font:
        d.text((540, 240), "Nimbus", font=font, fill=(11, 114, 133))
        d.text((544, 370), "Your desk-side Agent", font=sub, fill=(21, 170, 191))
    buf = io.BytesIO()
    card.save(buf, "PNG")
    png_bytes = buf.getvalue()
    write("assets/social-card.png", png_bytes)
    # Social crawlers (X/Slack/LinkedIn) don't render SVG og:images, so the
    # site's og:image points at a PNG - emit it into the Docusaurus static dir.
    write("website/static/img/social-card.png", png_bytes)


if __name__ == "__main__":
    print(f"generating into {ROOT}:")
    mark = gen_svgs()
    gen_progmem_header(mark)
    gen_pngs()
    sys.exit(0)
