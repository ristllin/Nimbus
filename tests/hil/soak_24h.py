#!/usr/bin/env python3
"""The 24 h soak driver (bench row K01), for the release image or a test image.

It keeps the raw record on disk as it goes, so the grade can be recomputed from the files and nothing rests on a
person watching: every poll's numbers in polls.jsonl (heap, heapMin, largest block, low water, boot count, crash
state, failed allocations, poll latency), the whole /api/state body every --raw-s under polls/, every turn's outcome
in turns.jsonl, every durable log read under errlog/, and with a console the console log.

    soak_24h.py --ip IP --token-file F --out DIR --expect-build v5.0.0            # the release image: LAN only
    soak_24h.py --ip IP --token-file F --out DIR --port P --mac MAC --chat ID     # a test image: adds photo albums
    soak_24h.py --ip IP --token-file F --out DIR --poll-only --hours 6            # just the monitor, no turns
    soak_24h.py --grade DIR                                                       # grade a run again from its files

The load: one text turn and one food turn an hour, a 3-photo album every two hours when a console is held (TGINJECT
exists only on a test image, so a release-image run says the albums were not driven). The unit's own dream and
routines are left to fire. Stop with SIGINT or by creating DIR/STOP; a partial run then grades BLOCKED, not PASS.
--resume continues a run in DIR after a host restart, keeping its start.

The scanner counts failed allocations in any case (bench_driver.scan_lines). The 28 Oct soak's scanner looked for
lower-case words and reported zero over 24 of these lines.

Grades: FAIL (a restart, a new crash, low water under 8 KB after the first hour, a low-memory turn failure, a failed
or slow turn, slow or failed polls, the settings store shrinking), BLOCKED (too short, a hole in the polls, a missing
number, no dream, a load that never ran), OPEN (it ran clean but memory failures were counted, for a person to read),
else PASS.
"""

from __future__ import annotations

import argparse
import collections
import glob
import json
import os
import signal
import statistics
import sys
import threading
import time
from typing import Any, Dict, List, Optional, Tuple

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import bench_driver as bd
import port_guard

MIN_LOW_WATER = 8000
SETTLE_S = 3600.0  # the low-water criterion starts after the first hour
TEXT_TURN_LIMIT_S = 30.0
POLL_MEDIAN_LIMIT_S = 0.3
POLL_MAX_LIMIT_S = 5.0
NVS_FLAT_TOLERANCE_B = 96  # three entries: a counter rewritten mid-page moves this much and back
MAX_POLL_GAP_S = 900.0  # a longer hole in the polls means the run was not watched
DURATION_SLACK = 0.99
TEXT_PROMPT = "What is 17 times 23? Answer with just the number."
FOOD_PROMPT = "Lunch: grilled chicken breast and rice, about 640 kcal."
ALBUM_PROMPT = "What do these photos show? One short line."
TURN_TIMEOUT_S = 90.0
ALBUM_TIMEOUT_S = 180.0
DREAM_RAN = "ran"  # the outcome slug of a night that did its work (lib/harness dream.cpp outcomeSlug)
DREAM_UNPROVEN = ("skipped", "deferred")  # the night did not run, so nothing was proven either way


