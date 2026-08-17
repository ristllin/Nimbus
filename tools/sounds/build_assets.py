#!/usr/bin/env python3
"""sounds build - synthesize the tone pack + write the device sync manifest.

Renders every (event, pool, variant) from palette.py via gen_tones.render()
(pure stdlib, no ffmpeg - nothing here is converted, everything is born as
device-format PCM):

  dist/sd/<pool>/<slug>-<n>.wav   22.05 kHz 16-bit mono (SD sync tier)
  dist/basic/<slug>.wav           16 kHz 16-bit mono    (embedded tier input)

and writes dist/manifest.json: {"version": N, "files": [{path, bytes, sha256}]}
- the device sync contract (paths relative to dist/). The version is the EXACT
int31 content rule the device parses (`.as<uint32_t>()`, generated & 0x7FFFFFFF
- see AGENTS.md): int(sha256(concat(file_hashes))[:8], 16) & 0x7FFFFFFF.

Byte-reproducible: every render is seeded (gen_tones.MASTER_SEED); re-running
yields an identical dist/ and identical manifest version. --selfcheck proves it
by building twice into temp dirs and comparing every byte.

Usage: python3 build_assets.py [--out DIR] [--selfcheck]
"""

import argparse
import hashlib
import json
import pathlib
import shutil
import sys

sys.path.insert(0, str(pathlib.Path(__file__).parent))
from gen_tones import RECIPES, render, write_wav  # noqa: E402
from palette import (  # noqa: E402
    BASIC_EVENTS,
    EVENTS,
    MAX_BASIC_PCM_BYTES,
    MAX_VARIANTS,
    SD_POOLS,
)


def sha256(p: pathlib.Path) -> str:
    h = hashlib.sha256()
    with open(p, "rb") as f:
        for chunk in iter(lambda: f.read(65536), b""):
            h.update(chunk)
    return h.hexdigest()


def check_palette() -> None:
    """Enforce the slug invariants documented in palette.py."""
    bad = []
    for slug, pools in EVENTS.items():
        if "-" in slug:
            bad.append(f"{slug}: '-' inside a slug breaks <slug>-<n>.wav parsing")
        if slug not in RECIPES:
            bad.append(f"{slug}: no recipe in gen_tones.RECIPES")
        for pool, n in pools.items():
            if pool not in SD_POOLS:
                bad.append(f"{slug}: unknown pool {pool}")
            if not (1 <= n <= MAX_VARIANTS):
                bad.append(f"{slug}: {pool} variant count {n} out of 1..{MAX_VARIANTS}")
    for slug in BASIC_EVENTS:
        if slug not in EVENTS:
            bad.append(f"basic event {slug} not in EVENTS")
    if bad:
        raise SystemExit("palette invariant violations:\n  " + "\n  ".join(bad))


def build(out: pathlib.Path) -> dict:
    check_palette()
    if out.exists():
        shutil.rmtree(out)

    n_sd = 0
    for slug, pools in EVENTS.items():
        for pool in SD_POOLS:
            for i in range(pools[pool]):
                dst = out / "sd" / pool / f"{slug}-{i}.wav"
                dst.parent.mkdir(parents=True, exist_ok=True)
                write_wav(dst, render(slug, i, 22050, pool=pool), 22050)
                n_sd += 1

    basic_pcm = 0
    for slug in BASIC_EVENTS:
        dst = out / "basic" / f"{slug}.wav"
        dst.parent.mkdir(parents=True, exist_ok=True)
        samples = render(slug, 0, 16000, pool="basic")
        write_wav(dst, samples, 16000)
        basic_pcm += len(samples) * 2
    if basic_pcm > MAX_BASIC_PCM_BYTES:
        raise SystemExit(
            f"basic tier PCM {basic_pcm} B exceeds the {MAX_BASIC_PCM_BYTES} B "
            "flash budget - trim recipes or drop events from BASIC_EVENTS"
        )

    files = []
    for p in sorted(out.rglob("*.wav")):
        files.append({"path": str(p.relative_to(out)), "bytes": p.stat().st_size, "sha256": sha256(p)})
    content = hashlib.sha256("".join(f["sha256"] for f in files).encode()).hexdigest()
    version = int(content[:8], 16) & 0x7FFFFFFF  # int31-safe content version (device contract)
    manifest = {"version": version, "files": files}
    (out / "manifest.json").write_text(json.dumps(manifest, indent=1))

    sd_bytes = sum(f["bytes"] for f in files if f["path"].startswith("sd/"))
    print(f"SD tier: {n_sd} clips, {sd_bytes / 1024:.0f} KB")
    print(f"basic tier: {len(BASIC_EVENTS)} clips, {basic_pcm / 1024:.0f} KB PCM")
    print(f"manifest version: {version} ({len(files)} files)")
    return manifest


def selfcheck(out: pathlib.Path) -> int:
    """Determinism proof: two independent builds must be byte-identical."""
    import tempfile

    with tempfile.TemporaryDirectory() as tmp:
        a, b = pathlib.Path(tmp) / "a", pathlib.Path(tmp) / "b"
        build(a)
        build(b)
        fa = sorted(p.relative_to(a) for p in a.rglob("*") if p.is_file())
        fb = sorted(p.relative_to(b) for p in b.rglob("*") if p.is_file())
        assert fa == fb, "file sets differ between runs"
        for rel in fa:
            assert (a / rel).read_bytes() == (b / rel).read_bytes(), f"nondeterministic: {rel}"
        if out.exists():
            base = out / "manifest.json"
            if base.exists() and base.read_bytes() != (a / "manifest.json").read_bytes():
                print("note: existing dist/manifest.json differs (recipes changed?) - rerun build")
    print(f"selfcheck OK: {len(fa)} files byte-identical across two builds")
    return 0


def main() -> int:
    ap = argparse.ArgumentParser()
    here = pathlib.Path(__file__).resolve().parent
    ap.add_argument("--out", default=str(here / "dist"))
    ap.add_argument("--selfcheck", action="store_true", help="build twice, assert byte-identical")
    args = ap.parse_args()
    out = pathlib.Path(args.out).resolve()
    if args.selfcheck:
        return selfcheck(out)
    build(out)
    return 0


if __name__ == "__main__":
    sys.exit(main())
