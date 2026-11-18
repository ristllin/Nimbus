#!/usr/bin/env python3
"""Lane RB-M2: the bench link budget and the send and arrival paths of a turn.

The rebuild PRD (section 2, "Latency model per request") prices a request as
1.5 s connect + body / 70,000 B per s + prefill + about 1.0 s + decode, and a chat
turn as arrival overhead + hold + one request + send path, with today's arrival
overhead (3.6 s over the settle hold) and send path (4.5 s from the model's result
to the Telegram send) taken from Lumi's logs. This module measures those constants
on the bench board and itemizes the two overheads.

Device side only. Every figure comes from the device's own clock:
  * LINKX lines (test console, src/test_console.cpp) give one exchange's phases in
    device ms: conn (TLS handshake), head, up (body upload), wait (last byte out to
    the status line), hdrs, read, total.
  * With LOGMS on, every serial log line carries the device's uptime
    ("[agent @<ms>] ..."), and a transport heaptrace line carries its own at= and
    ms=. The turn itemization reads only those stamps; the Mac's clock never enters
    a figure (the Mac is loaded, so a host wall clock is suspect).

Two halves:
  * Pure functions (parse, summarize, itemize) with host tests below (marker host,
    run by the default `pytest tests/hil`).
  * `python3 tests/hil/test_link_budget.py bench <plan> <outdir>`: the bench driver.
    Run it only under the unit F1 lock (MAC-verified flash first); it opens
    /dev/cu.usbmodem1101 only, never prints a key or token, and reads them inside
    this process only.
"""

from __future__ import annotations

import json
import os
import re
import statistics
import sys
import threading
import time
from dataclasses import dataclass, field
from typing import Dict, Iterable, List, Optional, Sequence, Tuple

import pytest

# ---- parsing --------------------------------------------------------------------------

LINKX_RE = re.compile(
    r"^LINKX (?P<kind>\w+) code=(?P<code>-?\d+) body=(?P<body>\d+) conn=(?P<conn>-?\d+) head=(?P<head>-?\d+) "
    r"up=(?P<up>-?\d+) paced=(?P<paced>\d+) wait=(?P<wait>-?\d+) hdrs=(?P<hdrs>-?\d+) read=(?P<read>-?\d+) "
    r"total=(?P<total>-?\d+) err=(?P<err>.*)$"
)
STAMP_RE = re.compile(r"^\[agent @(?P<ms>\d+)\] (?P<text>.*)$")
HEAPTRACE_RE = re.compile(r"transport: heaptrace (?P<target>\S+) body=(?P<body>\d+) .*?\bat=(?P<at>\d+) ms=(?P<ms>\d+)")

PHASES = ("conn", "head", "up", "wait", "hdrs", "read", "total")


@dataclass
class Exchange:
    kind: str
    code: int
    body: int
    conn: int
    head: int
    up: int
    paced: int
    wait: int
    hdrs: int
    read: int
    total: int
    err: str

    @property
    def ok(self) -> bool:
        return 200 <= self.code < 300 and self.err == "-"

    @property
    def upload_bps(self) -> Optional[float]:
        """Body bytes per second of the upload (the time the slices took to go out)."""
        if self.body <= 0 or self.up <= 0:
            return None
        return self.body * 1000.0 / self.up


def parse_linkx(line: str) -> Optional[Exchange]:
    m = LINKX_RE.match(line.strip())
    if not m:
        return None
    d = m.groupdict()
    return Exchange(
        kind=d["kind"],
        err=d["err"].strip(),
        **{k: int(d[k]) for k in ("code", "body", "conn", "head", "up", "paced", "wait", "hdrs", "read", "total")},
    )


@dataclass
class Stamped:
    ms: int
    text: str


def parse_stamped(lines: Iterable[str]) -> List[Stamped]:
    """The device-stamped log lines, in arrival order (serial keeps the device's order)."""
    out = []
    for raw in lines:
        m = STAMP_RE.match(raw.strip())
        if m:
            out.append(Stamped(int(m.group("ms")), m.group("text")))
    return out


# ---- statistics -----------------------------------------------------------------------