def metrics(state: Dict[str, Any]) -> Dict[str, Any]:
    """The numbers one poll keeps, from an /api/state body. Missing fields stay None: the grade reads them as not
    measured, never as fine."""
    mem = state.get("mem") if isinstance(state.get("mem"), dict) else {}
    boot = state.get("boot") if isinstance(state.get("boot"), dict) else {}
    crash = boot.get("crash") if isinstance(boot.get("crash"), dict) else {}
    nvs = state.get("nvs") if isinstance(state.get("nvs"), dict) else {}
    dream = state.get("dream") if isinstance(state.get("dream"), dict) else {}
    return {
        "build": state.get("build"),
        "uptimeMs": state.get("uptimeMs"),
        "bootN": boot.get("n"),
        "bootReason": boot.get("reason"),
        "crashFp": crash.get("fp"),
        "crashNew": crash.get("new"),
        "crashThisReset": crash.get("thisReset"),
        "heap": state.get("heap"),
        "heapMin": state.get("heapMin"),
        "intFree": mem.get("intFree"),
        "intLargest": mem.get("intLargest"),
        "lowWater": mem.get("lowWater"),
        "lowWaterTags": mem.get("lowWaterTags"),
        "allocFails": mem.get("allocFails"),
        "allocFailLast": mem.get("allocFailLast"),
        "pollStackMin": mem.get("pollStackMin"),
        "nvsFree": nvs.get("free"),
        "nvsFails": nvs.get("fails"),
        "dreamKnown": dream.get("known"),
        "dreamLast": dream.get("last"),
        "dreamStateSaved": dream.get("stateSaved"),
    }


# ---- the run ------------------------------------------------------------------------------------


class Soak:
    """The run's moving parts. The poller and the load run on separate threads so a slow turn never holes the polls."""

    def __init__(self, args, api: bd.Api, console=None, album=None):
        self.args, self.api, self.console, self.album = args, api, console, album
        self.ev = bd.Evidence(args.out, api.scrub)
        self.stop = threading.Event()
        self.start = time.time()
        self.last_raw = 0.0

    def stopped(self) -> bool:
        return self.stop.is_set() or os.path.exists(self.ev.path("STOP"))

    def duration_s(self) -> float:
        return self.args.hours * 3600.0

    # -- polls ----------------------------------------------------------------------------------
    def poll_once(self) -> None:
        reply = self.api.get("/api/state", self.args.poll_timeout)
        row: Dict[str, Any] = {"t": round(time.time(), 2), "http": reply.code, "s": round(reply.seconds, 3)}
        if reply.error:
            row["err"] = reply.error
        if reply.code == 200:
            row.update(metrics(reply.json()))
            if time.time() - self.last_raw >= self.args.raw_s:
                self.last_raw = time.time()
                self.ev.write(f"polls/state-{bd.utc_stamp()}.json", reply.text)
        self.ev.append("polls.jsonl", json.dumps(row, separators=(",", ":")))

    def poller(self) -> None:
        while not self.stopped() and time.time() - self.start < self.duration_s():
            self.poll_once()
            self.stop.wait(self.args.poll_s)

    # -- the load -------------------------------------------------------------------------------
    def record_turn(self, kind: str, result: bd.TurnResult) -> None:
        row = {
            "t": round(time.time(), 2),
            "kind": kind,
            "ok": result.ok,
            "s": round(result.seconds, 1),
            "http": result.http,
            "error": result.error[:200],
            "reply": result.reply[:200],
        }
        self.ev.append("turns.jsonl", json.dumps(row))
        self.ev.note(f"turn {kind}: {'ok' if result.ok else 'FAILED'} in {result.seconds:.1f} s {result.error[:120]}")

    def text_turn(self) -> None:
        self.record_turn("text", bd.chat_turn(self.api, TEXT_PROMPT, TURN_TIMEOUT_S, self.args.turn_poll_s))

    def food_turn(self) -> None:
        self.record_turn("food", bd.chat_turn(self.api, FOOD_PROMPT, TURN_TIMEOUT_S, self.args.turn_poll_s))

    def album_turn(self) -> None:
        self.record_turn("album", self.album())

    def read_errlog(self) -> None:
        reply = self.api.get("/api/errlog", 60.0)
        if reply.code == 200:
            self.ev.write(f"errlog/errlog-{bd.utc_stamp()}.txt", reply.text)
        else:
            self.ev.note(f"errlog read failed: {reply.code} {reply.error}")

    def tasks(self) -> List[Tuple[str, float]]:
        a = self.args
        rows = [("read_errlog", a.errlog_s)]
        if not a.poll_only:
            rows += [("text_turn", a.text_every), ("food_turn", a.food_every)]
            if self.album:
                rows.append(("album_turn", a.album_every))
        return rows

    def load(self) -> None:
        tasks = self.tasks()
        due = {name: self.start + every if name == "read_errlog" else self.start for name, every in tasks}
        while not self.stopped() and time.time() - self.start < self.duration_s():
            for name, every in tasks:
                if time.time() >= due[name] and not self.stopped():
                    due[name] = time.time() + every
                    getattr(self, name)()
            self.stop.wait(min(1.0, self.args.poll_s))

    # -- start and end --------------------------------------------------------------------------
    def begin(self) -> Optional[str]:
        """Write or read start.json. Returns why the run must not start, or None."""
        start_path = self.ev.path("start.json")
        if self.args.resume and os.path.exists(start_path):
            with open(start_path, encoding="utf-8") as f:
                self.start = json.load(f)["t"]
            self.ev.note(f"resumed: the run started at {bd.utc_iso(self.start)}")
            return None
        if os.path.exists(start_path):
            return f"{self.args.out} already holds a run; pass --resume or use a new folder"
        state = self.api.state(self.args.poll_timeout)
        if not state:
            return f"no answer from {self.api.ip}"
        build = str(state.get("build") or "")
        if self.args.expect_build and not build.startswith(self.args.expect_build):
            return f"the unit runs {build!r}, not {self.args.expect_build!r}"
        plan = {
            "t": self.start,
            "at": bd.utc_iso(self.start),
            "hours": self.args.hours,
            "pollOnly": self.args.poll_only,
            "albums": bool(self.album),
            "console": bool(self.console),
            "first": metrics(state),
        }
        self.ev.write("start.json", plan)
        self.ev.note(f"start: {json.dumps(plan['first'])}")
        return None

    def finish(self) -> Dict[str, Any]:
        self.poll_once()  # the last number on record is the end of the run, whatever the poll interval
        self.read_errlog()
        state = self.api.state(self.args.poll_timeout)
        self.ev.write("dream.json", state.get("dream") or {})
        self.ev.write("state-final.json", state or {})
        verdict = grade_dir(self.args.out)
        self.ev.write("verdict.json", verdict)
        self.ev.write("summary.md", summary_text(verdict))
        return verdict


