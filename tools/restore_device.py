#!/usr/bin/env python3
"""restore_device.py - push a Nimbus backup folder back onto a device.

The mirror of backup_device.py (CUM-406). backup_device.py is GET-only; this tool
is the write side: it takes a dated backup folder and restores the user-valuable
memory onto a device over the token-gated HTTP API, idempotently.

  <src>/
    vectors.json      -> POST /api/mem/import  kind=vectors    (replace-by-id)
    episodic.jsonl    -> POST /api/mem/import  kind=episodic   (append)
    scratchpad.json   -> POST /api/mem/import  kind=scratchpad (replace)
    files/<proj>/<nm> -> POST /api/files/upload                (overwrite; identical skipped)
    MANIFEST.json     -> verified before and after (counts + file sizes)

Idempotency:
  * vectors    - exact replace-by-id on the device, so re-running (or a retried
                 page) converges to the same set. Always safe.
  * scratchpad - replace. Always safe.
  * episodic   - the append-log store has no cross-call id index, so this tool
                 REFUSES to push episodic to a device that already has episodic
                 rows unless --force (a fresh-device restore never duplicates).
  * files      - a file already present with the same size is skipped.

Auth: the per-device access token travels ONLY on the "X-Nimbus-Token" header,
never a ?t= query (CUM-45), exactly like backup_device.py.

Secrets (provider keys, tokens) are NEVER restored - they are not in a backup
folder, and this tool touches only the memory engines + the artifact store.

Usage:
  # dry run first - validate + see counts, write nothing:
  python3 tools/restore_device.py --ip 192.0.2.10 --token <ACCESS_TOKEN> \
      --src ~/NimbusBackups/Lumi/2026-09-13_1042 --dry-run

  # then apply:
  python3 tools/restore_device.py --ip 192.0.2.10 --token <ACCESS_TOKEN> \
      --src ~/NimbusBackups/Lumi/2026-09-13_1042

  # locate the newest backup for a device automatically:
  python3 tools/restore_device.py --ip 192.0.2.10 --token <ACCESS_TOKEN> \
      --dest ~/NimbusBackups --dev Lumi --date latest
"""

import argparse
import json
import pathlib
import sys
import urllib.error
import urllib.parse
import urllib.request
import uuid


def _req(base, tok, path, data=None, ctype=None, timeout=60):
    url = base + path
    headers = {"X-Nimbus-Token": tok}
    if ctype:
        headers["Content-Type"] = ctype
    req = urllib.request.Request(url, data=data, headers=headers, method="POST" if data is not None else "GET")
    with urllib.request.urlopen(req, timeout=timeout) as r:
        return r.read()


def get_json(base, tok, path):
    return json.loads(_req(base, tok, path))


def post_import(base, tok, payload, timeout=120):
    body = json.dumps(payload).encode()
    return json.loads(_req(base, tok, "/api/mem/import", body, "application/json", timeout))


def resolve_src(a):
    if a.src:
        return pathlib.Path(a.src).expanduser()
    if not (a.dev and a.date):
        sys.exit("give --src, or --dest + --dev + --date")
    root = pathlib.Path(a.dest).expanduser() / a.dev
    if a.date == "latest":
        dates = sorted(p.name for p in root.iterdir() if p.is_dir())
        if not dates:
            sys.exit(f"no backups under {root}")
        return root / dates[-1]
    return root / a.date


def load_manifest(src):
    mf = src / "MANIFEST.json"
    if not mf.exists():
        sys.exit(f"no MANIFEST.json in {src} - not a backup folder")
    return json.loads(mf.read_bytes())


def chunks(seq, n):
    for i in range(0, len(seq), n):
        yield seq[i : i + n]


