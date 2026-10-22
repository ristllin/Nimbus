#!/usr/bin/env python3
"""Fill a bench unit's settings store (NVS) through the device's own web API, and put
it back exactly as it was.

This is how the v4.5.13 control image reaches a full settings store (it has no
``NVSFILL`` console verb): the same fillers the 14 Oct 2026 replay reproduction used,
each one restorable. Before the first write the script saves a snapshot of every
setting it will touch (``GET /api/orch`` plus the Telegram display names); ``restore``
writes the snapshot back and removes the fillers. A release candidate with the
settings reserve refuses the fillers below the reserve, which is the reserve doing its
job: storm legs on a candidate use ``NVSFILL`` instead.

Fillers, in order, each logged with the free bytes after it, until the store reports
``--target`` free bytes or fewer:

1. the directive padded to its 1,500-character limit;
2. Telegram display names on synthetic chat ids, added a few hundred bytes at a time
   (the names live in one string, and a string must fit in one storage page, so it
   grows until a write no longer lands, and that last step is taken back);
3. notes in every scratchpad tier that was empty before the fill (the scratchpad lived
   in the settings store until v4.5.14); restore clears exactly those tiers;
4. small numeric settings moved off their saved values (one entry each).

Usage (never prints the token)::

    settings_fill.py fill    --ip 192.168.50.61 --token-file unitL_webtok.txt --snapshot snap.json
    settings_fill.py restore --ip 192.168.50.61 --token-file unitL_webtok.txt --snapshot snap.json
    settings_fill.py free    --ip 192.168.50.61 --token-file unitL_webtok.txt
"""

from __future__ import annotations

import argparse
import json
import sys
import time
import urllib.error
import urllib.parse
import urllib.request
from typing import Callable, List, Optional, Tuple

PAD_ID_DIGITS = 220  # each synthetic chat id: long digits, so a name step is about 240 B
PAD_IDS = 40  # at most this many name steps
PAD_NAME = "pad"
SCRATCH_TIERS = ("long", "mid", "short")
SCRATCH_ITEM = 380  # bytes per scratchpad note
SCRATCH_ITEMS = 12  # at most this many notes per tier


def pad_id(k: int) -> str:
    """The k-th synthetic chat id of the display-name filler."""
    return str(9 - k % 9) + "9" * (PAD_ID_DIGITS - 3) + f"{k:02d}"


DIRECTIVE_MAX = 1499
DIRECTIVE_HEAD = (
    "Bench test device: answer in five words or fewer and do not use tools. Ignore the padding that follows: "
)
# Numeric settings a filler may move, with two values to alternate between. Each is a
# separate key: moving one off its default creates one entry.
DIALS: Tuple[Tuple[str, int, int], ...] = (
    ("loopMaxCnt", 8, 9),
    ("loopMinIvl", 300, 301),
    ("loopFires", 48, 49),
    ("loopTokens", 200000, 200001),
    ("loopDevTok", 100000, 100001),
    ("loopDevFir", 24, 25),
    ("capProbeH", 24, 25),
    ("loopDeadline", 900, 901),
)
SNAPSHOT_FIELDS = tuple(d[0] for d in DIALS)


class Api:
    """The unit's web API with the sign-in token in the X-Nimbus-Token header."""

    def __init__(self, ip: str, token: str, log: Optional[Callable[[str], None]] = None):
        self.base = f"http://{ip}"
        self.token = token
        self.opener = urllib.request.build_opener(urllib.request.ProxyHandler({}))
        self.log = log or (lambda s: print(s, flush=True))

    def _req(self, path: str, data: Optional[dict] = None, timeout: float = 20.0) -> Tuple[int, str]:
        body = urllib.parse.urlencode(data).encode() if data is not None else None
        req = urllib.request.Request(self.base + path, data=body, headers={"X-Nimbus-Token": self.token})
        try:
            with self.opener.open(req, timeout=timeout) as r:
                return r.status, r.read().decode("utf-8", "replace")
        except urllib.error.HTTPError as e:
            return e.code, e.read().decode("utf-8", "replace")

    def get_json(self, path: str) -> dict:
        code, text = self._req(path)
        if code != 200:
            raise RuntimeError(f"GET {path}: HTTP {code}")
        return json.loads(text)

    def post(self, path: str, data: dict) -> Tuple[int, str]:
        return self._req(path, data)

    def nvs(self) -> dict:
        return self.get_json("/api/state").get("nvs") or {}