def run_soak(args, api: bd.Api, console=None, album=None) -> Dict[str, Any]:
    soak = Soak(args, api, console, album)
    refused = soak.begin()
    if refused:
        soak.ev.note(f"not started: {refused}")
        return {"grade": "BLOCKED", "reasons": {"BLOCKED": [refused]}}
    threads = [threading.Thread(target=soak.poller, daemon=True), threading.Thread(target=soak.load, daemon=True)]
    for thread in threads:
        thread.start()
    signal_stop = lambda *_: soak.stop.set()  # noqa: E731
    if threading.current_thread() is threading.main_thread():
        signal.signal(signal.SIGINT, signal_stop)
        signal.signal(signal.SIGTERM, signal_stop)
    for thread in threads:
        while thread.is_alive():
            thread.join(0.5)
    return soak.finish()


# ---- albums, on a test image ------------------------------------------------------------------------


def stage_album(api: bd.Api, ev: bd.Evidence, count: int = 3) -> List[Tuple[str, int]]:
    """Put the album's photos on the card before any ballast: a large upload needs memory."""
    import tg_archive_bench as tb

    photos = []
    for i in range(count):
        data = tb.noise_jpeg(480, 360, 1000 + i)
        name = f"soak-{i}.jpg"
        reply = api.upload("soak", name, data)
        ev.note(f"staged {name}: http {reply.code}, {len(data)} B")
        if reply.code == 200:
            photos.append((name, len(data)))
    return photos