def summary(values: Sequence[float]) -> Dict[str, float]:
    """n, median, min, max, p90 and the spread (max - min). One sample is not a finding,
    so a summary of fewer than 5 values says so (enough=False)."""
    vals = sorted(float(v) for v in values)
    if not vals:
        return {"n": 0, "enough": False}
    p90 = vals[min(len(vals) - 1, int(round(0.9 * (len(vals) - 1))))]
    return {
        "n": len(vals),
        "median": statistics.median(vals),
        "min": vals[0],
        "max": vals[-1],
        "p90": p90,
        "spread": vals[-1] - vals[0],
        "enough": len(vals) >= 5,
    }


def summarize_exchanges(xs: Sequence[Exchange]) -> Dict[str, object]:
    ok = [x for x in xs if x.ok]
    out: Dict[str, object] = {"runs": len(xs), "ok": len(ok), "failed": [f"{x.code} {x.err}" for x in xs if not x.ok]}
    for p in PHASES:
        out[p + "_ms"] = summary([getattr(x, p) for x in ok if getattr(x, p) >= 0])
    out["paced_ms"] = summary([x.paced for x in ok])
    out["upload_Bps"] = summary([x.upload_bps for x in ok if x.upload_bps])
    out["body_B"] = summary([x.body for x in ok])
    return out


def fit_link(xs: Sequence[Exchange]) -> Dict[str, float]:
    """Least squares of upload ms on body bytes over successful exchanges: the slope is the
    link's marginal rate (B/s) and the intercept the fixed cost of the first slices."""
    pts = [(x.body, x.up) for x in xs if x.ok and x.up > 0 and x.body > 0]
    if len(pts) < 2:
        return {}
    n = len(pts)
    mx = sum(p[0] for p in pts) / n
    my = sum(p[1] for p in pts) / n
    sxx = sum((p[0] - mx) ** 2 for p in pts)
    if sxx == 0:
        return {}
    slope = sum((p[0] - mx) * (p[1] - my) for p in pts) / sxx   # ms per byte
    return {"n": n, "ms_per_byte": slope, "Bps_marginal": 1000.0 / slope if slope > 0 else float("inf"),
            "intercept_ms": my - slope * mx}


# ---- the turn itemization ---------------------------------------------------------------

ARRIVAL = "tg archive:"
TURN_START = "orchestrator: turn host="
TURN_END = "turn end:"
SEND_OK = "telegram: send ok"


@dataclass
class Segment:
    ms: int
    ends_at: str   # the line the gap ends on: the work that ran in it is what that line reports

    def as_dict(self) -> Dict[str, object]:
        return {"ms": self.ms, "ends_at": self.ends_at}


@dataclass
class TurnTiming:
    arrival_ms: Optional[int] = None
    start_ms: Optional[int] = None
    result_ms: Optional[int] = None      # the head's last exchange ended (its at= + ms=)
    end_ms: Optional[int] = None
    send_ms: Optional[int] = None
    arrival_segments: List[Segment] = field(default_factory=list)
    send_segments: List[Segment] = field(default_factory=list)
    head_exchanges: int = 0

    @property
    def arrival_to_start(self) -> Optional[int]:
        return None if self.arrival_ms is None or self.start_ms is None else self.start_ms - self.arrival_ms

    @property
    def result_to_send(self) -> Optional[int]:
        return None if self.result_ms is None or self.send_ms is None else self.send_ms - self.result_ms


def _segments(lines: Sequence[Stamped], t0: int, t1: int) -> List[Segment]:
    """The gaps between consecutive stamped lines inside (t0, t1], each named by the line it
    ends on; they sum to t1 - t0."""
    segs, prev = [], t0
    for ln in lines:
        if t0 < ln.ms <= t1:
            segs.append(Segment(ln.ms - prev, short(ln.text)))
            prev = ln.ms
    if prev < t1:
        segs.append(Segment(t1 - prev, "(no line)"))
    return segs


def short(text: str, n: int = 72) -> str:
    text = re.sub(r"\d{6,}", "<n>", text)   # chat ids, update ids, uptimes: not part of the name
    return text[:n]