def say(api: Api, text: str) -> None:
    api.log(time.strftime("%H:%M:%SZ ", time.gmtime()) + text.replace(api.token, "[token]"))


def take_snapshot(api: Api) -> dict:
    orch = api.get_json("/api/orch")
    names = ""
    try:
        names = str((api.get_json("/api/telegram") or {}).get("names") or "")
    except Exception:  # noqa: BLE001 - an image without the names field: the pad id is removed by id
        pass
    scratch = {}
    try:
        scratch = api.get_json("/api/mem/scratchpad")
    except Exception:  # noqa: BLE001 - no scratchpad route: that filler is skipped
        pass
    return {
        "taken": time.time(),
        "emptyTiers": [t for t in SCRATCH_TIERS if t in scratch and not scratch.get(t)],
        "directive": orch.get("directive", ""),
        "directiveDefault": orch.get("directiveDefault", ""),
        "dials": {k: orch.get(k) for k in SNAPSHOT_FIELDS if k in orch},
        "names": names,
        "nvsBefore": api.nvs(),
    }


def _step(api: Api, label: str, path: str, data: dict) -> Tuple[int, dict]:
    code, body = api.post(path, data)
    time.sleep(0.5)
    n = api.nvs()
    say(api, f"{label}: HTTP {code} -> free {n.get('free')} used {n.get('used')} fails {n.get('fails')}")
    return code, n


def _landed(before: dict, after: dict, code: int) -> bool:
    """A write landed: HTTP 200 and no new storage failure."""
    return code == 200 and int(after.get("fails", 0)) == int(before.get("fails", 0))


class _Filler:
    """One fill run: each filler step reads the store after it, and the run stops as soon
    as the store reports the target or a step no longer lands."""

    def __init__(self, api: Api, snapshot: dict, target_free: int):
        self.api, self.snapshot, self.target = api, snapshot, target_free
        self.n = api.nvs()

    def full(self) -> bool:
        return int(self.n.get("free", 0)) <= self.target

    def step(self, label: str, path: str, data: dict) -> bool:
        """One write; True when it landed (HTTP 200, no new storage failure)."""
        before = self.n
        code, self.n = _step(self.api, label, path, data)
        return _landed(before, self.n, code)

    def directive(self) -> None:
        text = DIRECTIVE_HEAD + "z" * (DIRECTIVE_MAX - len(DIRECTIVE_HEAD))
        self.step(f"directive {len(text)} B", "/api/orch", {"sysPrompt": text})

    def names(self) -> None:
        self.snapshot["padIds"] = []
        for k in range(PAD_IDS):
            if self.full():
                return
            self.snapshot["padIds"].append(pad_id(k))
            if not self.step(f"display name {k}", "/api/telegram/rename", {"id": pad_id(k), "name": PAD_NAME}):
                self.api.post("/api/telegram/rename", {"id": pad_id(k), "name": ""})
                return

    def scratchpad(self) -> None:
        for tier in self.snapshot.get("emptyTiers") or []:
            for i in range(SCRATCH_ITEMS):
                if self.full():
                    return
                text = f"bench pad {tier} {i} " + "q" * SCRATCH_ITEM
                data = {"action": "add", "tier": tier, "text": text}
                if not self.step(f"scratchpad {tier} {i}", "/api/mem/scratchpad", data):
                    break

    def dials(self, max_dials: int) -> None:
        for i in range(max_dials):
            if self.full():
                return
            key, value = dial_value(i, self.snapshot.get("dials") or {})
            if not self.step(f"dial {key}={value}", "/api/orch", {key: value}):
                say(self.api, "a one-entry write no longer lands: no room left")
                return


