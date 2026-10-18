#!/usr/bin/env python3
"""tg_archive_import.py - put a folder of backed-up media sidecars into the Telegram archive.

The restore path for a device whose old media store lost files (the retention pass
that deleted blob sidecars after a truncated boot). A backup folder holds the
sidecars by content hash (`<16 hex>.<ext>`: photos, voice notes, tool-output text).
This tool places each one into the device's Telegram archive on the SD card:

    /mem/tg/<chat>/<yyyy-mm-dd>/r<hash>.<ext>   plus one index line (dir "restored")

The chat and day come from the memory row that referenced the sidecar (pass the
rows with --rows: a JSON array of episodic rows, an object with a "rows" array, or
JSON lines; each row has "blob", "session", "ts" in hours, "kind", "role"). A
sidecar no row names goes under chat "restored" on the backup's day.

Safe by default:
  * a dry run unless --write is given, and --write also needs --owner-approved
    (restoring onto a device is the owner's call);
  * a rerun skips files already on the device (the device never overwrites);
  * --verify reads the archive back and lists anything missing;
  * the device token is read from NIMBUS_TOKEN or --token-file and never printed;
  * only counts and hash names are printed, never file contents.

Exit codes: 0 clean, 2 hard error, 3 finished with files missing or failed.

Usage:
  NIMBUS_TOKEN=... python3 tools/tg_archive_import.py --ip 192.0.2.10 \\
      --backup ~/NimbusBackups/Lumi/2026-10-18_blobs --rows rows.json            # plan
  NIMBUS_TOKEN=... python3 tools/tg_archive_import.py --ip 192.0.2.10 \\
      --backup ... --rows rows.json --write --owner-approved                    # restore
  NIMBUS_TOKEN=... python3 tools/tg_archive_import.py --ip 192.0.2.10 \\
      --backup ... --rows rows.json --verify                                    # check
"""

from __future__ import annotations

import argparse
import collections
import datetime as dt
import json
import os
import pathlib
import re
import sys
import time
import urllib.error
import urllib.parse
import urllib.request
from dataclasses import dataclass

MAX_BYTES = 20 * 1024 * 1024  # the device's per-file import limit
BLOB_RE = re.compile(r"^([0-9a-f]{16})\.([a-z0-9]{1,5})$")
ROW_BLOB_RE = re.compile(r"/mem/blobs/([0-9a-f]{16})\.([a-z0-9]{1,5})$")
CHAT_RE = re.compile(r"^-?[0-9]{1,20}$")
DAY_RE = re.compile(r"^(\d{4})-(\d{2})-(\d{2})")
RESTORED_CHAT = "restored"
KIND_BY_ROW = {"image": "photo", "audio": "voice", "file": "document"}
KIND_BY_EXT = {"jpg": "photo", "jpeg": "photo", "png": "photo", "webp": "photo", "ogg": "voice", "mp3": "audio"}


@dataclass
class Item:
    path: pathlib.Path
    chat: str
    day: str
    name: str
    kind: str
    role: str
    date: int
    size: int


def load_rows(path: str | None) -> list[dict]:
    """Episodic rows from a JSON array, an object with "rows", or JSON lines."""
    if not path:
        return []
    text = pathlib.Path(path).read_text(encoding="utf-8")
    try:
        data = json.loads(text)
    except json.JSONDecodeError:
        data = [json.loads(line) for line in text.splitlines() if line.strip()]
    if isinstance(data, dict):
        # {"rows": [...]}, or a single row (a one-line JSON-lines file)
        data = data["rows"] if isinstance(data.get("rows"), list) else [data]
    return [r for r in data if isinstance(r, dict)]


def ts_hours(row: dict) -> int:
    try:
        return int(str(row.get("ts", "0")))
    except ValueError:
        return 0


def blob_index(rows: list[dict]) -> dict[str, dict]:
    """hash -> the earliest row that references /mem/blobs/<hash>.<ext>."""
    out: dict[str, dict] = {}
    for r in rows:
        m = ROW_BLOB_RE.search(str(r.get("blob", "")))
        if not m:
            continue
        h = m.group(1)
        if h not in out or ts_hours(r) < ts_hours(out[h]):
            out[h] = r
    return out