def itemize_turn(lines: Sequence[Stamped], head_host: str = "api.anthropic.com") -> TurnTiming:
    """One turn's arrival and send path from its stamped lines (the lines of ONE turn: from
    the archive line that brought it in to the first send ok after its turn end)."""
    t = TurnTiming()
    for ln in lines:
        if t.arrival_ms is None and ln.text.startswith(ARRIVAL) and "new update" in ln.text:
            t.arrival_ms = ln.ms
        elif t.start_ms is None and ln.text.startswith(TURN_START):
            t.start_ms = ln.ms
        elif t.start_ms is not None and t.end_ms is None and ln.text.startswith(TURN_END):
            t.end_ms = ln.ms
        elif t.end_ms is not None and t.send_ms is None and ln.text.startswith(SEND_OK):
            t.send_ms = ln.ms
        m = HEAPTRACE_RE.search(ln.text)
        if m and m.group("target").startswith(head_host) and t.start_ms is not None and t.end_ms is None:
            t.result_ms = int(m.group("at")) + int(m.group("ms"))
            t.head_exchanges += 1
    if t.arrival_ms is not None and t.start_ms is not None:
        t.arrival_segments = _segments(lines, t.arrival_ms, t.start_ms)
    if t.result_ms is not None and t.send_ms is not None:
        t.send_segments = _segments(lines, t.result_ms, t.send_ms)
    return t


def split_turns(lines: Sequence[Stamped]) -> List[List[Stamped]]:
    """Cut a run's stamped lines into turns: each turn opens at an archive line that saved
    new updates and runs to the line before the next one."""
    starts = [i for i, ln in enumerate(lines) if ln.text.startswith(ARRIVAL) and "new update" in ln.text]
    return [list(lines[a:b]) for a, b in zip(starts, starts[1:] + [len(lines)])]


def aggregate_segments(turns: Sequence[TurnTiming], which: str) -> List[Dict[str, object]]:
    """Per named gap (the line it ends on), the median and spread across turns."""
    by: Dict[str, List[int]] = {}
    order: List[str] = []
    for t in turns:
        seen: Dict[str, int] = {}
        for s in getattr(t, which):
            seen[s.ends_at] = seen.get(s.ends_at, 0) + s.ms
        for k, v in seen.items():
            if k not in by:
                by[k] = []
                order.append(k)
            by[k].append(v)
    return [{"ends_at": k, **summary(by[k])} for k in order]


# ---- host tests -------------------------------------------------------------------------

pytestmark = pytest.mark.host


def test_linkx_line_parses_every_phase():
    x = parse_linkx("LINKX pad code=200 body=120000 conn=812 head=3 up=1650 paced=0 wait=2104 hdrs=4 read=11 "
                    "total=4590 err=-")
    assert x and x.ok and x.kind == "pad" and x.body == 120000 and x.wait == 2104
    assert abs(x.upload_bps - 120000 / 1.65) < 1


def test_linkx_failure_is_not_ok_and_is_counted():
    # Counter-test: a timeout (code 0) or a 2xx with a transport error never counts as a sample.
    bad = parse_linkx("LINKX pad code=0 body=120000 conn=900 head=2 up=-1 paced=0 wait=-1 hdrs=-1 read=-1 "
                      "total=120003 err=no response")
    half = parse_linkx("LINKX pad code=200 body=1 conn=1 head=1 up=1 paced=0 wait=1 hdrs=1 read=1 total=5 err=cut")
    good = parse_linkx("LINKX pad code=200 body=9 conn=1 head=1 up=1 paced=0 wait=1 hdrs=1 read=1 total=5 err=-")
    assert not bad.ok and not half.ok
    s = summarize_exchanges([bad, half, good])
    assert s["runs"] == 3 and s["ok"] == 1 and len(s["failed"]) == 2
    assert s["total_ms"]["n"] == 1


def test_malformed_linkx_line_is_refused():
    assert parse_linkx("LINKX pad code=200 body=1") is None
    assert parse_linkx("LINKX usage: LINKX pad <bytes>") is None


def test_summary_needs_five_samples():
    assert summary([1, 2, 3, 4])["enough"] is False
    s = summary([5, 1, 3, 2, 4])
    assert s["enough"] and s["median"] == 3 and s["spread"] == 4 and s["min"] == 1


def test_fit_link_recovers_the_rate():
    xs = [parse_linkx(f"LINKX pad code=200 body={b} conn=1 head=1 up={50 + b // 100} paced=0 wait=1 hdrs=1 "
                      f"read=1 total=9 err=-") for b in (60000, 120000, 250000)]
    f = fit_link(xs)
    assert abs(f["Bps_marginal"] - 100000) < 1 and abs(f["intercept_ms"] - 50) < 1