def make_owner_chat(api: bd.Api, chat: int) -> str:
    """Put the album's chat first on the allowlist and return the list to put back. It stays in memory, never in the
    evidence (it holds real chat ids). A bot token on the unit would poll the real bot, so that is refused."""
    orch = api.get_json("/api/orch")
    if orch.get("hasTg"):
        raise RuntimeError("a Telegram bot token is on the unit: clear it before an album run")
    before = str(orch.get("tgAllow") or "")
    api.post_form("/api/orch", {"tgAllow": str(chat)})
    return before


def album_runner(console, chat: int, photos: List[Tuple[str, int]]):
    """A callable that injects one album from the owner chat and waits for its turn. It must be the owner's chat: a
    member's 8 MB files quota ended the 28 Oct album run after about 15 cycles."""
    import tg_archive_bench as tb

    update = tb.first_update_id()

    def run() -> bd.TurnResult:
        nonlocal update
        started = time.time()
        updates = [
            tb.local_photo_update(update + i, chat, f"/mem/files/soak/{name}", size, group=f"soak{update}")
            for i, (name, size) in enumerate(photos)
        ]
        update += 100
        console.send("TGINJECT " + tb.page(updates))
        ended = console.wait("turn end:", ALBUM_TIMEOUT_S, since=started)
        ok = bool(ended) and "turn end: ok" in ended.lower()
        return bd.TurnResult(ok, ended[:200], "" if ok else ended[:200] or "no turn end", time.time() - started, 0)

    return run


# ---- the grade, from the files alone -------------------------------------------------------------


def load_jsonl(path: str) -> List[Dict[str, Any]]:
    rows = []
    if os.path.exists(path):
        with open(path, encoding="utf-8") as f:
            for line in f:
                try:
                    rows.append(json.loads(line))
                except ValueError:
                    pass  # a line cut by a power loss on the host
    return rows


def _read_lines(paths: List[str]) -> List[str]:
    lines: List[str] = []
    for path in paths:
        with open(path, encoding="utf-8", errors="replace") as f:
            lines.extend(f.read().splitlines())
    return lines


def scan_run(out: str) -> Dict[str, Any]:
    """The scanner over everything the run saved. The durable log is read many times and each read overlaps the last,
    so a line counts as often as the read that holds it most (two equal lines stay two, and a line in ten reads stays
    one). The console log is one stream. The headline is the larger of the two, because a line can reach one and not
    the other."""
    held: collections.Counter = collections.Counter()
    for path in sorted(glob.glob(os.path.join(out, "errlog", "*.txt"))):
        held |= collections.Counter(_read_lines([path]))
    errlog = list(held.elements())
    console = _read_lines(
        [os.path.join(out, "console.log")] if os.path.exists(os.path.join(out, "console.log")) else []
    )
    from_errlog, from_console = bd.scan_lines(errlog), bd.scan_lines(console)
    return {
        "errlog": from_errlog,
        "console": from_console,
        "total": max(from_errlog["total"], from_console["total"]),
        "turnFailedLowMemory": max(from_errlog["turnFailedLowMemory"], from_console["turnFailedLowMemory"]),
    }


def _numbers(rows: List[Dict[str, Any]], name: str) -> List[float]:
    return [r[name] for r in rows if isinstance(r.get(name), (int, float))]


def _grade_restart(answered, reasons) -> None:
    boots = _numbers(answered, "bootN")
    if not boots:
        reasons["BLOCKED"].append("no boot count in any poll")
    elif len(set(boots)) > 1:
        reasons["FAIL"].append(f"the unit restarted: boot count {boots[0]} went to {boots[-1]}")
    ups = _numbers(answered, "uptimeMs")
    if any(b < a for a, b in zip(ups, ups[1:])):
        reasons["FAIL"].append("the unit restarted: uptime went back")


def _grade_crash(answered, reasons) -> None:
    prints = {r.get("crashFp") for r in answered}
    if len(prints) > 1:
        reasons["FAIL"].append(f"a new crash record appeared: {sorted(str(p) for p in prints)}")
    first_new = answered[0].get("crashNew") if answered else None
    if any(r.get("crashNew") and not first_new for r in answered):
        reasons["FAIL"].append("the crash record turned new during the run")


