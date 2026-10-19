"""Host tests for the L40/L41 evidence parsers (l41_evidence.py). No device."""

from __future__ import annotations

import pytest

import l41_evidence as ev

TRACE_OK = (
    "transport: heaptrace api.anthropic.com/v1/messages body=71508 pre=24760/11764 "
    "conn=20792/7156 wrote=22128/10228 done=24600/11764 fail=none af=0 min=18000 at=42651042 ms=1712"
)
TRACE_BODY = (
    "transport: heaptrace api.openai.com/v1/responses body=71508 pre=24308/11252 "
    "conn=20000/9000 wrote=15204/7156 done=24000/11252 fail=body@8192 af=1 min=6900 at=1000 ms=2000"
)
TRACE_GET = (
    "transport: heaptrace api.mistral.ai/v1/models body=0 pre=30000/16000 conn=28000/15000 "
    "wrote=0/0 done=29000/16000 fail=none af=0 min=- at=5 ms=10"
)
BULK = "relay: bulk res body=375598 frame=500822 at=42646200 ms=2700 waited=0"


def test_trace_fields_parse():
    t = ev.parse_trace(TRACE_OK)
    assert (t.target, t.body, t.fail, t.af, t.min_free) == ("api.anthropic.com/v1/messages", 71508, "none", 0, 18000)
    assert t.span == (42651042, 42651042 + 1712)
    assert t.min_largest == 7156
    b = ev.parse_trace(TRACE_BODY)
    assert b.fail == "body@8192" and b.af == 1
    g = ev.parse_trace(TRACE_GET)
    assert g.min_free is None
    assert g.min_largest == 15000  # the unsampled (0,0) post-write slot is not a reading


def test_a_renamed_line_fails_loud():
    with pytest.raises(ValueError):
        ev.parse_trace(TRACE_OK.replace("heaptrace", "heap-trace"))
    with pytest.raises(ValueError):
        ev.parse_bulk(BULK.replace("waited", "wait"))
    # the pre-O2 line (no fail=/af=/min=) is not accepted as evidence
    with pytest.raises(ValueError):
        ev.parse_trace(TRACE_OK.split(" fail=")[0])


def test_collect_splits_kinds():
    lines = [
        TRACE_OK,
        BULK,
        "heap: new low-water 9348 (was 10500) free=21000 largest=9716 tags=turn|relay.bulk up=42640s",
        "heap: alloc FAILED size=1600 caps=DMA|INTERNAL task=tg_poll free=15204 largest=7156 up=42660s count=1",
        "tls: overlap relay.bulk (count=1)",
        "fabric: switching openai -> anthropic",
        "relay: res 200 body=375598 waited 20004ms for the TLS slot -> busy",
        "unrelated line",
    ]
    c = ev.collect(lines)
    assert len(c["traces"]) == 1 and len(c["bulks"]) == 1
    assert c["lows"][0]["tags"] == "turn|relay.bulk"
    assert c["fails"][0]["task"] == "tg_poll"
    assert c["overlaps"][0]["count"] == "1"
    assert c["switches"][0] == {"frm": "openai", "to": "anthropic"}
    assert len(c["busy"]) == 1


def test_bulk_overlap_rule():
    t = ev.parse_trace(TRACE_OK)  # 42651042 .. +1712
    inside = ev.parse_bulk("relay: bulk res body=375598 frame=500822 at=42650000 ms=5000 waited=0")
    assert ev.bulk_overlaps([t], [inside]) == [(inside, t)]  # a page written across an exchange
    yielded = ev.parse_bulk("relay: bulk res body=375598 frame=500822 at=42650000 ms=5000 waited=1800")
    assert ev.bulk_overlaps([t], [yielded]) == []  # it waited out the exchange between chunks
    apart = ev.parse_bulk("relay: bulk res body=375598 frame=500822 at=42653000 ms=2700 waited=0")
    assert ev.bulk_overlaps([t], [apart]) == []


def test_ladder_rule():
    ok = [{"mark": "20000"}, {"mark": "18976"}, {"mark": "9000"}]
    assert ev.ladder_ok(ok)
    assert not ev.ladder_ok([{"mark": "20000"}, {"mark": "19500"}])


def test_tlsf_classes_match_the_field_values():
    # 11,764 and 11,252 are neighbouring classes (one step); 15,860 vs 11,764 is not.
    assert ev.within_one_class(11764, 11252)
    assert ev.within_one_class(15860, 15860)
    assert not ev.within_one_class(15860, 11764)
    assert not ev.within_one_class(11764, 9716)