def day_of(epoch: int) -> str:
    # 2015-01-01 .. 2200-01-01, the device's own plausibility window.
    if epoch < 1420070400 or epoch >= 7258118400:
        return "undated"
    return dt.datetime.fromtimestamp(epoch, dt.timezone.utc).strftime("%Y-%m-%d")


def backup_day(folder: pathlib.Path) -> str:
    m = DAY_RE.match(folder.name)
    return f"{m.group(1)}-{m.group(2)}-{m.group(3)}" if m else "undated"


def place(path: pathlib.Path, row: dict | None, default_day: str) -> Item:
    h, ext = BLOB_RE.match(path.name).groups()
    chat, day, kind, role, date = RESTORED_CHAT, default_day, KIND_BY_EXT.get(ext, "text"), "", 0
    if row:
        session = str(row.get("session", ""))
        chat = session if CHAT_RE.match(session) else RESTORED_CHAT
        date = ts_hours(row) * 3600
        day = day_of(date) if date else default_day
        row_kind = str(row.get("kind", ""))
        kind = KIND_BY_ROW.get(row_kind, row_kind or kind)
        role = str(row.get("role", ""))[:12]
    return Item(path, chat, day, f"r{h}.{ext}", kind[:24], role, date, path.stat().st_size)


def plan(backup: pathlib.Path, rows: list[dict]) -> tuple[list[Item], list[str]]:
    """Items to restore, plus the names skipped (not a sidecar, empty or too big)."""
    index = blob_index(rows)
    default_day = backup_day(backup)
    items, skipped = [], []
    for p in sorted(backup.iterdir()):
        m = BLOB_RE.match(p.name) if p.is_file() else None
        if not m or not 0 < p.stat().st_size <= MAX_BYTES:
            skipped.append(p.name)
            continue
        items.append(place(p, index.get(m.group(1)), default_day))
    return items, skipped


def summarize(items: list[Item]) -> str:
    by = collections.Counter((it.chat, it.day) for it in items)
    return "\n".join(f"  {chat} / {day}: {n} file(s)" for (chat, day), n in sorted(by.items()))


class Device:
    """The device's archive routes over the LAN (token on the header only)."""

    def __init__(self, base: str, token: str, timeout: int = 120):
        self.base, self.token, self.timeout = base.rstrip("/"), token, timeout

    def _open(self, path: str, data: bytes | None = None):
        headers = {"X-Nimbus-Token": self.token}
        if data is not None:
            headers["Content-Type"] = "application/octet-stream"
        req = urllib.request.Request(
            self.base + path, data=data, headers=headers, method="POST" if data is not None else "GET"
        )
        return urllib.request.urlopen(req, timeout=self.timeout)

    def upload(self, it: Item, note: str) -> dict:
        q = urllib.parse.urlencode(
            {
                "chat": it.chat,
                "day": it.day,
                "name": it.name,
                "kind": it.kind,
                "role": it.role,
                "date": it.date,
                "note": note,
            }
        )
        with self._open("/api/tg/archive/import?" + q, it.path.read_bytes()) as r:
            return json.loads(r.read() or b"{}")

    def restored_files(self, chat: str, day: str) -> set[str]:
        names, start = set(), 0
        while True:
            q = urllib.parse.urlencode({"chat": chat, "day": day, "from": start, "limit": 200})
            with self._open("/api/tg/archive?" + q) as r:
                page = json.loads(r.read())
            names.update(row.get("file", "") for row in page.get("rows", []) if row.get("dir") == "restored")
            if not page.get("more"):
                return names
            start = page.get("next", start + 200)


def upload_with_retry(dev: Device, it: Item, note: str, sleep=time.sleep) -> dict:
    """One upload; a busy device (429) is retried. Errors come back as {"error": ...}."""
    for attempt in range(8):
        try:
            return dev.upload(it, note)
        except urllib.error.HTTPError as e:
            if e.code == 429 and attempt < 7:  # another upload, or the device catching up
                sleep(2.0)
                continue
            return {"error": f"HTTP {e.code}"}
        except (urllib.error.URLError, OSError, ValueError) as e:  # ValueError: a reply that is not JSON
            return {"error": str(getattr(e, "reason", e))}
    return {"error": "device busy"}