def _grade_memory(answered, start, reasons) -> Dict[str, Any]:
    all_low = _numbers(answered, "lowWater")
    if not all_low:
        reasons["BLOCKED"].append("no low-water number in any poll")
        return {}
    settled = [r for r in answered if r["t"] >= start + SETTLE_S]
    lows = _numbers(settled, "lowWater")
    lowest = min(settled, key=lambda r: r.get("lowWater", 1 << 30)) if lows else None
    if lows and min(lows) < MIN_LOW_WATER:
        reasons["FAIL"].append(f"low water {min(lows)} B after the first hour (limit {MIN_LOW_WATER} B)")
    return {
        "lowWaterLowest": min(all_low),
        "lowWaterAfterFirstHour": min(lows) if lows else None,
        "lowWaterTags": lowest.get("lowWaterTags") if lowest else None,
        "heapMinLowest": min(_numbers(answered, "heapMin"), default=None),
        "intLargestLowest": min(_numbers(answered, "intLargest"), default=None),
    }


def _grade_nvs(answered, reasons) -> None:
    free = _numbers(answered, "nvsFree")
    if not free:
        reasons["BLOCKED"].append("no settings-store number in any poll")
    elif free[0] - min(free) > NVS_FLAT_TOLERANCE_B:
        reasons["FAIL"].append(f"the settings store shrank: {free[0]} B free to {min(free)} B")


def _grade_polls(polls, reasons) -> Dict[str, Any]:
    answered = [r for r in polls if r.get("http") == 200]
    failed = len(polls) - len(answered)
    if failed:
        reasons["FAIL"].append(f"{failed} of {len(polls)} polls got no answer")
    times = [r["s"] for r in answered]
    median = statistics.median(times) if times else None
    if median is not None and median >= POLL_MEDIAN_LIMIT_S:
        reasons["FAIL"].append(f"median poll {median:.3f} s (limit {POLL_MEDIAN_LIMIT_S} s)")
    if times and max(times) > POLL_MAX_LIMIT_S:
        reasons["FAIL"].append(f"slowest poll {max(times):.2f} s (limit {POLL_MAX_LIMIT_S} s)")
    stamps = [r["t"] for r in polls]
    gap = max((b - a for a, b in zip(stamps, stamps[1:])), default=0.0)
    if gap > MAX_POLL_GAP_S:
        reasons["BLOCKED"].append(f"a {gap:.0f} s hole in the polls: the run was not watched then")
    return {"polls": len(polls), "failedPolls": failed, "pollMedianS": median, "pollMaxS": max(times, default=None)}


def _grade_turn_failures(turns, reasons) -> int:
    bad = [t for t in turns if not t.get("ok")]
    if bad:
        reasons["FAIL"].append(f"{len(bad)} of {len(turns)} turns failed; first: {bad[0].get('error', '')[:120]}")
    slow = [t for t in turns if t.get("kind") in ("text", "food") and t.get("ok") and t["s"] > TEXT_TURN_LIMIT_S]
    if slow:
        reasons["FAIL"].append(
            f"{len(slow)} text turns over {TEXT_TURN_LIMIT_S:.0f} s; slowest {max(t['s'] for t in slow)} s"
        )
    return len(bad)


def _grade_turn_coverage(turns, plan, reasons) -> Dict[str, int]:
    kinds = {kind: sum(1 for t in turns if t.get("kind") == kind) for kind in ("text", "food", "album")}
    if not plan.get("pollOnly"):
        for kind in ("text", "food"):
            if not kinds[kind]:
                reasons["BLOCKED"].append(f"no {kind} turn ran")
        if plan.get("albums") and not kinds["album"]:
            reasons["BLOCKED"].append("albums were planned and none ran")
    return kinds


