"""§L60 - the cacheable head prompt on real hardware (prompt_layout.h).

Two bench legs for the rules the host suites pin (test_prompt_layout,
test_file_append, test_orch_transcript):

1. Two head turns about a minute apart that differ only in per-turn content: the
   second turn's round 0 reads at least 90% of the cached prefix (the tools, the
   static block and the summary block) from the provider cache. Both turns are
   one request ("no tools"), so the turn's ``turn spend:`` usage IS round 0. The prefix size
   is the turn's provider-reported input minus the per-turn text (the per-turn
   block plus the user message, read back from /api/lastturn and counted with
   Anthropic's free count_tokens endpoint).
2. A food-log turn: "log my lunch" ends in at most 3 requests (headloop rounds)
   and appends the row with files.append, never a whole-file artifact.save.
3. The 19 Oct replay shape (L19-45): the same three-word owner text twice,
   30 s apart. The first reply moves the device history the second turn's
   prompt carries (the running-memory note, the recent window), so the second
   turn is a replay whose per-turn block changed. Turn 2 stays one request (its
   ``turn spend:`` usage IS round 0) and must read at least 90% of the golden
   prefix from the cache and write under 1,000 tokens to it - the shape that
   re-wrote 10-11K cached tokens per replay on 19 Oct.

Each turn is read to its own answer (``/api/chat?turn=<id>``) while /api/log is
followed and stitched (the RAM log is a 1,280-byte ring, less than one turn). The
usage comes from the ``turn spend:`` line (since v4.5.14 the ``turn ->`` line
carries no token counts).

Markers: ``net`` + ``agent`` (real provider calls on the head, about $0.03 on
claude-haiku-5-5). Needs --allow-hardware, NIMBUS_TEST_IP and NIMBUS_TEST_TOKEN,
an Anthropic head on claude-haiku-5-5, an SD card, and ANTHROPIC_API_KEY in the
environment of the bench host (count_tokens only; it never reaches the device).
"""

from __future__ import annotations

import json
import os
import re
import time

import pytest

from webrig import LogFollower, chat_turn, ip_tok_or_skip, make_session, require_orchestrator

pytestmark = [pytest.mark.net, pytest.mark.agent]

_S = None
RUN = time.strftime("%H%M%S", time.gmtime())  # makes this run's lunch a new row: the file outlives the run
SPEND_LINE = re.compile(r"turn spend: ok \S+ \| anthropic/\S+ in=(\d+) out=(\d+) cR=(\d+) cW=(\d+) n=(\d+)")
SPLIT = "[----- cached blocks end; per-turn block -----]\n"


def _chat(ip: str, text: str, timeout_s: int = 150):
    """One turn to its own answer; return (reply, the /api/log lines it printed)."""
    with LogFollower(_S, ip) as follow:
        reply = chat_turn(_S, ip, text, timeout_s=timeout_s)
    return reply, follow.text


def _turn_usage(log: str, one_request: bool = True) -> dict:
    """The turn's Anthropic usage. The measured turn must be one request, so its
    usage IS round 0; a warm-up turn may take more (it is only printed)."""
    hits = SPEND_LINE.findall(log)
    assert hits, "no 'turn spend: ok ... anthropic/' line in /api/log for the turn"
    i, o, r, w, n = (int(x) for x in hits[-1])
    if one_request:
        assert n == 1, f"the turn made {n} Anthropic requests - the round-0 check needs a one-request turn, re-run"
    return {"in": i, "out": o, "cacheR": r, "cacheW": w, "total": i + r + w, "requests": n}


def _per_turn_text(ip: str) -> str:
    """The per-turn block plus the per-turn input of the last turn (/api/lastturn)."""
    page = _S.get(f"http://{ip}/api/lastturn", timeout=10).text
    assert SPLIT in page, "/api/lastturn shows no layout split - is this build older than the layout?"
    dyn = page.split(SPLIT, 1)[1].split("===== 2.", 1)[0]
    inp = page.split("===== 2.", 1)[1].split("===== 3.", 1)[0]
    return dyn + "\n\n" + inp


def _history_line(ip: str) -> str:
    """The 'device history this turn:' line of the last turn's anatomy header
    (L19-28): what the device itself sent as history in the system prompt."""
    page = _S.get(f"http://{ip}/api/lastturn", timeout=10).text
    m = re.search(r"^device history this turn: (.*)$", page, re.M)
    assert m, "/api/lastturn shows no device history line - is this build older than the L19-28 header?"
    return m.group(1)


