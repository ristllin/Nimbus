#!/usr/bin/env python3
"""Bench legs for the episodic retention guard and the index backfill (v4.5.14).

Drives one Orchestrator board WITH an SD card over HTTP (test build). No serial,
no LLM unless a leg says so. Every leg prints a JSON summary for the bench log.

    export NIMBUS_TEST_IP=<board ip> NIMBUS_TEST_TOKEN=<web token from WEBTOK?>
    python3 tests/hil/bench_episodic_retention.py state
    python3 tests/hil/bench_episodic_retention.py seed            # B1/B2 corpus
    python3 tests/hil/bench_episodic_retention.py restart-watch   # B1 + B3 (no turn)
    python3 tests/hil/bench_episodic_retention.py blobs           # B2 counts
    python3 tests/hil/bench_episodic_retention.py prune           # B2 (EPIPRUNE)
    python3 tests/hil/bench_episodic_retention.py turn-prune      # deferred during a turn
    python3 tests/hil/bench_episodic_retention.py restart-turn    # B3 with a real turn

Legs:
  seed           Write the field-shaped corpus through POST /api/test/memfill
                 kind=epiday: 16 day-streams dated 1-16 days back (newest yesterday),
                 1,346 rows, about 805 KB, every third row a tool output with its own
                 /mem/blobs sidecar. Run it on an episodic store that is otherwise
                 empty (see PR_BODY: the bench runbook erases /mem first).
  restart-watch  Restart, then sample /api/state every 250 ms until the index is
                 complete: rows the boot scan indexed, rows the backfill added, the
                 time to converge, and the lowest internal free heap and largest
                 internal block seen while it ran. Pulls the boot and completion lines
                 from the durable log.
  blobs          Page every seeded row (cold scan) and probe each referenced sidecar
                 with GET /api/mem/blob: present / missing counts.
  prune          POST /api/test/epiprune, then print the retention lines it logged.
  turn-prune     Start a real turn (POST /api/chat), queue EPIPRUNE while it runs,
                 and show "retention deferred" until the turn ends, then the pass.
  restart-turn   Restart and send one real turn while the backfill runs; sample the
                 heap throughout (B3). Compare with `turn` on a complete index.
  turn           One real turn on a complete index with heap sampling (B3 control).

The turn legs spend real provider credit (one short turn each): configure the
board with the cheapest model the program allows before running them.
"""

from __future__ import annotations

import argparse
import json
import os
import sys
import threading
import time

try:
    import requests
except ImportError:  # pragma: no cover
    sys.exit("pip install requests")

# The field card's shape: (days back, rows, bytes). The eleven oldest files are
# small; the 14 Oct file is the heavy day; 15 Oct straddles the boot window.
FIELD_DAYS = [(16 - i, 26, 11941) for i in range(10)] + [
    (6, 26, 11944),
    (5, 494, 308748),
    (4, 297, 175580),
    (3, 106, 67375),
    (2, 105, 75728),
    (1, 58, 46763),
]
ROW_OVERHEAD = 120  # JSON keys, id, session and timestamp around the text
BLOB_EVERY = 3
# Rows per call. The seam allows 200, but every third row also creates a sidecar
# file, and the handler runs on the web server's task: keep each call short.
SEED_CHUNK = 60


def _env():
    ip = os.environ.get("NIMBUS_TEST_IP")
    tok = os.environ.get("NIMBUS_TEST_TOKEN")
    if not ip or not tok:
        sys.exit("set NIMBUS_TEST_IP and NIMBUS_TEST_TOKEN")
    return ip, tok


# One session for every request, carrying the web token as the X-Nimbus-Token
# header: the device rejects a ?t= query token, and a token in a URL ends up in
# tracebacks and logs. Set by main().
_S = requests.Session()


def _u(ip, path):
    return f"http://{ip}{path}"


def state(ip):
    return _S.get(_u(ip, "/api/state"), timeout=10).json()


def stats(ip):
    return _S.get(_u(ip, "/api/mem/stats"), timeout=15).json()


def log_tail(ip, nbytes=24576):
    r = _S.get(_u(ip, f"/api/errlog/recent?bytes={nbytes}"), timeout=15)
    return r.text if r.status_code == 200 else ""


def log_lines(ip, needles, nbytes=24576):
    return [ln for ln in log_tail(ip, nbytes).splitlines() if any(n in ln for n in needles)]