def restore_vectors(base, tok, src, a):
    p = src / "vectors.json"
    if not p.exists():
        print("  vectors: (none in backup)")
        return
    entries = json.loads(p.read_bytes()).get("entries", [])
    have_vec = sum(1 for e in entries if e.get("vec"))
    if entries and not have_vec:
        print(
            f"  vectors: {len(entries)} rows but NONE carry an embedding "
            "(pre-CUM-406 backup) - they cannot be restored losslessly; skipping."
        )
        return
    tot = {"added": 0, "replaced": 0, "skippedNoVec": 0, "widthErrors": 0, "badRows": 0}
    for batch in chunks(entries, a.batch):
        rep = post_import(base, tok, {"kind": "vectors", "dryRun": a.dry_run, "entries": batch})
        for k in tot:
            tot[k] += int(rep.get(k, 0))
    print(
        f"  vectors: {'(dry-run) ' if a.dry_run else ''}"
        f"added={tot['added']} replaced={tot['replaced']} "
        f"skippedNoVec={tot['skippedNoVec']} widthErrors={tot['widthErrors']} "
        f"badRows={tot['badRows']} (of {len(entries)})"
    )
    if tot["widthErrors"]:
        print(
            "  WARNING: some vectors did not match the device embedding width. Set the "
            "SAME embed config (provider/model/dims) on the device before restore.",
            file=sys.stderr,
        )


def restore_episodic(base, tok, src, a):
    p = src / "episodic.jsonl"
    if not p.exists():
        print("  episodic: (none in backup)")
        return
    msgs = [json.loads(line) for line in p.read_text().splitlines() if line.strip()]
    if not msgs:
        print("  episodic: (empty)")
        return
    if not a.dry_run and not a.force:
        stats = get_json(base, tok, "/api/mem/stats")
        if int(stats.get("episodicMsgs", 0)) > 0:
            print(
                f"  episodic: device already has {stats['episodicMsgs']} messages - "
                "SKIPPING (append would duplicate). Re-run with --force to append anyway.",
                file=sys.stderr,
            )
            return
    added = 0
    for batch in chunks(msgs, a.batch):
        rep = post_import(base, tok, {"kind": "episodic", "dryRun": a.dry_run, "messages": batch})
        added += int(rep.get("added", 0))
    print(f"  episodic: {'(dry-run) ' if a.dry_run else ''}added={added} (of {len(msgs)})")


def restore_scratchpad(base, tok, src, a):
    p = src / "scratchpad.json"
    if not p.exists():
        print("  scratchpad: (none in backup)")
        return
    sp = json.loads(p.read_bytes())
    rep = post_import(base, tok, {"kind": "scratchpad", "dryRun": a.dry_run, "scratchpad": sp})
    print(
        f"  scratchpad: {'(dry-run) ' if a.dry_run else ''}active={rep.get('active')} "
        f"short={rep.get('short')} mid={rep.get('mid')} long={rep.get('long')}"
    )


def upload_file(base, tok, project, name, data, timeout=120):
    # Minimal multipart/form-data body: one file part is all the device reads
    # (the project + name ride the query string; name falls back to the part's
    # filename). Boundary is random so it can never appear in the payload.
    boundary = "----nimbusrestore" + uuid.uuid4().hex
    pre = (
        f"--{boundary}\r\n"
        f'Content-Disposition: form-data; name="file"; filename="{name}"\r\n'
        "Content-Type: application/octet-stream\r\n\r\n"
    ).encode()
    post = f"\r\n--{boundary}--\r\n".encode()
    body = pre + data + post
    q = urllib.parse.urlencode({"project": project, "name": name})
    return json.loads(
        _req(base, tok, "/api/files/upload?" + q, body, f"multipart/form-data; boundary={boundary}", timeout)
    )


def device_file_sizes(base, tok):
    """Map (project, name) -> bytes for what the device already holds (idempotent skip)."""
    have = {}
    try:
        for f in get_json(base, tok, "/api/files/list").get("files", []):
            have[(f["project"], f["name"])] = int(f.get("bytes", -1))
    except urllib.error.HTTPError:
        pass
    return have


