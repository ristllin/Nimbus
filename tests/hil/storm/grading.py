"""Grading for the storm suite: pure functions over recorded device output.

Every storm leg reduces what it saw (device log lines, the owner chat's messages,
the LAN mock's request log) to one ``Metrics`` record per image, grades it against
the leg's ``Bounds`` on the four acceptance properties, prints a before/after table
(the v4.5.13 control image against the release candidate) and writes one JSON
evidence record per run. Nothing here touches a device: the host tests
(``test_storm_grading_host.py``) run it over recorded, scrubbed logs of the 19 and
21 Oct 2026 storms.

The four properties (PRD addendum, section B):

* usable: a fresh owner message, sent after the storm, gets an answer or an honest
  refusal within the leg's bound;
* no loop: paid turns, owner notices and identical log lines stay inside the leg's
  table;
* capped: provider requests stay inside the leg's table, and a parked or frozen
  provider gets none;
* told once: exactly one plain owner notice for the episode, per channel.
"""

from __future__ import annotations

import json
import os
import re
import time
from dataclasses import asdict, dataclass, field
from typing import Dict, Iterable, List, Optional, Sequence, Tuple

# The legs, in suite order. The release gate's storm step requires evidence for each
# (tools/release_gate/check_storm_evidence.py keeps the same list; a host test pins
# the two together and to the leg files).
LEGS: Tuple[str, ...] = (
    "a_settings_full",
    "b_page_reserved",
    "c_provider_refusing",
    "d_deterministic_error",
    "e_low_heap",
    "f_reboot_mid_turn",
    "g_file_store_full",
    "h_false_claim",
)
PROPERTIES: Tuple[str, ...] = ("usable", "no_loop", "capped", "told_once")
PASS, FAIL, BLOCKED = "PASS", "FAIL", "BLOCKED"
CONTROL, CANDIDATE = "control", "candidate"
EVIDENCE_SCHEMA = 1

# One durable-log line: "<seq> <uptime ms> <tag> <text>" (/api/errlog, /api/errlog/recent).
LOG_LINE_RE = re.compile(r"^\s*(?P<seq>\d+)\s+(?P<ms>\d+)\s+(?P<tag>\S+)\s(?P<text>.*)$")
# A paid head turn ended (both images log it): "turn end: ok host=anthropic tokens in=.. out=..".
TURN_END_RE = re.compile(
    r"\bturn end: (?P<outcome>\S+) host=(?P<host>\S+)(?: tokens in=(?P<tin>\d+) out=(?P<tout>\d+))?"
    r"(?:.*?\bsrc=(?P<src>\S+))?"
)
# Turns the device starts on its own (a helper's synthesis, a routine, the nightly
# upkeep): they belong to whatever started them, not to the inbound storm a leg drives
# (on a shared bench unit, an earlier lane's helper may finish inside a leg's window).
BACKGROUND_SOURCES = ("synthesis", "loop", "dream")
# Requests per turn as the device bills them: "turn spend: ok owner | anthropic/<model> ... n=3".
TURN_SPEND_RE = re.compile(r"\bturn spend: \S+ (?P<src>\S+) \| (?P<rows>.*)$")
SPEND_N_RE = re.compile(r"\bn=(\d+)")
# The provider answered on the device's behalf (PROVFAULT): a request that never left.
OVERRIDE_RE = re.compile(r"provider: TEST override answered (?P<host>[^/\s]+)\S* with HTTP (?P<status>\d+)")
PARKED_RE = re.compile(r"provider: (?P<host>\S+) parked for every seam")
# A Telegram message line ("telegram: msg from <name> (chat <id>): <text>").
TG_MSG_RE = re.compile(r"telegram: msg from .*? \(chat (?P<chat>-?\d+)\): (?P<text>.*)$")
# Notices the device logs when it queues one for the owner outside a turn's reply.
NOTICE_LOG_RE = re.compile(
    r"telegram: guard notice \((?P<kind>\w+)\) queued for the owner"
    r"|telegram: storage-full notice sent to the owner"
    r"|crash: owner notice queued"
)