TURN_FIXTURE = [
    "[agent @1000] tg archive: 1 new update(s) saved, 0 already saved",
    "[agent @7010] telegram: settle released 1 update(s)",
    "[agent @7400] memory: recall 3 hit(s)",
    "[agent @9600] orchestrator: turn host=anthropic (conv=4 heap=40000)",
    "[agent @9700] transport: heaptrace api.anthropic.com/v1/messages body=61000 pre=1/1 conn=1/1 wrote=1/1 "
    "done=1/1 fail=none af=0 min=1 at=9650 ms=7350 wms=900 pms=0",
    "[agent @17200] apply: 0 actions",
    "[agent @19000] turn end: ok host=anthropic tokens in=1 out=1 tools=0 reply=40B src=tg",
    "[agent @21500] telegram: send ok to 123456789",
    "[agent @30000] tg archive: 1 new update(s) saved, 0 already saved",
]


def test_itemize_turn_splits_arrival_and_send_path():
    turns = split_turns(parse_stamped(TURN_FIXTURE))
    assert len(turns) == 2
    t = itemize_turn(turns[0])
    assert t.arrival_to_start == 8600 and t.result_ms == 17000 and t.result_to_send == 4500
    assert sum(s.ms for s in t.arrival_segments) == 8600
    assert sum(s.ms for s in t.send_segments) == 4500
    assert [s.ms for s in t.send_segments] == [200, 1800, 2500]
    assert t.send_segments[-1].ends_at.startswith("telegram: send ok to <n>")


def test_itemize_turn_ignores_unstamped_and_other_hosts():
    # Counter-test: a line without the device stamp never enters a figure (no host clock),
    # and an exchange to another host is not the model's result.
    lines = parse_stamped(TURN_FIXTURE[:4] + [
        "[agent] transport: heaptrace api.anthropic.com/v1/messages body=1 at=99999 ms=1",
        "[agent @9800] transport: heaptrace api.telegram.org/x body=1 pre=1/1 at=9800 ms=50",
    ] + TURN_FIXTURE[4:8])
    t = itemize_turn(lines)
    assert t.result_ms == 17000 and t.head_exchanges == 1


def test_aggregate_segments_medians_per_named_gap():
    extra = TURN_FIXTURE[1:8] + ["[agent @52000] telegram: send ok to 123456789"]
    turns = [itemize_turn(x) for x in split_turns(parse_stamped(TURN_FIXTURE + extra))]
    agg = aggregate_segments([t for t in turns if t.send_segments], "send_segments")
    assert agg[0]["ends_at"].startswith("apply:") and agg[0]["n"] >= 1


# ---- the bench driver -----------------------------------------------------------------

PORT = "/dev/cu.usbmodem1101"           # unit F1 only; never /dev/cu.usbmodem2101
SECRETS = "/Users/roydarnell/Projects/nimbus-project/secrets/devices"
MAIN_ENV = "/Users/roydarnell/Projects/nimbus-project/nimbus/.env"


def load_env(path: str) -> Dict[str, str]:
    """KEY=value pairs, read inside this process only (never echoed)."""
    out = {}
    with open(path) as f:
        for line in f:
            m = re.match(r"\s*(?:export\s+)?([A-Za-z_][A-Za-z0-9_]*)\s*=\s*(.*)$", line)
            if m:
                out[m.group(1)] = m.group(2).strip().strip('"').strip("'")
    return out


