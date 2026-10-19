"""Parsers for the internal-SRAM evidence lines (L40/L41 and the O2 bench runs).

Host-testable (test_l41_evidence_host.py); no device access. Each parser raises on a
line of another shape, so a renamed log line fails loud instead of passing vacuously.

Lines (firmware v4.5.14):
  transport: heaptrace <host><path> body=N pre=F/L conn=F/L wrote=F/L done=F/L
      fail=<none|connect|head|body@N|read|headers|parse> af=N min=<N|-> at=MS ms=MS
  relay: bulk res body=N frame=N at=MS ms=MS waited=MS
  heap: new low-water N (was <N|none>) free=N largest=N tags=<a|b|-> up=Ns
  heap: alloc FAILED size=N caps=<CAPS> task=<name> free=N largest=N up=Ns count=N
  tls: overlap <who> (count=N)
"""

from __future__ import annotations

import re
from dataclasses import dataclass
from typing import Iterable, List, Optional, Tuple

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
    out = {"traces": [], "bulks": [], "lows": [], "fails": [], "overlaps": [], "switches": [], "busy": []}
    for ln in lines:
        if TRACE_RE.search(ln):
            out["traces"].append(parse_trace(ln))
        elif BULK_RE.search(ln):
            out["bulks"].append(parse_bulk(ln))
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