@dataclass
class LogLine:
    seq: int
    ms: int
    tag: str
    text: str


def parse_log(text: str) -> List[LogLine]:
    """The durable-log lines of ``text``, in order; lines of another shape are kept as
    text-only lines (seq -1), so a console capture grades the same way."""
    out = []
    for raw in (text or "").splitlines():
        m = LOG_LINE_RE.match(raw)
        if m:
            out.append(LogLine(int(m.group("seq")), int(m.group("ms")), m.group("tag"), m.group("text").rstrip()))
        elif raw.strip():
            out.append(LogLine(-1, -1, "-", raw.strip()))
    return out


def after_seq(lines: Iterable[LogLine], seq: int) -> List[LogLine]:
    """The lines logged after ``seq`` (a console line, seq -1, is always kept)."""
    return [ln for ln in lines if ln.seq < 0 or ln.seq > seq]


def newest_seq(lines: Iterable[LogLine]) -> int:
    seqs = [ln.seq for ln in lines if ln.seq >= 0]
    return max(seqs) if seqs else -1


# Periodic status lines: a steady device repeats them word for word on a timer, which is
# health, not a loop.
HEARTBEATS: Tuple[str, ...] = ("[sram] steady",)


def identical_peak(lines: Iterable[LogLine], ignore: Sequence[str] = ()) -> Tuple[int, str]:
    """The most often repeated exact line text and its count. Lines holding any of
    ``ignore`` (a leg's own drive, such as the injected page's echo) or a periodic
    status line (HEARTBEATS) are not counted."""
    counts: Dict[str, int] = {}
    skip = tuple(ignore) + HEARTBEATS
    for ln in lines:
        if any(s in ln.text for s in skip):
            continue
        counts[ln.text] = counts.get(ln.text, 0) + 1
    if not counts:
        return 0, ""
    text = max(counts, key=lambda k: counts[k])
    return counts[text], text


def turn_ends(lines: Iterable[LogLine]) -> List[dict]:
    """Every head turn that ended, with its host and the provider-reported tokens."""
    out = []
    for ln in lines:
        m = TURN_END_RE.search(ln.text)
        if m:
            out.append(
                {
                    "outcome": m.group("outcome"),
                    "host": m.group("host"),
                    "tin": int(m.group("tin") or 0),
                    "tout": int(m.group("tout") or 0),
                    "src": m.group("src") or "",
                }
            )
    return out


def paid_turns(lines: Iterable[LogLine]) -> int:
    """Inbound head turns that reached a provider: a turn that ended with no host ran
    nothing, and a turn the device started on its own is not the storm's."""
    return sum(1 for t in turn_ends(lines) if t["host"] not in ("none", "-", "") and t["src"] not in BACKGROUND_SOURCES)


def billed_requests(lines: Iterable[LogLine]) -> int:
    """Provider requests of the inbound turns as the device bills them (the n= of each
    host on a 'turn spend:' line); 0 on an image that logs no spend lines."""
    total = 0
    for ln in lines:
        m = TURN_SPEND_RE.search(ln.text)
        if m and m.group("src") not in BACKGROUND_SOURCES:
            total += sum(int(n) for n in SPEND_N_RE.findall(m.group("rows")))
    return total


def rows_per_turn(rows: Iterable[dict]) -> Dict[str, int]:
    """Assistant messages per turn (the 'turn:' tag of each message): one turn should
    reach the owner as one message."""
    out: Dict[str, int] = {}
    for r in rows:
        if r.get("role") != "assistant":
            continue
        tag = next((t for t in str(r.get("tags") or "").split(",") if t.startswith("turn:")), "turn:?")
        out[tag] = out.get(tag, 0) + 1
    return out


def override_requests(lines: Iterable[LogLine]) -> Dict[str, int]:
    """Requests the PROVFAULT override answered, per API host."""
    out: Dict[str, int] = {}
    for ln in lines:
        m = OVERRIDE_RE.search(ln.text)
        if m:
            out[m.group("host")] = out.get(m.group("host"), 0) + 1
    return out


