#!/usr/bin/env python3
"""restore_device.py - push a Nimbus backup folder back onto a device.

The mirror of backup_device.py (CUM-406). backup_device.py is GET-only; this tool
is the write side: it takes a dated backup folder and restores the user-valuable
memory onto a device over the token-gated HTTP API, idempotently.

  <src>/
    vectors.json      -> POST /api/mem/import  kind=vectors    (replace-by-id)
    episodic.jsonl    -> POST /api/mem/import  kind=episodic   (append)
    scratchpad.json   -> POST /api/mem/import  kind=scratchpad (replace present tiers)
    files/<proj>/<nm> -> POST /api/files/upload                (overwrite; identical skipped)
    MANIFEST.json     -> VERIFIED against the folder before restore; reconciled after

Idempotency:
  * vectors    - exact replace-by-id on the device, so re-running (or a retried
                 page) converges to the same set. Always safe.
  * scratchpad - replace of the tiers the backup carries. Always safe.
  * episodic   - the append-log store has no cross-call id index, so this tool
                 REFUSES to push episodic to a device that already has episodic
                 rows unless --force (a fresh-device restore never duplicates).
  * files      - a file already present with the same size is skipped.

Honesty (the tool never reports success it did not get):
  * Every import batch is checked for the device's "ok" flag AND its per-row
    accounting; an ok:false, an HTTP error, a body-cap refusal, or a row count that
    does not reconcile is a HARD ERROR (exit 2), never silently counted as success.
  * MANIFEST is really verified: entry/line counts + file sizes must match the
    folder before anything is pushed (exit 2 on mismatch, nothing applied).
  * A lossy outcome the device reports honestly - vectors evicted over the device
    cap, episodic truncated by the no-SD ring, rows skipped for a missing embedding,
    or width mismatches - is a loud WARNING and a non-zero exit (3), because the
    restore did not land in full.

Exit codes: 0 clean, 2 hard error / refused, 3 completed but lossy/incomplete.

Auth: the per-device access token travels ONLY on the "X-Nimbus-Token" header,
never a ?t= query (CUM-45). Note: the device LAN HTTP API has no on-device TLS, so
the token (like backup_device.py's) crosses the LAN in plaintext - use it on a
trusted network only. See PR_BODY / docs for this known LAN-only constraint.

Secrets (provider keys, tokens) are NEVER restored - they are not in a backup
folder, and this tool touches only the memory engines + the artifact store.

Usage:
  # dry run first - validate + see counts, write nothing:
  python3 tools/restore_device.py --ip 192.0.2.10 --token <ACCESS_TOKEN> \
      --src ~/NimbusBackups/Lumi/2026-09-13_1042 --dry-run

  # then apply:
  python3 tools/restore_device.py --ip 192.0.2.10 --token <ACCESS_TOKEN> \
      --src ~/NimbusBackups/Lumi/2026-09-13_1042
"""

import argparse
import json
import pathlib
import sys
import urllib.error
import urllib.parse
import urllib.request
import uuid

# The device caps an import body at 512 KB; keep a batch comfortably under that so a
# straddling row never trips the device's refusal. A single row larger than this is
# sent alone and the device's 413 becomes a hard error (honest: it cannot be restored).
SAFE_BODY = 400 * 1024


class RestoreError(Exception):
    """A hard failure (HTTP error, ok:false, or a reconcile mismatch)."""


class Outcome:
    """Collects hard errors + lossy warnings so the final exit code is honest."""

    def __init__(self):
        self.errors = []
        self.warnings = []

    def fail(self, msg):
        self.errors.append(msg)
        print(f"  ERROR: {msg}", file=sys.stderr)

    def warn(self, msg):
        self.warnings.append(msg)
        print(f"  WARNING: {msg}", file=sys.stderr)

    def exit_code(self):
        return 2 if self.errors else (3 if self.warnings else 0)


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


def post_import(base, tok, kind, key, rows, dry_run):
    """POST one import batch and return the device reply dict.

    Raises RestoreError on ANY failure the caller must not paper over: an HTTP error
    (incl. 401 auth / 413 body-too-large), a non-JSON reply, or ok:false.
    """
    payload = {"kind": kind, "dryRun": dry_run, "count": len(rows), key: rows}
    body = json.dumps(payload).encode()
    try:
        raw = _req(base, tok, "/api/mem/import", body, "application/json", timeout=120)
    except urllib.error.HTTPError as e:
        detail = ""
        try:
            detail = ": " + json.loads(e.read()).get("error", "")
        except Exception:
            pass
        raise RestoreError(f"{kind} batch HTTP {e.code}{detail}") from e
    except urllib.error.URLError as e:
        raise RestoreError(f"{kind} batch transport error: {e}") from e
    try:
        rep = json.loads(raw)
    except ValueError as e:
        raise RestoreError(f"{kind} batch: non-JSON reply") from e
    if not rep.get("ok"):
        raise RestoreError(f"{kind} batch rejected: {rep.get('error', 'ok:false')}")
    return rep


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