def split_project_name(rel):
    """A files/ relative path -> (project, name). Top folder is the project."""
    parts = rel.parts
    if len(parts) > 1:
        return parts[0], "/".join(parts[1:])
    return "uploads", parts[0]


def push_one_file(base, tok, project, name, data):
    """Upload one file; return 'up' or 'fail' (printing the reason on failure)."""
    try:
        r = upload_file(base, tok, project, name, data)
        if r.get("ok"):
            return "up"
        print(f"    FAIL {project}/{name}: {r.get('error')}", file=sys.stderr)
    except Exception as e:  # keep going - one hole beats aborting the rest
        print(f"    FAIL {project}/{name}: {e}", file=sys.stderr)
    return "fail"


def restore_files(base, tok, src, a):
    fdir = src / "files"
    if a.no_files or not fdir.exists():
        print("  files: (skipped)" if a.no_files else "  files: (none in backup)")
        return
    have = device_file_sizes(base, tok)
    tally = {"up": 0, "skip": 0, "fail": 0}
    for fp in (p for p in fdir.rglob("*") if p.is_file()):
        project, name = split_project_name(fp.relative_to(fdir))
        data = fp.read_bytes()
        if have.get((project, name)) == len(data):
            tally["skip"] += 1
        elif a.dry_run:
            tally["up"] += 1
        else:
            tally[push_one_file(base, tok, project, name, data)] += 1
    print(
        f"  files: {'(dry-run) would upload ' if a.dry_run else 'uploaded '}{tally['up']}, "
        f"skipped {tally['skip']} identical, {tally['fail']} failed"
    )


def main():
    ap = argparse.ArgumentParser(description="Restore a Nimbus backup folder onto a device.")
    ap.add_argument("--ip", required=True)
    ap.add_argument("--token", required=True)
    ap.add_argument("--src", help="the dated backup folder (…/<dev>/<date>)")
    ap.add_argument("--dest", default=str(pathlib.Path.home() / "NimbusBackups"))
    ap.add_argument("--dev", help="device name under --dest (with --date)")
    ap.add_argument("--date", help="a dated subfolder, or 'latest'")
    ap.add_argument("--dry-run", action="store_true", help="validate + count; write nothing")
    ap.add_argument("--force", action="store_true", help="append episodic even if the device is non-empty")
    ap.add_argument("--no-files", action="store_true", help="skip the artifact files")
    ap.add_argument("--batch", type=int, default=64, help="rows per import request (default 64)")
    a = ap.parse_args()
    base = f"http://{a.ip}"
    tok = a.token

    src = resolve_src(a)
    if not src.is_dir():
        sys.exit(f"not a folder: {src}")
    manifest = load_manifest(src)

    # Confirm we are pointed at the intended device (name is advisory - a restore to a
    # differently-named scratch device is legitimate, so warn, never block).
    try:
        state = get_json(base, tok, "/api/state")
        dev_now = state.get("devName", "?")
        if manifest.get("device") and dev_now != manifest["device"]:
            print(f"NOTE: backup is from '{manifest['device']}', device reports '{dev_now}'.")
    except Exception as e:
        sys.exit(f"cannot reach device /api/state: {e}")

    print(f"restore {'(DRY RUN) ' if a.dry_run else ''}{src} -> {a.ip} ({dev_now})")
    print(
        f"  manifest: {manifest.get('vectorCount', '?')} vectors, "
        f"{manifest.get('episodicCount', '?')} episodic, "
        f"{len(manifest.get('files', []))} files"
    )
    restore_vectors(base, tok, src, a)
    restore_episodic(base, tok, src, a)
    restore_scratchpad(base, tok, src, a)
    restore_files(base, tok, src, a)

    if not a.dry_run:
        after = get_json(base, tok, "/api/mem/stats")
        print(
            f"verify: device now holds {after.get('vectors')} vectors, {after.get('episodicMsgs')} episodic messages."
        )
    print("done." if not a.dry_run else "dry run complete - nothing was written.")


if __name__ == "__main__":
    main()