def requests_after_park(lines: Sequence[LogLine], host_word: str, request_re: "re.Pattern[str]") -> int:
    """Requests matching ``request_re`` logged after the first line that parks the
    provider named by ``host_word`` (0 when it was never parked)."""
    parked = False
    n = 0
    for ln in lines:
        if not parked:
            m = PARKED_RE.search(ln.text)
            parked = bool(m and host_word in m.group("host"))
            continue
        if request_re.search(ln.text):
            n += 1
    return n


def log_notices(lines: Iterable[LogLine]) -> List[str]:
    """The owner notices the device logged as queued or sent outside a turn's reply."""
    return [ln.text for ln in lines if NOTICE_LOG_RE.search(ln.text)]


def inbound_texts(lines: Iterable[LogLine], chat: Optional[int] = None) -> List[str]:
    """Telegram message texts the device took in (optionally from one chat)."""
    out = []
    for ln in lines:
        m = TG_MSG_RE.search(ln.text)
        if m and (chat is None or int(m.group("chat")) == chat):
            out.append(m.group("text"))
    return out


def new_rows(before: Dict[str, dict], after: Dict[str, dict]) -> List[dict]:
    """Owner-chat messages that appeared between two reads (by message id), oldest first."""
    return [after[k] for k in sorted(after) if k not in before]


def classify_replies(rows: Iterable[dict], answer_re: "re.Pattern[str]", notice_re: "re.Pattern[str]") -> dict:
    """The assistant messages of a window split into answers (the leg's expected
    answer), notices (the leg's notice words) and other replies."""
    out = {"answers": [], "notices": [], "other": []}
    for r in rows:
        if r.get("role") != "assistant":
            continue
        text = str(r.get("text") or "")
        if notice_re.search(text):
            out["notices"].append(text)
        elif answer_re.search(text):
            out["answers"].append(text)
        else:
            out["other"].append(text)
    return out


# ---- metrics, bounds and verdicts --------------------------------------------------


@dataclass
class Metrics:
    """What one run of one leg measured. ``None`` = not measured on this image (the
    reason is in ``notes``); a bound on an unmeasured metric grades BLOCKED."""

    paid_turns: Optional[int] = None
    provider_requests: Optional[int] = None
    parked_requests: Optional[int] = None
    notices: Optional[int] = None
    identical_peak: Optional[int] = None
    identical_line: str = ""
    usable: Optional[bool] = None
    usable_s: Optional[float] = None
    extra: Dict[str, object] = field(default_factory=dict)
    notes: List[str] = field(default_factory=list)
    blocked_by: str = ""  # a precondition the bench did not meet: every property BLOCKED


@dataclass
class Bounds:
    """The leg's table for the release candidate: the most each counter may reach."""

    max_paid_turns: int
    max_provider_requests: int
    max_identical: int
    notices: Tuple[int, int] = (1, 1)  # (least, most) owner notices for the episode
    max_parked_requests: int = 0
    usable_within_s: float = 180.0


@dataclass
class Verdict:
    status: str
    detail: str

    def __str__(self) -> str:
        return f"{self.status} ({self.detail})" if self.detail else self.status


def _bounded(value: Optional[int], most: int, what: str) -> Verdict:
    if value is None:
        return Verdict(BLOCKED, f"{what} not measured")
    return Verdict(PASS if value <= most else FAIL, f"{what} {value} (at most {most})")


def _worst(*vs: Verdict) -> Verdict:
    for status in (FAIL, BLOCKED):
        hit = [v for v in vs if v.status == status]
        if hit:
            return Verdict(status, "; ".join(v.detail for v in hit))
    return Verdict(PASS, "; ".join(v.detail for v in vs))