class Bench:
    """Serial console + device HTTP for one hold. All lines go to <out>/serial.log with the
    host receive time for orientation only; figures use the device's own stamps."""

    def __init__(self, out: str, deadline: float):
        import serial  # noqa: PLC0415 - bench only
        import urllib.request  # noqa: PLC0415

        self.out, self.deadline = out, deadline
        self.ip = open(f"{SECRETS}/unitF1_ip.txt").read().strip()
        self.tok = open(f"{SECRETS}/unitF1_webtok.txt").read().strip()
        self.secrets = [self.tok]
        self.opener = urllib.request.build_opener(urllib.request.ProxyHandler({}))
        self.s = serial.Serial()
        self.s.port, self.s.baudrate, self.s.timeout = PORT, 115200, 0.1
        self.s.dtr = self.s.rts = False   # an asserted line resets the board
        self.s.open()
        self.lines: List[str] = []
        self.lock = threading.Lock()
        self.log = open(os.path.join(out, "serial.log"), "a")
        self.notes = open(os.path.join(out, "run.log"), "a")
        self.stop = False
        self.reader = threading.Thread(target=self._read, daemon=True)
        self.reader.start()

    def redact(self, s: str) -> str:
        for sec in self.secrets:
            if sec:
                s = s.replace(sec, "[redacted]")
        return s

    def _read(self):
        buf = b""
        while not self.stop:
            buf += self.s.read(4096)
            *full, buf = buf.split(b"\n")
            for raw in full:
                line = self.redact(raw.decode("utf-8", "replace").rstrip("\r"))
                with self.lock:
                    self.lines.append(line)
                self.log.write(time.strftime("%H:%M:%S ", time.gmtime()) + line + "\n")
            self.log.flush()

    def say(self, *a):
        line = self.redact(time.strftime("%H:%M:%SZ ", time.gmtime()) + " ".join(str(x) for x in a))
        print(line, flush=True)
        self.notes.write(line + "\n")
        self.notes.flush()

    def mark(self) -> int:
        with self.lock:
            return len(self.lines)

    def since(self, mark: int) -> List[str]:
        with self.lock:
            return list(self.lines[mark:])

    def verb(self, cmd: str, prefix: str, timeout: float = 10.0) -> str:
        m = self.mark()
        self.s.write((cmd + "\n").encode())
        end = time.time() + timeout
        while time.time() < end:
            for ln in self.since(m):
                if ln.startswith(prefix):
                    return ln
            time.sleep(0.05)
        return ""

    def wait_line(self, mark: int, pred, timeout: float) -> Optional[str]:
        end = time.time() + timeout
        while time.time() < end:
            for ln in self.since(mark):
                if pred(ln):
                    return ln
            time.sleep(0.2)
        return None

    def http(self, method: str, path: str, form: Optional[dict] = None) -> Tuple[int, str]:
        import urllib.error  # noqa: PLC0415
        import urllib.parse  # noqa: PLC0415
        import urllib.request  # noqa: PLC0415

        data = urllib.parse.urlencode({**form, "t": self.tok}).encode() if form is not None else None
        req = urllib.request.Request(f"http://{self.ip}{path}", data=data, method=method,
                                     headers={"X-Nimbus-Token": self.tok})
        try:
            with self.opener.open(req, timeout=20) as r:
                return r.status, r.read().decode("utf-8", "replace")
        except urllib.error.HTTPError as e:
            return e.code, e.read().decode("utf-8", "replace")

    def orch(self) -> dict:
        _, body = self.http("GET", "/api/orch")
        try:
            return json.loads(body)
        except ValueError:
            return {}

    def time_left(self) -> float:
        return self.deadline - time.time()

    def close(self):
        self.stop = True
        self.reader.join(timeout=2)
        self.s.close()

    # -- the first action after every flash or reset: sound 0, voice replies off, read back --
    def sound_off(self, label: str) -> bool:
        line = ""
        for _ in range(15):
            line = self.verb("SFXVOL 0", "SFXVOL ->", 4)
            if line:
                break
            time.sleep(2)
        self.http("POST", "/api/orch", {"ttsOn": "0", "sfxVol": "0", "sfxLvlO": "0", "sfxLvlN": "0"})
        o = self.orch()
        picked = {k: o.get(k) for k in ("sfxVol", "ttsOn", "sfxLvlO", "sfxLvlN", "sfxTier")}
        ok = "0%" in line and o.get("sfxVol") == 0 and o.get("ttsOn") is False
        self.say(f"SOUND {label}: console={line!r} readback={picked} {'PASS' if ok else 'FAIL'}")
        return ok

    def fsput(self, local: str, path: str) -> bool:
        import base64  # noqa: PLC0415
        import hashlib  # noqa: PLC0415

        data = open(local, "rb").read()
        if not self.verb(f"FSPUT {len(data)} {path}", "FSPUT READY", 10):
            return False
        m = self.mark()
        b64 = base64.b64encode(data)
        acked = 0
        for i in range(0, len(b64), 2048):
            self.s.write(b64[i:i + 2048])
            # pace on the device's 4 KB acks so its RX buffer never overflows
            while (i + 2048) * 3 // 4 - acked > 8192:
                ln = self.wait_line(m, lambda s: s.startswith("FSACK ") or s.startswith("FSPUT DONE"), 10)
                if not ln:
                    return False
                if ln.startswith("FSPUT DONE"):
                    break
                acked = int(ln.split()[1])
                m = self.mark()
        done = self.wait_line(m, lambda s: s.startswith("FSPUT DONE"), 20) or ""
        return hashlib.sha256(data).hexdigest() in done


