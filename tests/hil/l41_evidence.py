"""Parsers for the internal-SRAM evidence lines (L40/L41 and the O2 bench runs).

Host-testable (test_l41_evidence_host.py); no device access. Each parser raises on a
line of another shape, so a renamed log line fails loud instead of passing vacuously.

Lines (firmware v4.5.14):
  transport: heaptrace <host><path> body=N pre=F/L conn=F/L wrote=F/L done=F/L
      fail=<none|connect|head|body@N|read|headers|parse> af=N min=<N|-> at=MS ms=MS
  relay: bulk res body=N frame=N at=MS ms=MS waited=MS
  relay: res STATUS body=N sent=<0|1>
  relay: res STATUS body=N waited MSms for the TLS slot -> busy
  heap: new low-water N (was <N|none>) free=N largest=N tags=<a|b|-> up=Ns
  heap: alloc FAILED size=N caps=<CAPS> task=<name> free=N largest=N up=Ns count=N
  tls: overlap <who> (count=N)
"""

from __future__ import annotations

import re
from dataclasses import dataclass
from pathlib import Path
from typing import Iterable, List, Optional, Tuple

# An answer whose body is over this many bytes takes the TLS work slot; smaller ones
# (the dashboard's state polls) never do. Mirrors kRelayBulkBytes in
# lib/core/include/nimbus/cloud/relay_slot.h (relay_bulk_bytes_in_source reads it).
RELAY_BULK_BYTES = 8192
_RELAY_SLOT_H = Path(__file__).resolve().parents[2] / "lib/core/include/nimbus/cloud/relay_slot.h"

TRACE_RE = re.compile(
    r"transport: heaptrace (?P<target>\S+) body=(?P<body>\d+) "
    r"pre=(?P<pre_f>\d+)/(?P<pre_l>\d+) conn=(?P<conn_f>\d+)/(?P<conn_l>\d+) "
    r"wrote=(?P<wrote_f>\d+)/(?P<wrote_l>\d+) done=(?P<done_f>\d+)/(?P<done_l>\d+) "
    r"fail=(?P<fail>[a-z]+(?:@\d+)?) af=(?P<af>\d+) min=(?P<min>\d+|-) at=(?P<at>\d+) ms=(?P<ms>\d+)"
)
BULK_RE = re.compile(
    r"relay: bulk res body=(?P<body>\d+) frame=(?P<frame>\d+) at=(?P<at>\d+) ms=(?P<ms>\d+) waited=(?P<waited>\d+)"
)
LOW_RE = re.compile(
    r"heap: new low-water (?P<mark>\d+) \(was (?P<was>\d+|none)\) free=(?P<free>\d+) "
    r"largest=(?P<largest>\d+) tags=(?P<tags>\S+) up=(?P<up>\d+)s"
)
FAIL_RE = re.compile(
    r"heap: alloc FAILED size=(?P<size>\d+) caps=(?P<caps>\S+) task=(?P<task>\S+) "
    r"free=(?P<free>\d+) largest=(?P<largest>\d+) up=(?P<up>\d+)s"
)
RES_RE = re.compile(r"relay: res (?P<status>\d+) body=(?P<body>\d+) sent=(?P<sent>[01])")
SLOT_BUSY_RE = re.compile(
    r"relay: res (?P<status>\d+) body=(?P<body>\d+) waited (?P<ms>\d+)ms for the TLS slot -> busy"
)
OVERLAP_RE = re.compile(r"tls: overlap (?P<who>\S+) \(count=(?P<count>\d+)\)")
SWITCH_RE = re.compile(r"fabric: switching (?P<frm>\S+) -> (?P<to>\S+)")
BUSY_RE = re.compile(r"relay: .*-> busy")


@dataclass
class Trace:
    target: str
    body: int
    fail: str
    af: int
    min_free: Optional[int]
    at: int
    ms: int
    min_largest: int

    @property
    def span(self) -> Tuple[int, int]:
        return (self.at, self.at + self.ms)


@dataclass
class Bulk:
    body: int
    frame: int
    at: int
    ms: int
    waited: int

    @property
    def span(self) -> Tuple[int, int]:
        return (self.at, self.at + self.ms)


def parse_trace(line: str) -> Trace:
    m = TRACE_RE.search(line)
    if not m:
        raise ValueError(f"not a heaptrace line: {line!r}")
    lows = [int(m[k]) for k in ("pre_l", "conn_l", "wrote_l", "done_l")]
    frees = [int(m[k]) for k in ("pre_f", "conn_f", "wrote_f", "done_f")]
    # (0,0) is "not sampled" (a body-less request has no post-write sample).
    taken = [lg for lg, fr in zip(lows, frees) if lg or fr]
    return Trace(
        target=m["target"],
        body=int(m["body"]),
        fail=m["fail"],
        af=int(m["af"]),
        min_free=None if m["min"] == "-" else int(m["min"]),
        at=int(m["at"]),
        ms=int(m["ms"]),
        min_largest=min(taken) if taken else 0,
    )