def grade(m: Metrics, b: Bounds) -> Dict[str, Verdict]:
    """The four acceptance properties of one run against the leg's bounds."""
    if m.blocked_by:
        return {p: Verdict(BLOCKED, m.blocked_by) for p in PROPERTIES}
    if m.usable is None:
        usable = Verdict(BLOCKED, "no fresh owner message was judged")
    elif not m.usable:
        usable = Verdict(FAIL, "the fresh owner message got no answer and no honest refusal")
    elif m.usable_s is not None and m.usable_s > b.usable_within_s:
        usable = Verdict(FAIL, f"answered after {m.usable_s:.0f} s (bound {b.usable_within_s:.0f} s)")
    else:
        took = f" in {m.usable_s:.0f} s" if m.usable_s is not None else ""
        usable = Verdict(PASS, f"answered{took}")
    no_loop = _worst(
        _bounded(m.paid_turns, b.max_paid_turns, "paid turns"),
        _bounded(m.identical_peak, b.max_identical, "identical log lines"),
        _bounded(m.notices, b.notices[1], "notices"),
    )
    capped = _worst(
        _bounded(m.provider_requests, b.max_provider_requests, "provider requests"),
        _bounded(m.parked_requests, b.max_parked_requests, "requests to a parked provider"),
    )
    if m.notices is None:
        told = Verdict(BLOCKED, "owner notices not measured")
    else:
        lo, hi = b.notices
        ok = lo <= m.notices <= hi
        want = f"exactly {lo}" if lo == hi else f"{lo} to {hi}"
        told = Verdict(PASS if ok else FAIL, f"{m.notices} notice(s) ({want})")
    return {"usable": usable, "no_loop": no_loop, "capped": capped, "told_once": told}


def overall(verdicts: Dict[str, Verdict]) -> str:
    statuses = [v.status for v in verdicts.values()]
    if FAIL in statuses:
        return FAIL
    if BLOCKED in statuses:
        return BLOCKED
    return PASS


# ---- the before/after table -----------------------------------------------------------

TABLE_ROWS: Tuple[Tuple[str, str], ...] = (
    ("paid turns", "paid_turns"),
    ("provider requests", "provider_requests"),
    ("requests to a parked provider", "parked_requests"),
    ("owner notices", "notices"),
    ("identical log lines (peak)", "identical_peak"),
    ("fresh message answered", "usable"),
    ("answered within (s)", "usable_s"),
)


def _cell(v) -> str:
    if v is None:
        return "-"
    if isinstance(v, bool):
        return "yes" if v else "no"
    if isinstance(v, float):
        return f"{v:.0f}"
    return str(v)


def _bound_cell(attr: str, b: Optional[Bounds]) -> str:
    if b is None:
        return "-"
    return {
        "paid_turns": f"<= {b.max_paid_turns}",
        "provider_requests": f"<= {b.max_provider_requests}",
        "parked_requests": f"<= {b.max_parked_requests}",
        "notices": f"{b.notices[0]}" if b.notices[0] == b.notices[1] else f"{b.notices[0]}-{b.notices[1]}",
        "identical_peak": f"<= {b.max_identical}",
        "usable": "yes",
        "usable_s": f"<= {b.usable_within_s:.0f}",
    }[attr]


def _measure_rows(control: Optional[Metrics], candidate: Optional[Metrics], bounds: Optional[Bounds]) -> list:
    def side(m: Optional[Metrics], attr: str) -> str:
        return _cell(getattr(m, attr)) if m else "not run"

    return [
        (label, side(control, attr), side(candidate, attr), _bound_cell(attr, bounds)) for label, attr in TABLE_ROWS
    ]


def _verdict_rows(graded: Dict[str, Optional[Dict[str, Verdict]]]) -> Tuple[list, List[str]]:
    """The four property rows and the grade row of both images, and one detail line per
    graded property."""

    def status(name: str, prop: str) -> str:
        vs = graded[name]
        return vs[prop].status if vs else "-"

    rows = [(prop, status("v4.5.13", prop), status("candidate", prop), "the leg's table") for prop in PROPERTIES]
    rows.append(("grade", *(overall(graded[n]) if graded[n] else "-" for n in ("v4.5.13", "candidate")), ""))
    details = [f"{name} {prop}: {vs[prop]}" for name, vs in graded.items() if vs for prop in PROPERTIES]
    return rows, details