def run_linkx(b: Bench, cmd: str, n: int, timeout: float = 150.0, pause: float = 0.5) -> List[str]:
    lines = []
    for i in range(n):
        if b.time_left() < timeout + 30:
            b.say(f"STOP {cmd}: the hold ends in {int(b.time_left())} s, {i} of {n} run")
            break
        ln = b.verb(f"LINKX {cmd}", "LINKX ", timeout)
        b.say(f"> LINKX {cmd} [{i + 1}/{n}] -> {ln or '(no answer)'}")
        lines.append(ln or f"LINKX none code=0 body=0 conn=-1 head=-1 up=-1 paced=0 wait=-1 hdrs=-1 read=-1 "
                           f"total=-1 err=no console answer in {int(timeout)} s")
        time.sleep(pause)
    return lines


def settle_page(chat: str, update_id: int, text: str) -> str:
    msg = {"message_id": update_id % 100000, "from": {"id": int(chat), "is_bot": False, "first_name": "Bench"},
           "chat": {"id": int(chat), "type": "private"}, "date": int(time.time()), "text": text}
    return json.dumps({"ok": True, "settle": True, "result": [{"update_id": update_id, "message": msg}]},
                      separators=(",", ":"))


TURN_TEXTS = [
    "What is 17 plus 25? One short sentence please.",
    "Name one fruit that is yellow. Just one line.",
    "Say good morning in French, nothing else.",
    "How many days are in a leap year? Short answer.",
    "What colour is the sky on a clear day? One line.",
    "Give me one word that rhymes with cat.",
    "What is 9 times 7? Short answer.",
]


def plan_turns(b: Bench, n: int, out: str) -> None:
    """Telegram turns on the bench bot through the full receive path (a settle page via
    TGINJECT: archive, settle hold, batcher, one turn) and the real sendMessage."""
    env = load_env(MAIN_ENV)
    tg_tok, chat = env.get("TELEGRAM_KEY_2", ""), env.get("NIMBUS_TEST_TG_CHAT", "")
    b.secrets += [tg_tok, chat]
    if not tg_tok or not chat:
        b.say("TURNS NOT RUN: no bench bot token or test chat in the main .env")
        return
    before = b.orch()
    restore = {"orchHost": before.get("orchHost", ""), "tgAllow": before.get("tgAllow", "")}
    b.say(f"TURNS setup: head was {restore['orchHost']!r}, Telegram {'on' if before.get('hasTg') else 'off'}")
    b.http("POST", "/api/orch", {"tgToken": tg_tok, "tgAllow": chat, "orchHost": "anthropic"})
    time.sleep(8)
    o = b.orch()
    b.say(f"TURNS config: hasTg={o.get('hasTg')} tgVerify={o.get('tgVerify')} head={o.get('orchHost')}")
    raw: List[str] = []
    try:
        for i in range(n):
            if b.time_left() < 120:
                b.say(f"TURNS stop: hold ends in {int(b.time_left())} s after {i} turns")
                break
            m = b.mark()
            uid = 900000000 + int(time.time()) % 100000000
            page = settle_page(chat, uid, TURN_TEXTS[i % len(TURN_TEXTS)])
            ack = b.verb(f"TGINJECT {page}", "TGINJECT", 10)
            b.say(f"TURN {i + 1}/{n} injected: {ack[:80]!r}")
            sent = b.wait_line(m, lambda s: "telegram: send " in s, 150)
            b.say(f"TURN {i + 1}/{n} -> {sent or '(no send in 150 s)'}")
            time.sleep(12)   # the turn's after-work (memory, usage) lands before the next one
            raw += b.since(m)
    finally:
        b.http("POST", "/api/orch", {"clr_tgToken": "1", "tgAllow": restore["tgAllow"],
                                     "orchHost": restore["orchHost"]})
        time.sleep(3)
        o = b.orch()
        b.say(f"TURNS restored: hasTg={o.get('hasTg')} head={o.get('orchHost')} tgAllow={'set' if o.get('tgAllow') else 'empty'}")
    with open(os.path.join(out, "turns.raw.log"), "w") as f:
        f.write("\n".join(b.redact(x) for x in raw) + "\n")


