#!/usr/bin/env python3
"""backup_device.py - pull a Nimbus device's user data to the Mac.

The owner dogfoods a device; its SD card is a single point of failure (cold
joints have unmounted cards before). This pulls everything user-valuable over
the token-gated HTTP API into a dated folder:

  <dest>/<devName>/<YYYY-MM-DD_HHMM>/
    files/<project>/<name>       every artifact-store file, byte-for-byte
    vectors.json                 the full vector DB (content + metadata, paged)
    episodic.jsonl               the full episodic history (day-streams, paged)
    scratchpad.json              goal tiers
    state.json                   /api/state snapshot (fw, storage, health)
    usage.json                   the usage/budget ledger view
    MANIFEST.json                what was saved, sizes, and any per-item errors

Auth: the per-device access token travels as the "X-Nimbus-Token" header on
every request. A URL query (?t=) is dropped by the device since CUM-45 (a query
lands in browser/history/log records), so it is never used here.

Secrets (provider keys, tokens) are deliberately NOT exported - NVS config is
not user data and a backup folder must not become a credential store.

Usage:
  python3 tools/backup_device.py --ip 192.0.2.10 --token <ACCESS_TOKEN> \
      [--dest ~/NimbusBackups]

Read-only: only GETs (and the paged vector/episodic browse). Safe to run any time.
"""

import argparse
import datetime
import json
import pathlib
import sys
import urllib.parse
import urllib.request


def get(base, tok, path, params=None, timeout=30):
    url = base + path
    if params:
        url += "?" + urllib.parse.urlencode(params)
    # CUM-45: the token authenticates on the X-Nimbus-Token header, NEVER a ?t=
    # query param (the device drops the query). One header, every request.
    req = urllib.request.Request(url, headers={"X-Nimbus-Token": tok})
    with urllib.request.urlopen(req, timeout=timeout) as r:
        return r.read()


def export_episodic(base, tok, root, manifest):
    """Page the full episodic history to episodic.jsonl (one message per line).

    Walks GET /api/mem/episodic with cold=1 (reads the SD day-streams below the
    boot-scan index, not just the in-RAM window), following the nextBefore cursor
    the store hands back until no older history remains. Writes incrementally so a
    mid-run failure still leaves what was pulled on disk.
    """
    count, pages, before, seen = 0, 0, None, set()
    info = {}
    out = root / "episodic.jsonl"
    try:
        with out.open("wb") as fh:
            while True:
                params = {"limit": 200, "cold": 1}
                if before:
                    params["before"] = before
                page = json.loads(get(base, tok, "/api/mem/episodic", params, timeout=120))
                msgs = page.get("messages", [])
                for m in msgs:
                    fh.write(json.dumps(m).encode() + b"\n")
                count += len(msgs)
                pages += 1
                info = {
                    "searchedTo": page.get("searchedTo"),
                    "olderExists": page.get("olderExists", False),
                    "total": page.get("total"),
                }
                nxt = page.get("nextBefore") or ""
                # Stop when nothing older remains, the store hands back no cursor,
                # or the cursor stops advancing (guards against an endless loop).
                if not page.get("olderExists") or not nxt or nxt in seen:
                    break
                seen.add(nxt)
                before = nxt
    except Exception as e:  # keep the rest of the backup going
        manifest["errors"].append(f"/api/mem/episodic: {e}")
    manifest["episodicCount"] = count
    manifest["episodicPages"] = pages
    manifest["episodicInfo"] = info
    print(f"  episodic: {count} messages ({pages} pages)")
    return count


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

    # ---- episodic history: paged day-streams to JSONL ----------------------
    episodic = export_episodic(base, tok, root, manifest)

    # ---- scratchpad + usage ------------------------------------------------
    for path, fname in (("/api/mem/scratchpad", "scratchpad.json"), ("/api/orch", "usage.json")):
        try:
            (root / fname).write_bytes(get(base, tok, path))
        except Exception as e:
            manifest["errors"].append(f"{path}: {e}")

    manifest["vectorCount"] = len(vectors)
    (root / "MANIFEST.json").write_bytes(json.dumps(manifest, indent=1).encode())
    print(f"backup -> {root}")
    print(f"{len(files)} files, {len(vectors)} vectors, {episodic} episodic, {len(manifest['errors'])} errors")
    if manifest["errors"]:
        for e in manifest["errors"]:
            print("  ERROR:", e, file=sys.stderr)
        sys.exit(1)


if __name__ == "__main__":
    main()