def parse_bulk(line: str) -> Bulk:
    m = BULK_RE.search(line)
    if not m:
        raise ValueError(f"not a relay bulk line: {line!r}")
    return Bulk(int(m["body"]), int(m["frame"]), int(m["at"]), int(m["ms"]), int(m["waited"]))


def collect(lines: Iterable[str]):
    """Split a captured serial/log stream into the evidence kinds."""
    out = {"traces": [], "bulks": [], "res": [], "lows": [], "fails": [], "overlaps": [], "switches": [], "busy": []}
    for ln in lines:
        if TRACE_RE.search(ln):
            out["traces"].append(parse_trace(ln))
        elif BULK_RE.search(ln):
            out["bulks"].append(parse_bulk(ln))
        elif RES_RE.search(ln):
            m = RES_RE.search(ln)
            out["res"].append({"status": int(m["status"]), "body": int(m["body"]), "sent": m["sent"] == "1"})
        elif LOW_RE.search(ln):
            out["lows"].append(LOW_RE.search(ln).groupdict())
        elif FAIL_RE.search(ln):
            out["fails"].append(FAIL_RE.search(ln).groupdict())
        elif OVERLAP_RE.search(ln):
            out["overlaps"].append(OVERLAP_RE.search(ln).groupdict())
        elif SWITCH_RE.search(ln):
            out["switches"].append(SWITCH_RE.search(ln).groupdict())
        elif BUSY_RE.search(ln):
            out["busy"].append(ln)
    return out


def overlapping(a: Tuple[int, int], b: Tuple[int, int]) -> bool:
    return a[0] < b[1] and b[0] < a[1]


def bulk_overlaps(traces: List[Trace], bulks: List[Bulk]) -> List[Tuple[Bulk, Trace]]:
    """Big relay frames whose whole span contains a provider exchange. Inside the
    work slot the relay gives the slot back between 8 KB chunks, so a span can hold
    an exchange only if the frame waited for it: an overlap longer than the frame's
    own wait is a real concurrent write."""
    bad = []
    for bk in bulks:
        for tr in traces:
            if not overlapping(bk.span, tr.span):
                continue
            inter = min(bk.span[1], tr.span[1]) - max(bk.span[0], tr.span[0])
            if inter > bk.waited:
                bad.append((bk, tr))
    return bad


def small_answers_in_the_slot(c: dict) -> List[str]:
    """Answers with a body of at most RELAY_BULK_BYTES that went through the TLS work
    slot (a bulk frame) or waited for it and were answered busy. The slot rule is on
    the body, so a state poll (6-7 KB, an 8.5-9.5 KB frame) must never appear here."""
    bad = [f"bulk body={b.body} frame={b.frame} waited={b.waited}" for b in c["bulks"] if b.body <= RELAY_BULK_BYTES]
    for ln in c["busy"]:
        m = SLOT_BUSY_RE.search(ln)
        if m and int(m["body"]) <= RELAY_BULK_BYTES:
            bad.append(f"busy body={m['body']} after {m['ms']} ms waiting for the slot")
    return bad


def relay_bulk_bytes_in_source() -> int:
    """kRelayBulkBytes as relay_slot.h defines it (raises if the line moved)."""
    m = re.search(r"kRelayBulkBytes\s*=\s*(\d+)\s*;", _RELAY_SLOT_H.read_text())
    if not m:
        raise ValueError("kRelayBulkBytes is gone from relay_slot.h")
    return int(m[1])


def ladder_ok(lows: List[dict]) -> bool:
    """The logged low-water marks fall strictly, by at least 1 KB each."""
    marks = [int(x["mark"]) for x in lows]
    return all(b + 1024 <= a for a, b in zip(marks, marks[1:]))


# The TLSF size classes of the internal heap. ESP-IDF's TLSF picks its second-level
# split per region size (heap/tlsf/tlsf.c: 8 classes per power of two up to 16 KB
# regions, 16 up to 256 KB, 32 above); the internal DRAM regions are under 256 KB, so
# 16 classes: 512 B wide between 8 and 16 KB. The field values (16,372 / 11,764 /
# 11,252 / 7,156) are class floors less the 12 B of light heap poisoning.
_SL_LOG2 = 4
_POISON = 12


def tlsf_class(n: int) -> int:
    n = n + _POISON
    if n <= 0:
        return 0
    fl = n.bit_length() - 1
    if fl < _SL_LOG2:
        return n
    sl = (n >> (fl - _SL_LOG2)) ^ (1 << _SL_LOG2)
    return (fl << _SL_LOG2) | sl


def within_one_class(a: int, b: int) -> bool:
    return abs(tlsf_class(a) - tlsf_class(b)) <= 1