def make_photos(dirpath: str, n: int = 4) -> List[str]:
    """Synthetic 800 x 600 JPEGs of about 75 KB each (100,000 B of base64): two make the
    PRD's 200,000 B inline allowance. Noise over a gradient, so the size is real."""
    import random  # noqa: PLC0415

    from PIL import Image  # noqa: PLC0415

    paths = []
    rnd = random.Random(20261118)
    for k in range(n):
        img = Image.new("RGB", (800, 600))
        px = img.load()
        for y in range(600):
            for x in range(800):
                v = rnd.randrange(64)
                px[x, y] = ((x * 255 // 800 + v) % 256, (y * 255 // 600 + v) % 256, (k * 60 + v) % 256)
        p = os.path.join(dirpath, f"p{k + 1}.jpg")
        q = 60
        while True:
            img.save(p, "JPEG", quality=q)
            if os.path.getsize(p) <= 78000 or q <= 10:
                break
            q -= 5
        paths.append(p)
    return paths


PAD_SIZES = (60000, 85000, 120000, 180000, 250000)


def plan_link(b: Bench, reps: int, out: str) -> None:
    res: Dict[str, List[str]] = {}
    res["get_telegram"] = run_linkx(b, "get api.telegram.org /", reps, 30)
    res["get_anthropic"] = run_linkx(b, "get api.anthropic.com /", reps, 30)
    for size in PAD_SIZES:
        res[f"pad_{size}"] = run_linkx(b, f"pad {size}", reps)
    with open(os.path.join(out, "link.raw.json"), "a") as f:
        f.write(json.dumps(res) + "\n")


def plan_photos(b: Bench, reps: int, out: str) -> None:
    import tempfile  # noqa: PLC0415

    tmp = tempfile.mkdtemp(prefix="rbm2-photos-")
    local = make_photos(tmp)
    sizes = [os.path.getsize(p) for p in local]
    b.say(f"PHOTOS made: {sizes} B")
    card = []
    for i, p in enumerate(local):
        dst = f"/mem/rbm2/p{i + 1}.jpg"
        ok = b.fsput(p, dst)
        b.say(f"FSPUT {dst} {'ok' if ok else 'FAILED'}")
        if ok:
            card.append(dst)
    res: Dict[str, object] = {"photo_bytes": sizes}
    if card:
        res["file_upload"] = run_linkx(b, f"file {card[0]}", reps)
    if len(card) >= 2:
        res["album2_pad60000"] = run_linkx(b, f"img 60000 {card[0]} {card[1]}", reps)
    if len(card) >= 4:
        res["album4_pad60000"] = run_linkx(b, f"img 60000 {' '.join(card[:4])}", reps)
    with open(os.path.join(out, "photos.raw.json"), "a") as f:
        f.write(json.dumps(res) + "\n")


def plan_hundred(b: Bench, n: int, out: str) -> None:
    lines = run_linkx(b, "pad 120000", n, pause=0.2)
    with open(os.path.join(out, "hundred.raw.json"), "a") as f:
        f.write(json.dumps({"pad_120000_x": lines}) + "\n")


def bench_main(argv: Sequence[str]) -> int:
    """bench <plan[,plan...]> <outdir> [minutes]: plans are sound, logms, turns, link,
    photos, hundred, store (test_store_timing). Stops each plan before the hold's end."""
    plans, out = argv[0].split(","), argv[1]
    minutes = float(argv[2]) if len(argv) > 2 else 28.0
    os.makedirs(out, exist_ok=True)
    b = Bench(out, time.time() + minutes * 60)
    rc = 0
    try:
        if not b.sound_off("hold start"):
            rc = 2
        ms = b.verb("LOGMS on", "LOGMS ", 5)
        b.say(f"LOGMS -> {ms!r}")
        for plan in plans:
            b.say(f"PLAN {plan} start ({int(b.time_left())} s left in the hold)")
            if plan == "turns":
                plan_turns(b, 6, out)
            elif plan == "link":
                plan_link(b, 6, out)
            elif plan == "photos":
                plan_photos(b, 6, out)
            elif plan == "hundred":
                plan_hundred(b, 100, out)
            elif plan == "store":
                sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
                import test_store_timing  # noqa: PLC0415

                test_store_timing.plan_store(b, 8, out)
            b.say(f"PLAN {plan} done")
        b.verb("LOGMS off", "LOGMS ", 5)
        b.sound_off("hold end")
    finally:
        b.close()
    return rc


if __name__ == "__main__":
    if len(sys.argv) >= 4 and sys.argv[1] == "bench":
        sys.exit(bench_main(sys.argv[2:]))
    print(__doc__)
    sys.exit(64)