def _grade_turns(turns, plan, reasons) -> Dict[str, Any]:
    failed = _grade_turn_failures(turns, reasons)
    kinds = _grade_turn_coverage(turns, plan, reasons)
    return {"turns": kinds, "failedTurns": failed}


def _grade_dream(answered, reasons) -> Optional[str]:
    last = [r.get("dreamLast") for r in answered if r.get("dreamKnown")]
    if not last:
        reasons["BLOCKED"].append("no dream ran in the window, so the night's work is unproven")
        return None
    if last[-1] in DREAM_UNPROVEN:
        reasons["BLOCKED"].append(f"the night's dream was {last[-1]}, so the night's work is unproven")
    elif last[-1] != DREAM_RAN:
        reasons["FAIL"].append(f"the night's dream ended {last[-1]!r}")
    if any(r.get("dreamKnown") and r.get("dreamStateSaved") is False for r in answered):
        reasons["FAIL"].append("the dream's state was not saved")
    return last[-1]


def _grade_scan(answered, scan, reasons) -> Dict[str, Any]:
    fails = _numbers(answered, "allocFails")
    delta = int(fails[-1] - fails[0]) if fails else None
    if delta is None:
        reasons["BLOCKED"].append("no failed-allocation counter in any poll")
    if scan["turnFailedLowMemory"]:
        reasons["FAIL"].append(f"{scan['turnFailedLowMemory']} turns failed for low memory")
    if scan["total"] or delta:
        reasons["OPEN"].append(
            f"memory failures to read: {scan['total']} log lines, the device's own counter rose by {delta}. "
            f"Classes: {json.dumps(scan['errlog']['heapBySize'] or scan['console']['heapBySize'])}"
        )
    return {"allocFailureLines": scan["total"], "allocFailsDelta": delta}


def grade_run(plan: Dict[str, Any], polls, turns, scan) -> Dict[str, Any]:
    """The grade over a run's parsed files (pure, host-tested). Only ever PASS with every number measured."""
    reasons: Dict[str, List[str]] = {"FAIL": [], "BLOCKED": [], "OPEN": []}
    start = plan.get("t", 0.0)
    answered = [r for r in polls if r.get("http") == 200]
    if not answered:
        reasons["BLOCKED"].append("no poll was answered")
    watched = (polls[-1]["t"] - start) / 3600.0 if polls else 0.0
    numbers: Dict[str, Any] = {"hoursWatched": round(watched, 3)}
    if watched < plan.get("hours", 24.0) * DURATION_SLACK:
        reasons["BLOCKED"].append(f"watched {watched:.3f} h of {plan.get('hours')} h")
    _grade_restart(answered, reasons)
    _grade_crash(answered, reasons)
    _grade_nvs(answered, reasons)
    numbers.update(_grade_memory(answered, start, reasons))
    numbers.update(_grade_polls(polls, reasons))
    numbers.update(_grade_turns(turns, plan, reasons))
    numbers["dream"] = _grade_dream(answered, reasons)
    numbers.update(_grade_scan(answered, scan, reasons))
    grades = [g for g in ("FAIL", "BLOCKED", "OPEN") if reasons[g]] or ["PASS"]
    return {"grade": bd.worst_grade(grades), "reasons": reasons, "numbers": numbers, "albumsDriven": plan.get("albums")}


def grade_dir(out: str) -> Dict[str, Any]:
    start_path = os.path.join(out, "start.json")
    if not os.path.exists(start_path):
        return {"grade": "BLOCKED", "reasons": {"BLOCKED": [f"no start.json in {out}"]}}
    with open(start_path, encoding="utf-8") as f:
        plan = json.load(f)
    polls = load_jsonl(os.path.join(out, "polls.jsonl"))
    turns = load_jsonl(os.path.join(out, "turns.jsonl"))
    return grade_run(plan, polls, turns, scan_run(out))


