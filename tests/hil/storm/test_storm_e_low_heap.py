"""Storm leg e: low internal memory during a turn and during the nightly upkeep.

Reproduces 19 Oct 2026 (L19-15, L19-16, L19-34): six owner messages failed between
09:12Z and 09:34Z because the request could not be written with the largest free
internal block at 7 KB, and the owner was told to check the connection; the 02:30Z
upkeep ran eight rounds with internal memory tight and was cut by a reset.

Drive: ``FAULT sram_ballast`` holds internal memory until about 14 KB is free, then one
owner message and the upkeep drill (``DREAM``) run under it; the ballast is released and
one fresh message probes that the device is usable again. The candidate's head is
Anthropic ``NIMBUS_STORM_MODEL``; the v4.5.13 control runs the bench control image built
with the same ballast verb backported to its test console (bench/wt/STORM-v4513-ballast,
nothing else changed) and the LAN mock as its head. Every other provider key is cleared.
Unit A.

Bounds on the release candidate: at most three turns (the message and the upkeep's two
phases), at most twelve head requests, no request written with 0 bytes sent more than
once, exactly one owner message about it when the owner's turn failed (none needed when
it got through), the upkeep finishing with a result, no log line repeated more than 5
times, the probe answered within 180 s of the release.

Run: NIMBUS_STORM_UNIT=A python3 -m pytest tests/hil/storm/test_storm_e_low_heap.py --allow-hardware -s
"""

from __future__ import annotations

import re
import time

import pytest

import grading as g
import kit

pytestmark = [pytest.mark.hil, pytest.mark.net]

LEG = "e_low_heap"
LEAVE_FREE = 14000  # internal bytes left free under the ballast
NOTICE_RE = re.compile(r"memory|connection|network|try again|could not|couldn.t|didn.t go through", re.I)
ZERO_WRITE_RE = re.compile(r"short body write 0/|after 0 of \d+ bytes|not sent \(first write\)")
BALLAST_RE = re.compile(r"FAULT sram_ballast=(\d+) held=(\d+) intFree=(\d+) intLargest=(\d+)")
BOUNDS = g.Bounds(max_paid_turns=3, max_provider_requests=12, max_identical=5, notices=(0, 1), usable_within_s=180.0)
# When the owner's turn failed under the ballast, the owner must hear exactly once why.
BOUNDS_FAILED = g.Bounds(
    max_paid_turns=3, max_provider_requests=12, max_identical=5, notices=(1, 1), usable_within_s=180.0
)


def _internal_free(storm) -> int:
    out = storm.verb("FAULT sram_ballast 0", "FAULT sram_ballast", timeout=8.0)
    m = BALLAST_RE.search(out)
    if not m:
        pytest.fail(f"this image has no internal-memory ballast: {out.strip()!r}")
    return int(m.group(3))


DREAM_DONE_RE = re.compile(r"dream: \S+.* tokens=\d+|dream: skipped tonight|dream: not idle")


def _dream(storm, timeout: float = 420.0) -> str:
    """The upkeep drill. v4.5.13 runs it on the console and answers when it is done; the
    candidate queues it for the turn task and logs a result line."""
    try:
        out = storm.verb("DREAM", "DREAM", timeout=timeout).strip()
    except Exception as e:  # noqa: BLE001 - an upkeep that never answers is the finding, not a crash
        return f"DREAM gave no answer within {timeout:.0f} s ({type(e).__name__})"
    if "queued" not in out:
        return out
    end = time.time() + timeout
    while time.time() < end:
        done = [ln for ln in storm.tap.poll() if DREAM_DONE_RE.search(ln)]
        if done:
            return f"{out} | {done[-1]}"
        time.sleep(3.0)
    return f"{out} | no result line within {timeout:.0f} s"


def test_storm_e_low_heap(storm):
    uid = kit.first_update_id()
    with storm.session(kit.keep_for(storm), kit.head_for(storm)):
        free = _internal_free(storm)
        hold = max(0, free - LEAVE_FREE)
        # The window opens before the ballast and closes after its release: under the
        # ballast the history route itself answers busy (unit A, 22 Oct).
        w = kit.Window(storm).open()
        out = storm.verb(f"FAULT sram_ballast {hold}", "FAULT sram_ballast", timeout=10.0)
        held = BALLAST_RE.search(out)
        print(f"[storm] ballast: {out.strip()}")
        try:
            kit.owner_turn(storm, f"storm e {uid}: reply with the single word pong", uid, timeout=240.0)
            dream = _dream(storm)
        finally:
            storm.verb("FAULT sram_ballast 0", "FAULT sram_ballast", timeout=10.0)
        w.close()
        probe = storm.fresh_message(kit.ANSWER_RE, f"storm e probe {uid}: reply pong", uid + 1)
    replies = w.replies(NOTICE_RE)
    zero = [ln.text for ln in w.lines if ZERO_WRITE_RE.search(ln.text)]
    peak, line = g.identical_peak(w.lines, ignore=kit.DRIVE_LINES)
    m = g.Metrics(
        paid_turns=g.paid_turns(w.lines),
        provider_requests=w.requests(),
        parked_requests=0,
        notices=w.notices(NOTICE_RE),
        identical_peak=max(peak, len(zero)),
        identical_line=(line if peak >= len(zero) else (zero[0] if zero else ""))[:160],
        usable=probe["usable"],
        usable_s=probe["usable_s"],
    )
    m.extra.update(
        {
            "ballast": out.strip(),
            "intFreeBefore": free,
            "intLargestUnder": int(held.group(4)) if held else None,
            "zeroByteWrites": zero[:5],
            "zeroByteWriteCount": len(zero),
            "dream": dream[:300],
            "dreamFinished": "no answer" not in dream and "no result line" not in dream,
            "replies": replies,
            "probeReply": probe["reply"][:200],
        }
    )
    if not m.extra["dreamFinished"]:
        # A device whose nightly upkeep never finishes is not usable, whatever the probe
        # got (both images are measured the same way).
        m.notes.append(f"the upkeep did not finish under low memory: {dream[:120]}")
        m.usable = False
    failed = g.owner_turn_failed(w.lines)
    m.extra["ownerTurnFailed"] = failed
    kit.assert_graded(storm.finish(LEG, m, BOUNDS_FAILED if failed else BOUNDS))