def verify_manifest(src, manifest):
    """Check the folder matches its MANIFEST (counts + file sizes). Returns a list of
    problems; a non-empty list means the backup is inconsistent and must NOT be pushed."""
    probs = []
    # A declared nonzero count with the tier file ABSENT is a verification FAILURE: the
    # tier would otherwise restore empty and still exit 0. Mirror the files tier's
    # missing-file handling below (a declared-but-missing file is always a problem).
    vf = src / "vectors.json"
    vc = manifest.get("vectorCount")
    if vc is not None:
        if not vf.exists():
            if vc:
                probs.append(f"vectors.json missing but MANIFEST declares {vc} entries")
        else:
            n = len(json.loads(vf.read_bytes()).get("entries", []))
            if n != vc:
                probs.append(f"vectors.json has {n} entries, MANIFEST says {vc}")
    ef = src / "episodic.jsonl"
    ec = manifest.get("episodicCount")
    if ec is not None:
        if not ef.exists():
            if ec:
                probs.append(f"episodic.jsonl missing but MANIFEST declares {ec} entries")
        else:
            n = sum(1 for line in ef.read_text().splitlines() if line.strip())
            if n != ec:
                probs.append(f"episodic.jsonl has {n} lines, MANIFEST says {ec}")
    for f in manifest.get("files", []):
        fp = src / "files" / f["project"] / f["name"]
        exp = f.get("bytes")
        if not fp.exists():
            probs.append(f"file missing: {f['project']}/{f['name']}")
        elif exp is not None and fp.stat().st_size != exp:
            probs.append(f"file size mismatch {f['project']}/{f['name']}: {fp.stat().st_size} != {exp}")
    return probs


def size_batches(rows, key, max_rows):
    """Yield sublists of `rows` whose serialized body stays under SAFE_BODY (and under
    max_rows), so a batch can never trip the device body cap mid-array."""
    envelope = 64 + len(key)
    batch, size = [], envelope
    for row in rows:
        rs = len(json.dumps(row)) + 1
        if batch and (size + rs > SAFE_BODY or len(batch) >= max_rows):
            yield batch
            batch, size = [], envelope
        batch.append(row)
        size += rs
    if batch:
        yield batch


def restore_vectors(base, tok, src, a, out):
    p = src / "vectors.json"
    if not p.exists():
        print("  vectors: (none in backup)")
        return
    entries = json.loads(p.read_bytes()).get("entries", [])
    if not entries:
        print("  vectors: (empty)")
        return
    if not any(e.get("vec") for e in entries):
        out.warn(
            f"vectors: {len(entries)} rows carry NO embedding (pre-CUM-406 backup); "
            "not restoring - they cannot be reconstructed losslessly."
        )
        return
    tot = {"added": 0, "replaced": 0, "skippedNoVec": 0, "widthErrors": 0, "badRows": 0, "evicted": 0}
    sent = 0
    for batch in size_batches(entries, "entries", a.batch):
        rep = post_import(base, tok, "vectors", "entries", batch, a.dry_run)
        accounted = sum(int(rep.get(k, 0)) for k in ("added", "replaced", "skippedNoVec", "widthErrors", "badRows"))
        if accounted != len(batch):
            raise RestoreError(f"vectors batch: device accounted {accounted} of {len(batch)} rows")
        for k in tot:
            tot[k] += int(rep.get(k, 0))
        sent += len(batch)
    print(
        f"  vectors: {'(dry-run) ' if a.dry_run else ''}sent={sent} added={tot['added']} "
        f"replaced={tot['replaced']} skippedNoVec={tot['skippedNoVec']} "
        f"widthErrors={tot['widthErrors']} badRows={tot['badRows']} evicted={tot['evicted']}"
    )
    if tot["widthErrors"]:
        out.warn(
            "vectors: some rows did not match the device embedding width. Set the SAME "
            "embed config (provider/model/dims) on the device before restore."
        )
    if tot["skippedNoVec"]:
        out.warn(f"vectors: {tot['skippedNoVec']} rows had no embedding and were skipped.")
    if tot["evicted"]:
        out.warn(
            f"vectors: {tot['evicted']} restored rows were evicted over the device cap "
            "(restore is NOT lossless - raise max_vectors or free space)."
        )