def push(dev: Device, items: list[Item], note: str, sleep=time.sleep) -> tuple[int, int, list[str]]:
    """Upload each item. Returns (written, skipped, failures)."""
    written = skipped = 0
    failures: list[str] = []
    for it in items:
        reply = upload_with_retry(dev, it, note, sleep)
        if reply.get("ok") and reply.get("skipped"):
            skipped += 1
        elif reply.get("ok"):
            written += 1
        else:
            failures.append(f"{it.name}: {reply.get('error', 'no reply')}")
    return written, skipped, failures


def missing_now(dev: Device, items: list[Item]) -> list[str]:
    missing = []
    for chat, day in sorted({(it.chat, it.day) for it in items}):
        have = dev.restored_files(chat, day)
        missing += [it.name for it in items if (it.chat, it.day) == (chat, day) and it.name not in have]
    return missing


def verify(dev: Device, items: list[Item], wait_s: float = 0, sleep=time.sleep) -> list[str]:
    """Names planned but not recorded in the archive. The device writes an index line
    on its next poll cycle (up to about 30 s), so after a write this waits up to wait_s."""
    waited = 0.0
    missing = missing_now(dev, items)
    while missing and waited < wait_s:
        sleep(5.0)
        waited += 5.0
        missing = missing_now(dev, items)
    return missing


def read_token(args) -> str:
    if args.token_file:
        return pathlib.Path(args.token_file).read_text(encoding="utf-8").strip()
    return os.environ.get("NIMBUS_TOKEN", "").strip()


def parse_args(argv):
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--ip", help="device address (or a full http:// URL)")
    ap.add_argument("--backup", required=True, help="folder of <16 hex>.<ext> sidecars")
    ap.add_argument(
        "--rows",
        action="append",
        default=[],
        help="episodic rows that reference the sidecars (JSON or JSON lines); repeat to merge several",
    )
    ap.add_argument("--token-file", help="file holding the device sign-in token (else NIMBUS_TOKEN)")
    ap.add_argument("--note", default="", help="where the files came from (default: the backup folder name)")
    ap.add_argument("--write", action="store_true", help="upload (default: plan only)")
    ap.add_argument("--owner-approved", action="store_true", help="required with --write")
    ap.add_argument("--verify", action="store_true", help="read the archive back and list what is missing")
    return ap.parse_args(argv)


def refusal(args) -> str:
    """Why this run may not touch the device ("" when it may)."""
    if args.write and not args.owner_approved:
        return "--write needs --owner-approved (restoring onto a device is the owner's call)"
    if not args.ip or not read_token(args):
        return "--ip and a token (NIMBUS_TOKEN or --token-file) are required"
    return ""


def run_write(dev: Device, items: list[Item], note: str) -> int:
    written, skipped_n, failures = push(dev, items, note)
    print(f"write: {written} uploaded, {skipped_n} already there, {len(failures)} failed")
    for f in failures:
        print(f"  failed: {f}", file=sys.stderr)
    return 3 if failures else 0


def run_verify(dev: Device, items: list[Item], wait_s: float) -> int:
    missing = verify(dev, items, wait_s=wait_s)
    print(f"verify: {len(items) - len(missing)} of {len(items)} recorded")
    for name in missing:
        print(f"  missing: {name}", file=sys.stderr)
    return 3 if missing else 0


def main(argv=None) -> int:
    args = parse_args(argv)
    backup = pathlib.Path(args.backup).expanduser()
    if not backup.is_dir():
        print(f"backup folder not found: {backup}", file=sys.stderr)
        return 2
    items, skipped = plan(backup, [r for path in args.rows for r in load_rows(path)])
    mapped = sum(1 for it in items if it.chat != RESTORED_CHAT)
    print(f"plan: {len(items)} file(s), {mapped} placed by their memory row, {len(skipped)} not sidecars")
    print(summarize(items))
    if not (args.write or args.verify):
        return 0
    why = refusal(args)
    if why:
        print(f"refused: {why}", file=sys.stderr)
        return 2
    dev = Device(args.ip if args.ip.startswith("http") else f"http://{args.ip}", read_token(args))
    code = run_write(dev, items, (args.note or backup.name)[:40]) if args.write else 0
    if args.verify:  # reported even after a failed write: it lists what is still missing
        code = max(code, run_verify(dev, items, 90 if args.write else 0))
    return code


if __name__ == "__main__":
    sys.exit(main())