def _render(rows: list) -> List[str]:
    widths = [max(len(r[i]) for r in rows) for i in range(4)]
    out = []
    for i, r in enumerate(rows):
        out.append("  ".join(c.ljust(widths[k]) for k, c in enumerate(r)).rstrip())
        if i == 0:
            out.append("  ".join("-" * w for w in widths))
    return out


def table(leg: str, control: Optional[Metrics], candidate: Optional[Metrics], bounds: Optional[Bounds]) -> str:
    """The before/after table of one leg: each counter on v4.5.13 and on the release
    candidate, with the candidate's bound and the verdicts."""
    sides = (("v4.5.13", control), ("candidate", candidate))
    rows = [("measure", "v4.5.13", "candidate", "bound")] + _measure_rows(control, candidate, bounds)
    details: List[str] = []
    if bounds is not None:
        more, details = _verdict_rows({n: grade(m, bounds) if m is not None else None for n, m in sides})
        rows += more
    lines = [f"storm leg {leg}: before (v4.5.13) and after (release candidate)"] + _render(rows) + details
    lines += [f"note ({name}): {note}" for name, m in sides if m for note in m.notes]
    return "\n".join(lines)


# ---- evidence ----------------------------------------------------------------------------


@dataclass
class RunId:
    """Where and when one leg ran: the leg, the image's role, its build and commit, the
    unit, and the run's start and end (epoch seconds)."""

    leg: str
    role: str
    build: str
    commit: str
    unit: str
    started: float
    finished: float


def evidence_record(run: RunId, metrics: Metrics, bounds: Optional[Bounds]) -> dict:
    """One run's evidence: what ran where, what it measured and how it graded. The
    control run is recorded, never graded (its numbers are the 'before')."""
    vs = grade(metrics, bounds) if (bounds is not None and run.role == CANDIDATE) else {}
    return {
        "schema": EVIDENCE_SCHEMA,
        **asdict(run),
        "finishedUtc": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime(run.finished)),
        "metrics": asdict(metrics),
        "bounds": asdict(bounds) if bounds else None,
        "verdicts": {k: {"status": v.status, "detail": v.detail} for k, v in vs.items()},
        "grade": overall(vs) if vs else "RECORDED",
    }


def write_evidence(directory: str, record: dict) -> str:
    os.makedirs(directory, exist_ok=True)
    stamp = time.strftime("%Y%m%dT%H%M%SZ", time.gmtime(record["finished"]))
    path = os.path.join(directory, f"{record['leg']}-{record['role']}-{stamp}.json")
    with open(path, "w", encoding="utf-8") as f:
        json.dump(record, f, indent=1, sort_keys=True)
        f.write("\n")
    return path


def load_evidence(directory: str) -> List[dict]:
    out = []
    if not os.path.isdir(directory):
        return out
    for name in sorted(os.listdir(directory)):
        if not name.endswith(".json"):
            continue
        try:
            with open(os.path.join(directory, name), encoding="utf-8") as f:
                rec = json.load(f)
        except (OSError, ValueError):
            continue
        if isinstance(rec, dict) and rec.get("schema") == EVIDENCE_SCHEMA:
            out.append(rec)
    return out


def latest(records: Iterable[dict], leg: str, role: str) -> Optional[dict]:
    hits = [r for r in records if r.get("leg") == leg and r.get("role") == role]
    return max(hits, key=lambda r: r.get("finished", 0)) if hits else None


def metrics_of(record: Optional[dict]) -> Optional[Metrics]:
    if not record:
        return None
    m = dict(record.get("metrics") or {})
    known = {f for f in Metrics.__dataclass_fields__}
    return Metrics(**{k: v for k, v in m.items() if k in known})


def bounds_of(record: Optional[dict]) -> Optional[Bounds]:
    if not record or not record.get("bounds"):
        return None
    b = dict(record["bounds"])
    b["notices"] = tuple(b.get("notices") or (1, 1))
    return Bounds(**b)