def _count_tokens(model: str, text: str) -> int:
    import requests

    key = os.environ.get("ANTHROPIC_API_KEY")
    if not key:
        pytest.skip("set ANTHROPIC_API_KEY on the bench host (count_tokens only)")
    r = requests.post(
        "https://api.anthropic.com/v1/messages/count_tokens",
        headers={"x-api-key": key, "anthropic-version": "2023-06-01", "content-type": "application/json"},
        data=json.dumps({"model": model, "messages": [{"role": "user", "content": text}]}),
        timeout=30,
    )
    assert r.status_code == 200, f"count_tokens {r.status_code}: {r.text[:200]}"
    return int(r.json()["input_tokens"])


@pytest.fixture(scope="module")
def board():
    global _S
    ip, tok = ip_tok_or_skip("L60")
    _S = make_session(tok)
    require_orchestrator(_S, ip, "L60")
    orch = _S.get(f"http://{ip}/api/orch", timeout=10).json()
    host = orch.get("orchHost") or (orch.get("provPrio") or "").split(",")[0].strip()
    model = ((orch.get("providers") or {}).get(host) or {}).get("orchModel") or ""
    if host != "anthropic" or "haiku" not in model:
        pytest.skip(f"L60 needs an Anthropic haiku head (found {host!r} / {model!r})")
    return ip, model


def test_l60_second_turn_reads_the_static_prefix(board):
    ip, model = board
    _, log1 = _chat(ip, "In one short line and without tools: name a light lunch.")
    first = _turn_usage(log1, one_request=False)
    time.sleep(60)  # a minute apart: the minute clock changed, the static block did not
    _, log2 = _chat(ip, "Thanks. One more one-line idea, again without tools.")
    second = _turn_usage(log2)
    per_turn = _count_tokens(model, _per_turn_text(ip))
    prefix = second["total"] - per_turn
    print(f"L60 turn1={first} turn2={second} perTurnTokens={per_turn} prefixTokens={prefix}")
    assert prefix > 1024, f"implausible cached prefix size {prefix}"
    assert second["cacheR"] >= 0.9 * prefix, (
        f"turn 2 round 0 read {second['cacheR']} of a {prefix}-token static prefix (< 90%)"
    )


def test_l60_replayed_turn_hits_the_prefix_cache(board):
    """L19-45: the 19 Oct replay shape on the bench. The same three-word owner
    text, 30 s apart; the first reply moves the device history the second
    turn's prompt carries. Turn 2 must stay one round (the turn line IS round
    0) and read at least 90% of the golden prefix while writing under 1,000
    tokens to the cache. The exact running-memory change between the pair is
    pinned by the live leg (prompt-cache-live cache-replay), which controls
    the note; here it is reported, and the history line as a whole must move
    (the recent window always grows by the first exchange)."""
    ip, model = board
    text = "thanks, same again"  # three words, the same on both turns
    _, log1 = _chat(ip, text)
    first = _turn_usage(log1, one_request=False)
    hist1 = _history_line(ip)
    time.sleep(30)  # the replay gap of the 19 Oct incident
    _, log2 = _chat(ip, text)
    second = _turn_usage(log2)
    hist2 = _history_line(ip)
    rounds = re.findall(r"headloop round=(\d+)", log2)
    assert rounds and len(rounds) == 1, (
        f"the replayed turn took {len(rounds)} headloop rounds - the round-0 check "
        "needs a tool-free turn, re-run the leg"
    )
    per_turn = _count_tokens(model, _per_turn_text(ip))
    prefix = second["total"] - per_turn
    print(
        f"L60 replay turn1={first} turn2={second} perTurnTokens={per_turn} "
        f"prefixTokens={prefix}\nhistory turn1: {hist1}\nhistory turn2: {hist2}"
    )
    assert prefix > 1024, f"implausible cached prefix size {prefix}"
    assert second["cacheR"] >= 0.9 * prefix, (
        f"replayed turn 2 round 0 read {second['cacheR']} of a {prefix}-token golden prefix (< 90%)"
    )
    assert second["cacheW"] < 1000, f"replayed turn 2 round 0 wrote {second['cacheW']} tokens to the cache (>= 1000)"
    assert hist1 != hist2, "the device history in the prompt did not move between the replays"


def test_l60_food_log_turn_appends_in_three_requests(board):
    ip, _ = board
    reply, log = _chat(
        ip,
        "Log my lunch to l60-bench/food-log.csv (columns date,meal,kcal,protein_g): "
        f"lentil soup with bread (bench run {RUN}), about 520 kcal and 24 g protein.",
        timeout_s=200,
    )
    rounds = re.findall(r"headloop round=(\d+)", log)
    print(f"L60 food-log rounds={rounds} reply={reply[:160]!r}")
    assert rounds, "no headloop rounds in /api/log (is the tool loop on?)"
    assert len(rounds) <= 3, f"the food-log turn took {len(rounds)} requests (> 3)"
    assert "files: appended" in log, "files.append never ran (no 'files: appended' line)"
    assert "files: artifact l60-bench/food-log.csv" not in log, "the log was re-saved whole"