def dial_value(i: int, saved: dict) -> Tuple[str, int]:
    """The i-th dial write: every pass moves each dial to the other of its two values
    (the first pass never writes the saved value back)."""
    key, a, b = DIALS[i % len(DIALS)]
    value = a if (i // len(DIALS)) % 2 == 0 else b
    if i < len(DIALS) and saved.get(key) == value:
        value = b if value == a else a
    return key, value


def fill(api: Api, snapshot: dict, target_free: int = 0, max_dials: int = 64) -> dict:
    """Fill toward ``target_free`` free bytes; returns the last NVS reading. The
    snapshot must have been taken (and saved) before this is called; the fillers it
    adds are recorded in it for ``restore``."""
    f = _Filler(api, snapshot, target_free)
    say(api, f"fill start: {f.n}")
    f.directive()
    f.names()
    f.scratchpad()
    f.dials(max_dials)
    say(api, f"fill end: {f.n}")
    return f.n


def _undo_fillers(api: Api, snapshot: dict) -> List[str]:
    problems = []
    for pid in snapshot.get("padIds") or []:
        code, _ = api.post("/api/telegram/rename", {"id": pid, "name": ""})
        if code != 200:
            problems.append(f"removing a padded display name: HTTP {code}")
    for tier in snapshot.get("emptyTiers") or []:
        code, _ = api.post("/api/mem/scratchpad", {"action": "clear", "tier": tier})
        if code not in (200, 507):
            problems.append(f"clearing the scratchpad tier {tier}: HTTP {code}")
    return problems


def _write_back(api: Api, snapshot: dict) -> List[str]:
    directive = snapshot.get("directive") or ""
    if directive == snapshot.get("directiveDefault"):
        directive = ""
    writes = [("sysPrompt", directive)] + [(k, v) for k, v in (snapshot.get("dials") or {}).items() if v is not None]
    problems = []
    for key, value in writes:
        code, _ = api.post("/api/orch", {key: value})
        if code != 200:
            problems.append(f"restoring {key}: HTTP {code}")
    return problems


def _verify(api: Api, snapshot: dict) -> List[str]:
    orch = api.get_json("/api/orch")
    dials = snapshot.get("dials") or {}
    problems = [
        f"{k} reads {orch.get(k)!r}, snapshot {v!r}" for k, v in dials.items() if v is not None and orch.get(k) != v
    ]
    try:
        names = json.dumps(api.get_json("/api/telegram"))
    except Exception:  # noqa: BLE001 - an image without the names listing: not checkable
        return problems
    left = [pid for pid in snapshot.get("padIds") or [] if pid in names]
    if left:
        problems.append(f"{len(left)} padded display names are still present")
    return problems


def restore(api: Api, snapshot: dict) -> List[str]:
    """Write the snapshot back and remove the fillers. Returns the problems (empty =
    restored)."""
    problems = _undo_fillers(api, snapshot) + _write_back(api, snapshot) + _verify(api, snapshot)
    say(api, f"restore end: {api.nvs()} problems={problems}")
    return problems


def _token(path: str) -> str:
    with open(path, encoding="utf-8") as f:
        return f.read().strip()


def main(argv: Optional[List[str]] = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("action", choices=("fill", "restore", "free"))
    ap.add_argument("--ip", required=True)
    ap.add_argument("--token-file", required=True, help="file holding the unit's web sign-in token")
    ap.add_argument("--snapshot", help="snapshot file (written by fill, read by restore)")
    ap.add_argument("--target", type=int, default=0, help="stop at this many free bytes or fewer (default 0)")
    args = ap.parse_args(argv)
    api = Api(args.ip, _token(args.token_file))
    if args.action == "free":
        print(json.dumps(api.nvs()))
        return 0
    if not args.snapshot:
        ap.error("--snapshot is required for fill and restore")
    if args.action == "fill":
        snap = take_snapshot(api)
        with open(args.snapshot, "w", encoding="utf-8") as f:
            json.dump(snap, f, indent=1)
        say(api, f"snapshot saved to {args.snapshot}")
        try:
            n = fill(api, snap, target_free=args.target)
        finally:
            with open(args.snapshot, "w", encoding="utf-8") as f:
                json.dump(snap, f, indent=1)  # now with the fillers it added
        return 0 if int(n.get("free", 1 << 30)) <= args.target else 3
    with open(args.snapshot, encoding="utf-8") as f:
        snap = json.load(f)
    return 1 if restore(api, snap) else 0


if __name__ == "__main__":
    sys.exit(main())