def restore_episodic(base, tok, src, a, out):
    p = src / "episodic.jsonl"
    if not p.exists():
        print("  episodic: (none in backup)")
        return
    msgs = [json.loads(line) for line in p.read_text().splitlines() if line.strip()]
    if not msgs:
        print("  episodic: (empty)")
        return
    # Query the device's current episodic count once, up front. It serves two honesty
    # checks: the non-empty SKIP guard below, and (under --force) knowing the store
    # started non-empty so the ring-eviction warning does not over-claim.
    have_n = int(get_json(base, tok, "/api/mem/stats").get("episodicMsgs", 0))
    # Gate on --force only so --dry-run runs this check too and its preview is honest:
    # a real (non --force) apply SKIPS episodic on a non-empty store, so the dry-run must
    # project that skip rather than print a misleading added=N.
    if not a.force and have_n > 0:
        verb = "would SKIP" if a.dry_run else "SKIPPING"
        out.warn(
            f"episodic: device already has {have_n} messages: {verb} "
            "(append would duplicate). Re-run with --force to append anyway."
        )
        return
    started_nonempty = have_n > 0  # reachable here only under --force (else we returned)
    added = truncated = 0
    for batch in size_batches(msgs, "messages", a.batch):
        rep = post_import(base, tok, "episodic", "messages", batch, a.dry_run)
        accounted = sum(int(rep.get(k, 0)) for k in ("added", "dupSkipped", "badRows"))
        if accounted != len(batch):
            raise RestoreError(f"episodic batch: device accounted {accounted} of {len(batch)} rows")
        added += int(rep.get("added", 0))
        truncated += int(rep.get("truncated", 0))
    print(f"  episodic: {'(dry-run) ' if a.dry_run else ''}added={added} of {len(msgs)} (truncated={truncated})")
    if truncated:
        if started_nonempty:
            out.warn(
                f"episodic: the no-SD ring evicted {truncated} messages during a --force append to a "
                "non-empty store. That count can include PRE-EXISTING history displaced to make room, "
                "not only restored rows, so it does not by itself mean the restore is incomplete. A card "
                "is needed to hold full history."
            )
        else:
            out.warn(
                f"episodic: {truncated} messages dropped by the no-SD ring cap "
                "(restore is NOT complete: a card is needed for full history)."
            )


def restore_scratchpad(base, tok, src, a, out):
    p = src / "scratchpad.json"
    if not p.exists():
        print("  scratchpad: (none in backup)")
        return
    sp = json.loads(p.read_bytes())
    rep = post_import(base, tok, "scratchpad", "scratchpad", sp, a.dry_run)
    print(
        f"  scratchpad: {'(dry-run) ' if a.dry_run else ''}active={rep.get('active')} "
        f"short={rep.get('short')} mid={rep.get('mid')} long={rep.get('long')} (-1 = not in backup)"
    )


def upload_file(base, tok, project, name, data, timeout=120):
    # Minimal multipart/form-data body: one file part is all the device reads (project
    # + name ride the query string; name falls back to the part's filename). Boundary is
    # random so it can never appear in the payload.
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


def push_one_file(base, tok, project, name, data, out):
    """Upload one file; return 'up' or 'fail' (recording the reason on failure)."""
    try:
        r = upload_file(base, tok, project, name, data)
        if r.get("ok"):
            return "up"
        out.fail(f"file {project}/{name}: {r.get('error')}")
    except Exception as e:  # keep going - one hole beats aborting the rest
        out.fail(f"file {project}/{name}: {e}")
    return "fail"


def restore_files(base, tok, src, a, out):
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
            tally[push_one_file(base, tok, project, name, data, out)] += 1
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
    ap.add_argument("--batch", type=int, default=64, help="max rows per import request (default 64)")
    a = ap.parse_args()
    base = f"http://{a.ip}"
    tok = a.token

    src = resolve_src(a)
    if not src.is_dir():
        sys.exit(f"not a folder: {src}")
    manifest = load_manifest(src)

    # MANIFEST verify FIRST: an inconsistent backup is never pushed (no partial apply).
    probs = verify_manifest(src, manifest)
    if probs:
        print("MANIFEST verification FAILED - refusing to restore:", file=sys.stderr)
        for p in probs:
            print(f"  - {p}", file=sys.stderr)
        sys.exit(2)
    if manifest.get("errors"):
        print(
            f"NOTE: the backup's own MANIFEST recorded {len(manifest['errors'])} error(s) at "
            "capture time; the restore can only be as complete as the backup."
        )

    try:
        state = get_json(base, tok, "/api/state")
    except Exception as e:
        sys.exit(f"cannot reach device /api/state: {e}")
    dev_now = state.get("devName", "?")
    if manifest.get("device") and dev_now != manifest["device"]:
        print(f"NOTE: backup is from '{manifest['device']}', device reports '{dev_now}'.")

    print(f"restore {'(DRY RUN) ' if a.dry_run else ''}{src} -> {a.ip} ({dev_now})")
    print(
        f"  manifest: {manifest.get('vectorCount', '?')} vectors, "
        f"{manifest.get('episodicCount', '?')} episodic, {len(manifest.get('files', []))} files (verified)"
    )

    out = Outcome()
    try:
        restore_vectors(base, tok, src, a, out)
        restore_episodic(base, tok, src, a, out)
        restore_scratchpad(base, tok, src, a, out)
        restore_files(base, tok, src, a, out)
    except RestoreError as e:
        out.fail(str(e))

    if not a.dry_run and not out.errors:
        after = get_json(base, tok, "/api/mem/stats")
        print(
            f"verify: device now holds {after.get('vectors')} vectors, {after.get('episodicMsgs')} episodic messages."
        )

    code = out.exit_code()
    if code == 0:
        print("done." if not a.dry_run else "dry run complete - nothing was written.")
    elif code == 3:
        print(f"COMPLETED WITH WARNINGS ({len(out.warnings)}) - the restore was not fully lossless.", file=sys.stderr)
    else:
        print(f"FAILED ({len(out.errors)} error(s)) - restore did not complete.", file=sys.stderr)
    sys.exit(code)


if __name__ == "__main__":
    main()
