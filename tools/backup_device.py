#!/usr/bin/env python3
"""backup_device.py - pull a Nimbus device's user data to the Mac.

The owner dogfoods a device; its SD card is a single point of failure (cold
joints have unmounted cards before). This pulls everything user-valuable over
the token-gated HTTP API into a dated folder:

  <dest>/<devName>/<YYYY-MM-DD_HHMM>/
    files/<project>/<name>       every artifact-store file, byte-for-byte
    vectors.json                 the full vector DB (content + metadata, paged)
    scratchpad.json              goal tiers
    state.json                   /api/state snapshot (fw, storage, health)
    usage.json                   the usage/budget ledger view
    MANIFEST.json                what was saved, sizes, and any per-item errors

Secrets (provider keys, tokens) are deliberately NOT exported - NVS config is
not user data and a backup folder must not become a credential store.

Usage:
  python3 tools/backup_device.py --ip 192.0.2.10 --token <WEBTOK> \
      [--dest ~/NimbusBackups]

Read-only: only GETs (and the paged vector browse). Safe to run any time.
"""

import argparse
import datetime
import json
import pathlib
import sys
import urllib.parse
import urllib.request


def get(base, tok, path, params=None, timeout=30):
    q = dict(params or {})
    q["t"] = tok
    url = base + path + "?" + urllib.parse.urlencode(q)
    with urllib.request.urlopen(url, timeout=timeout) as r:
        return r.read()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--ip", required=True)
    ap.add_argument("--token", required=True)
    ap.add_argument("--dest", default=str(pathlib.Path.home() / "NimbusBackups"))
    a = ap.parse_args()
    base = f"http://{a.ip}"
    tok = a.token

    state = json.loads(get(base, tok, "/api/state"))
    dev = state.get("devName", a.ip).replace("/", "_")
    stamp = datetime.datetime.now().strftime("%Y-%m-%d_%H%M")
    root = pathlib.Path(a.dest).expanduser() / dev / stamp
    root.mkdir(parents=True, exist_ok=True)
    manifest = {"device": dev, "ip": a.ip, "fw": state.get("fw"), "when": stamp, "files": [], "errors": []}

    (root / "state.json").write_bytes(json.dumps(state, indent=1).encode())

    # ---- artifact store: every file, byte-for-byte -------------------------
    listing = json.loads(get(base, tok, "/api/files/list"))
    files = listing.get("files", [])
    fdir = root / "files"
    for f in files:
        proj, name, size = f["project"], f["name"], f.get("bytes", 0)
        try:
            data = get(base, tok, "/api/files/dl", {"project": proj, "name": name}, timeout=120)
            out = fdir / proj / name
            out.parent.mkdir(parents=True, exist_ok=True)
            out.write_bytes(data)
            ok = len(data) == size
            manifest["files"].append(
                {"project": proj, "name": name, "bytes": len(data), "expected": size, "sizeMatch": ok}
            )
            if not ok:
                manifest["errors"].append(f"size mismatch {proj}/{name}: {len(data)} != {size}")
            print(f"  file {proj}/{name} {len(data)}B{'' if ok else '  SIZE MISMATCH'}")
        except Exception as e:  # keep going - a backup with one hole beats none
            manifest["errors"].append(f"download failed {proj}/{name}: {e}")
            print(f"  FAIL {proj}/{name}: {e}", file=sys.stderr)

    # ---- vector DB: paged browse until exhausted ---------------------------
    vectors, offset = [], 0
    while True:
        page = json.loads(get(base, tok, "/api/mem/vector", {"limit": 100, "offset": offset}))
        rows = page.get("entries", [])
        vectors.extend(rows)
        total = page.get("total", 0)
        offset += len(rows)
        if not rows or offset >= total:
            break
    (root / "vectors.json").write_bytes(json.dumps({"total": len(vectors), "entries": vectors}, indent=1).encode())
    print(f"  vectors: {len(vectors)}")

    # ---- scratchpad + usage ------------------------------------------------
    for path, fname in (("/api/mem/scratch", "scratchpad.json"), ("/api/orch", "usage.json")):
        try:
            (root / fname).write_bytes(get(base, tok, path))
        except Exception as e:
            manifest["errors"].append(f"{path}: {e}")

    manifest["vectorCount"] = len(vectors)
    (root / "MANIFEST.json").write_bytes(json.dumps(manifest, indent=1).encode())
    print(f"backup -> {root}")
    print(f"{len(files)} files, {len(vectors)} vectors, {len(manifest['errors'])} errors")
    if manifest["errors"]:
        for e in manifest["errors"]:
            print("  ERROR:", e, file=sys.stderr)
        sys.exit(1)


if __name__ == "__main__":
    main()
