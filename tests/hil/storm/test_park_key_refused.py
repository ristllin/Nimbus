"""A provider that rejects its key is parked and asked no more (storm leg c, refused key).

Leg c's shape for a refused key, on the storm kit: with Anthropic the only keyed
provider and every request refused with a 401 (every seam) or a 403 (the chat seam),
each episode is one owner message and one fresh message (the probe). Run on the release
candidate only (PROVFAULT api.anthropic.com <status> <body>; nothing leaves the device).

Bounds per episode: one provider request, zero to the provider once it is parked, the
key notice exactly once ("... rejected its key. Update it under Assistant > Models."),
and the probe answered within 120 s by the one short answer ("Your message was not
answered. Anthropic rejected its key: update it under Assistant > Models.").

Outside the storm suite's leg list (the release gate's storm step does not read it): a
candidate-only check that rides the storm kit. Run it in bot mode (the owner channel
the notice is counted on):
NIMBUS_STORM_UNIT=A NIMBUS_STORM_TG=1 python3 -m pytest tests/hil/storm/test_park_key_refused.py \
    -m "hil or net" --allow-hardware -s
"""

from __future__ import annotations

import json
import re

import pytest

import grading as g
import kit

pytestmark = [pytest.mark.hil, pytest.mark.net]

LEG = "c_key_refused"
HOST = "api.anthropic.com"
SHAPES = (
    ("key-401", 401, {"type": "error", "error": {"type": "authentication_error", "message": "invalid x-api-key"}}),
    (
        "key-403",
        403,
        {
            "type": "error",
            "error": {
                "type": "permission_error",
                "message": "Your API key does not have permission to use the specified resource.",
            },
        },
    ),
)
# The notice only (the short answer reads "rejected its key: update it ..."): told once.
NOTICE_RE = re.compile(r"rejected its key\. Update it under Assistant > Models", re.I)
SHORT_ANSWER = "Your message was not answered. Anthropic rejected its key: update it under Assistant > Models."
# A 401 parks every seam at the wire; a 403 parks the chat seam in the fabric.
PARK_LINE_RE = re.compile(r"provider: anthropic parked for every seam|fabric: anthropic parked \(auth")
BOUNDS = g.Bounds(
    max_paid_turns=2,
    max_provider_requests=1,
    max_identical=5,
    notices=(1, 1),
    max_parked_requests=0,
    usable_within_s=120.0,
)


def requests_after_park(lines) -> int:
    parked, n = False, 0
    for ln in lines:
        if not parked:
            parked = bool(PARK_LINE_RE.search(ln.text))
            continue
        n += 1 if g.OVERRIDE_RE.search(ln.text) else 0
    return n


def _episode(storm, name: str, status: int, body: dict) -> g.Metrics:
    uid = kit.first_update_id()
    storm.verb("PARK clear", "PARK", timeout=6.0)
    out = storm.verb(f"PROVFAULT {HOST} {status} {json.dumps(body, separators=(',', ':'))}", "PROVFAULT")
    assert "armed" in out, out
    try:
        w = kit.Window(storm).open()
        kit.owner_turn(storm, f"park key {name} {uid}: reply with the single word pong", uid)
        probe = storm.fresh_message(kit.ANSWER_RE, f"park key {name} probe {uid}: reply pong", uid + 1, timeout=150.0)
        w.close()
        park = storm.verb("PARK?", "PARK", timeout=6.0).strip()
    finally:
        storm.verb("PROVFAULT off", "PROVFAULT", timeout=6.0)
        storm.verb("PARK clear", "PARK", timeout=6.0)
    peak, line = g.identical_peak(w.lines, ignore=kit.DRIVE_LINES)
    m = g.Metrics(
        paid_turns=g.paid_turns(w.lines),
        provider_requests=w.requests(HOST),
        parked_requests=requests_after_park(w.lines),
        notices=w.notices(NOTICE_RE),
        identical_peak=peak,
        identical_line=line[:160],
        usable=probe["usable"] and probe["reply"].strip() == SHORT_ANSWER,
        usable_s=probe["usable_s"],
    )
    if probe["usable"] and probe["reply"].strip() != SHORT_ANSWER:
        m.notes.append("the probe's reply was not the one short answer")
    m.extra.update({"shape": name, "probeReply": probe["reply"][:200], "park": park})
    return m


def test_park_key_refused(storm):
    assert not storm.control, "a candidate-only leg (the control image has no PROVFAULT)"
    episodes = []
    with storm.session(kit.keep_for(storm), kit.head_for(storm)):
        for name, status, body in SHAPES:
            m = _episode(storm, name, status, body)
            print(f"[storm] episode {name}: {m}")
            episodes.append(m)
    worst = kit.worst_episode(episodes, BOUNDS)
    summary = [{k: v for k, v in vars(e).items() if k not in ("extra", "notes")} for e in episodes]
    for row, e in zip(summary, episodes):
        row["shape"] = e.extra.get("shape")
        row["probeReply"] = e.extra.get("probeReply")
    worst.extra["episodes"] = summary
    kit.assert_graded(storm.finish(LEG, worst, BOUNDS))