def summary_of(st, ms):
    mem = st.get("mem", {})
    return {
        "indexComplete": st.get("epiIndexComplete"),
        "bootIndexed": ms.get("epiBootIndexed"),
        "backfillRows": ms.get("epiBackfillRows"),
        "episodicMsgs": ms.get("episodicMsgs"),
        "heapMin": st.get("heapMin"),
        "intFree": mem.get("intFree"),
        "intLargest": mem.get("intLargest"),
    }


def cmd_state(ip, _args):
    print(json.dumps(summary_of(state(ip), stats(ip)), indent=2))


def cmd_seed(ip, _args):
    st = state(ip)
    if not st.get("memSd"):
        sys.exit("no SD card: the corpus lives on the card")
    epoch = int(st.get("clock", {}).get("epoch", 0))
    if not st.get("clock", {}).get("synced") or epoch < 1_600_000_000:
        sys.exit("device clock not set: dated rows need a real date")
    today = epoch // 86400
    written, blobs = 0, 0
    for back, rows, nbytes in FIELD_DAYS:
        day = today - back
        per_row = max(64, nbytes // rows - ROW_OVERHEAD)
        left = rows
        while left > 0:
            n = min(SEED_CHUNK, left)
            body = {"kind": "epiday", "day": str(day), "n": str(n), "bytes": str(per_row), "blob": str(BLOB_EVERY)}
            r = _S.post(_u(ip, "/api/test/memfill"), data=body, timeout=120)
            if r.status_code == 404:
                sys.exit("/api/test/memfill absent: flash the test build")
            r.raise_for_status()
            got = int(r.json()["added"])
            if got != n:
                sys.exit(f"day {day}: asked for {n} rows, the seam wrote {got}")
            written += got
            blobs += (n + BLOB_EVERY - 1) // BLOB_EVERY
            left -= n
            time.sleep(1.5)  # SD append + watchdog headroom
    print(json.dumps({"rowsWritten": written, "sidecarsWritten": blobs, "days": len(FIELD_DAYS)}, indent=2))


def _restart(ip, timeout=150):
    try:
        _S.post(_u(ip, "/api/test/reboot"), timeout=5)
    except requests.RequestException:
        pass
    time.sleep(6)
    deadline = time.time() + timeout
    while time.time() < deadline:
        try:
            if state(ip).get("mode") == 1:
                return True
        except requests.RequestException:
            pass
        time.sleep(0.5)
    return False


class Sampler(threading.Thread):
    """Samples /api/state every `period` s: index completeness and internal heap."""

    def __init__(self, ip, period=0.25):
        super().__init__(daemon=True)
        self.ip, self.period = ip, period
        self.stop = threading.Event()
        self.samples = 0
        self.min_int_free = None
        self.min_int_largest = None
        self.heap_min = None
        self.complete_at = None
        self.t0 = time.time()

    def run(self):
        while not self.stop.is_set():
            try:
                st = state(self.ip)
                mem = st.get("mem", {})
                self.samples += 1
                for attr, val in (("min_int_free", mem.get("intFree")), ("min_int_largest", mem.get("intLargest"))):
                    if val is None:
                        continue
                    cur = getattr(self, attr)
                    if cur is None or val < cur:
                        setattr(self, attr, val)
                self.heap_min = st.get("heapMin", self.heap_min)
                if st.get("epiIndexComplete") and self.complete_at is None:
                    self.complete_at = time.time() - self.t0
            except requests.RequestException:
                pass
            time.sleep(self.period)


def _watch_until_complete(ip, sampler, timeout):
    deadline = time.time() + timeout
    while time.time() < deadline and sampler.complete_at is None:
        time.sleep(0.5)
    sampler.stop.set()
    sampler.join(timeout=5)


def _report(ip, sampler, extra=None):
    ms = stats(ip)
    out = {
        "convergedAfterS": None if sampler.complete_at is None else round(sampler.complete_at, 1),
        "samples": sampler.samples,
        "heapMin": sampler.heap_min,
        "minIntFree": sampler.min_int_free,
        "minIntLargest": sampler.min_int_largest,
        "bootIndexed": ms.get("epiBootIndexed"),
        "backfillRows": ms.get("epiBackfillRows"),
        "episodicMsgs": ms.get("episodicMsgs"),
        "indexComplete": ms.get("epiIndexComplete"),
        "log": log_lines(ip, ["episodic boot scan", "episodic index", "retention"]),
    }
    if extra:
        out.update(extra)
    print(json.dumps(out, indent=2))
    return out


def cmd_restart_watch(ip, args):
    if not _restart(ip):
        sys.exit("board did not come back")
    s = Sampler(ip)
    s.start()
    _watch_until_complete(ip, s, args.timeout)
    _report(ip, s)


def _turn(ip, text, timeout=180):
    t0 = time.time()
    r = _S.post(_u(ip, "/api/chat"), data={"text": text}, timeout=20)
    r.raise_for_status()
    turn = r.json().get("turn")
    while time.time() - t0 < timeout:
        g = _S.get(_u(ip, f"/api/chat?turn={turn}"), timeout=10).json()
        if not g.get("pending") and g.get("reply"):
            return {"turnS": round(time.time() - t0, 1), "replyChars": len(g.get("reply", ""))}
        time.sleep(1)
    return {"turnS": None, "replyChars": 0}


TURN_TEXT = "Reply with one short sentence: what is two plus two?"


def cmd_restart_turn(ip, args):
    if not _restart(ip):
        sys.exit("board did not come back")
    s = Sampler(ip)
    s.start()
    turn = _turn(ip, TURN_TEXT)
    _watch_until_complete(ip, s, args.timeout)
    _report(ip, s, {"turn": turn, "leg": "backfill-with-turn"})


def cmd_turn(ip, args):
    s = Sampler(ip)
    s.start()
    turn = _turn(ip, TURN_TEXT)
    time.sleep(3)
    s.stop.set()
    s.join(timeout=5)
    _report(ip, s, {"turn": turn, "leg": "control-no-backfill"})


def _seeded_blob_paths(ip, limit_pages=400):
    paths, before = [], ""
    for _ in range(limit_pages):
        q = f"/api/mem/episodic?session=hiltest-epi&cold=1&limit=200&before={requests.utils.quote(before)}"
        page = _S.get(_u(ip, q), timeout=30).json()
        for m in page.get("messages", []):
            if m.get("blob"):
                paths.append(m["blob"])
        nxt = page.get("nextBefore") or ""
        if not nxt or nxt == before:
            break
        before = nxt
    return sorted(set(paths))


def cmd_blobs(ip, _args):
    paths = _seeded_blob_paths(ip)
    present = missing = 0
    for p in paths:
        r = _S.get(_u(ip, f"/api/mem/blob?path={requests.utils.quote(p)}"), timeout=15)
        if r.status_code == 200:
            present += 1
        elif r.status_code == 404:
            missing += 1
    print(json.dumps({"referenced": len(paths), "present": present, "missing": missing}, indent=2))


def cmd_prune(ip, args):
    r = _S.post(_u(ip, "/api/test/epiprune"), timeout=10)
    if r.status_code == 404:
        sys.exit("/api/test/epiprune absent: this build has no EPIPRUNE seam")
    time.sleep(args.wait)
    print(json.dumps({"queued": r.status_code == 202, "log": log_lines(ip, ["retention"])}, indent=2))


def cmd_turn_prune(ip, args):
    t0 = time.time()
    r = _S.post(_u(ip, "/api/chat"), data={"text": TURN_TEXT}, timeout=20)
    r.raise_for_status()
    turn = r.json().get("turn")
    time.sleep(1.0)  # the turn is in flight
    _S.post(_u(ip, "/api/test/epiprune"), timeout=10)
    during = []
    while time.time() - t0 < 180:
        g = _S.get(_u(ip, f"/api/chat?turn={turn}"), timeout=10).json()
        if not during:
            during = log_lines(ip, ["retention deferred"])
        if not g.get("pending"):
            break
        time.sleep(1)
    time.sleep(args.wait)
    print(
        json.dumps(
            {
                "deferredSeenDuringTurn": bool(during),
                "duringTurn": during,
                "afterTurn": log_lines(ip, ["retention"]),
            },
            indent=2,
        )
    )


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument(
        "leg", choices=["state", "seed", "restart-watch", "blobs", "prune", "turn-prune", "restart-turn", "turn"]
    )
    ap.add_argument("--timeout", type=float, default=300.0, help="seconds to wait for the index to complete")
    ap.add_argument("--wait", type=float, default=4.0, help="seconds to let a queued pass run before reading the log")
    args = ap.parse_args()
    ip, tok = _env()
    _S.headers["X-Nimbus-Token"] = tok
    {
        "state": cmd_state,
        "seed": cmd_seed,
        "restart-watch": cmd_restart_watch,
        "blobs": cmd_blobs,
        "prune": cmd_prune,
        "turn-prune": cmd_turn_prune,
        "restart-turn": cmd_restart_turn,
        "turn": cmd_turn,
    }[args.leg](ip, args)


if __name__ == "__main__":
    main()