def summary_text(verdict: Dict[str, Any]) -> str:
    lines = [f"# 24 h soak: {verdict['grade']}", ""]
    for grade, reasons in verdict.get("reasons", {}).items():
        lines += [f"- {grade}: {reason}" for reason in reasons]
    lines += ["", "```", json.dumps(verdict.get("numbers", {}), indent=1, sort_keys=True), "```", ""]
    return "\n".join(lines)


# ---- the command line ---------------------------------------------------------------------------


def parse_args(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--ip")
    ap.add_argument("--token-file", help="a file holding the device token; a token never goes on a command line")
    ap.add_argument("--out", help="the run's evidence folder")
    ap.add_argument("--hours", type=float, default=24.0)
    ap.add_argument("--port", help="the test console's port (optional; a release image has no console)")
    ap.add_argument("--mac", help="the bench unit's full MAC; the port's USB serial must match")
    ap.add_argument("--chat", type=int, help="the owner chat id for album turns on a test image")
    ap.add_argument("--expect-build", default="", help="refuse to start unless the unit's build starts with this")
    ap.add_argument("--ballast", type=int, default=0, help="test image: hold this many bytes (FAULT sram_ballast)")
    ap.add_argument("--poll-only", action="store_true", help="only watch: no turns")
    ap.add_argument("--resume", action="store_true", help="continue the run already in --out")
    ap.add_argument("--grade", metavar="DIR", help="grade a finished run again from its files, then exit")
    ap.add_argument("--poll-s", type=float, default=10.0)
    ap.add_argument("--poll-timeout", type=float, default=25.0)
    ap.add_argument("--raw-s", type=float, default=300.0, help="how often the whole /api/state body is kept")
    ap.add_argument("--text-every", type=float, default=3600.0)
    ap.add_argument("--food-every", type=float, default=3600.0)
    ap.add_argument("--album-every", type=float, default=7200.0)
    ap.add_argument("--errlog-s", type=float, default=3600.0)
    ap.add_argument("--turn-poll-s", type=float, default=2.0)
    args = ap.parse_args(argv)
    if not args.grade and not (args.ip and args.token_file and args.out):
        ap.error("--ip, --token-file and --out are required")
    return args


def _open_console(args):
    """The guarded console and the album runner, or (None, None) on a LAN-only run."""
    if not args.port:
        return None
    port_guard.check_mac(args.mac, "soak_24h")
    port_guard.check_port(args.port, "soak_24h")
    if args.mac and port_guard.port_for_mac(args.mac, "soak_24h") != args.port:
        raise port_guard.ForbiddenDevice(f"soak_24h: {args.port} is not the board {args.mac}")
    return bd.Console(args.port, os.path.join(args.out, "console.log"), "soak_24h")


def main(argv=None) -> int:
    args = parse_args(argv)
    if args.grade:
        verdict = grade_dir(args.grade)
        print(summary_text(verdict))
        return 0 if verdict["grade"] == "PASS" else 1
    os.makedirs(args.out, exist_ok=True)
    try:
        console = _open_console(args)
    except port_guard.ForbiddenDevice as exc:
        print(exc, file=sys.stderr)
        return port_guard.REFUSED_EXIT
    api = bd.Api(args.ip, bd.read_token(args.token_file))
    album, allow_before = None, None
    try:
        if console and args.chat and not args.poll_only:
            allow_before = make_owner_chat(api, args.chat)
            album = album_runner(console, args.chat, stage_album(api, bd.Evidence(args.out, api.scrub)))
        if console and args.ballast:
            print(console.cmd(f"FAULT sram_ballast {args.ballast}", "intFree=", 15.0))
        verdict = run_soak(args, api, console, album)
    finally:
        if allow_before is not None:
            api.post_form("/api/orch", {"tgAllow": allow_before})
        if console:
            if args.ballast:
                console.cmd("FAULT sram_ballast 0", "FAULT", 8.0)
            console.close()
    print(summary_text(verdict))
    return 0 if verdict["grade"] == "PASS" else 1


if __name__ == "__main__":
    raise SystemExit(main())
