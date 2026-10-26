"""L78 - the v4.6.1 hotfix legs: the owner first after an update, a long tool loop
that still answers, and a fold with no false alarm.

Reproduces Lumi on v4.6.0 (26 Oct 2026, first hour): the post-update awareness turn ran
the 8-round loop for four minutes, a fold followed in the same pass and the owner's
message waited 5.5 minutes; that turn's 8th request outgrew the 85 KB body budget and
the whole turn failed; right after a completed fold the owner was told compaction had
paused because the chat "kept refilling".

Legs (each runnable alone, graded from the device log and the owner chat):
  a  OTASIM arm fires the real mark-valid path (EvValidated stages the awareness turn);
     an owner message 5 s later is answered at normal latency, BEFORE the awareness
     turn starts, with no fold in between; the awareness turn runs bounded (2 rounds,
     or single-shot on a fallback head).
  b  a long tool loop on Mistral (mistral-small) reads a 64 KB file page by page; when
     its requests reach the 85 KB body budget the loop folds older rounds and the owner
     gets an answer (no "request body over" failure, no body over the budget sent).
  c  with compactKB at its floor (8), ten ordinary turns make a fold due; it runs
     after two quiet minutes, three more turns follow, and no compaction-paused
     notice reaches the owner.

Run (candidate test-cyd build, one unit, its lock held):
  NIMBUS_PORT=... NIMBUS_TEST_IP=... NIMBUS_TEST_TOKEN=... NIMBUS_STORM_UNIT=<A|L> \\
  NIMBUS_STORM_EVIDENCE=<lane>/evidence/hil NIMBUS_ENV_FILE=<nimbus>/.env PYTHONUNBUFFERED=1 \\
  python3 -m pytest tests/hil/test_l78_owner_first.py -m "hil or net" --allow-hardware -s
Spend: legs a and c on claude-haiku-5-5 (a few cents), leg b on mistral-small-latest.
"""

from __future__ import annotations

import contextlib
import json
import os
import re
import time

import pytest
import requests

pytestmark = [pytest.mark.hil, pytest.mark.net]

LEG = "l78_owner_first"
OWNER_END_RE = re.compile(r"turn end: \w+ .*src=owner")
BOUNDED_RE = re.compile(r"tool-loop ON \(\d+ tools, 2 rounds/|tool-loop OFF - a bounded turn")
SYSTEM_END_RE = re.compile(r"turn end: |stayed silent")
FOLD_RE = re.compile(r"fold\([^)]*\): ok")
REFIT_RE = re.compile(r"headloop: round \d+ body \d+ B over the 85000 B budget - folded (\d+) B")
OVER_FAIL_RE = re.compile(r"turn failed \(request body over")
BODY_RE = re.compile(r"heaptrace \S+ body=(\d+)")
PAUSED_RE = re.compile(r"summaries are paused|compaction paused|kept refilling", re.I)
KEEP_RE = re.compile(r"turn end|tool-loop|fold\(|Updated to|stayed silent|OTA|headloop|heaptrace|fabric")
MISTRAL_SMALL = "mistral-small-latest"
BUDGET = 85000


def _write(tag: str, rec: dict) -> None:
    root = os.environ.get("NIMBUS_STORM_EVIDENCE", "")
    print(json.dumps({k: v for k, v in rec.items() if k != "lines"}, indent=1))
    if not root:
        return
    os.makedirs(root, exist_ok=True)
    with open(os.path.join(root, f"l78_{tag}_{int(time.time())}.json"), "w") as f:
        json.dump(rec, f, indent=1)


def _first(lines, rx):
    return next((i for i, ln in enumerate(lines) if rx.search(ln)), None)


def _kept(lines):
    return [ln for ln in lines if KEEP_RE.search(ln)][:120]


def _collect(storm, seconds, stop):
    """Log lines from the tap for up to `seconds`, or until `stop(lines)` holds."""
    lines, end = [], time.time() + seconds
    while time.time() < end and not stop(lines):
        lines += storm.tap.poll()
        time.sleep(3.0)
    return lines


def _new_replies(storm, before, timeout=180.0):
    end = time.time() + timeout
    while time.time() < end:
        rows = [r for r in storm.chat_rows(80).values() if str(r.get("id")) not in before]
        replies = [r for r in rows if r.get("role") == "assistant"]
        if replies:
            return replies
        time.sleep(3.0)
    return []


# ---- leg a: the owner first after an update -------------------------------------------
def _system_turn_done(lines):
    at = _first(lines, BOUNDED_RE)
    return at is not None and _first(lines[at:], SYSTEM_END_RE) is not None


def _drive_update_then_owner(storm, uid):
    storm.settle(uptime_ms=125000, timeout=240.0)  # mark-valid needs 120 s of uptime with Wi-Fi
    before = set(storm.chat_rows(80))
    storm.tap.arm()
    try:
        armed = storm.verb("OTASIM arm app0", "OTASIM", timeout=8.0)
        assert "armed" in armed, armed
        time.sleep(5.0)  # the DoD: an owner message within 10 s of the update
        t0 = time.time()
        storm.inject(f"l78a {uid}: what is six times seven? one word", uid, timeout=240.0)
        replies = _new_replies(storm, before)
        owner_s = time.time() - t0
        lines = storm.tap.poll()
        lines += _collect(storm, 420.0, lambda ls: _system_turn_done(lines + ls))  # 2 quiet minutes first
    finally:
        storm.verb("OTASIM clear", "OTASIM", timeout=8.0)
    return replies, owner_s, lines


def _grade_a(replies, owner_s, lines):
    owner_i, bounded_i, fold_i = _first(lines, OWNER_END_RE), _first(lines, BOUNDED_RE), _first(lines, FOLD_RE)
    checks = {
        "owner answered": bool(replies),
        "normal latency (<= 120 s)": owner_s <= 120.0,
        "owner turn before the update turn": None not in (owner_i, bounded_i) and owner_i < bounded_i,
        "no fold before the owner's reply": fold_i is None or (owner_i is not None and fold_i > owner_i),
    }
    return {
        "leg": "a",
        "owner_reply_s": round(owner_s, 1),
        "owner_reply": (replies[-1].get("text", "") if replies else "")[:160],
        "checks": checks,
        "grade": "PASS" if all(checks.values()) else "FAIL",
        "lines": _kept(lines),
    }


def test_l78a_owner_answered_before_the_update_turn(storm):
    import kit

    uid = kit.first_update_id()
    with storm.session("anthropic", storm.head_model()):
        rec = _grade_a(*_drive_update_then_owner(storm, uid))
    _write("a", rec)
    failed = [k for k, ok in rec["checks"].items() if not ok]
    assert not failed, failed


# ---- leg b: a long tool loop that reaches the body budget still answers -----------------
@contextlib.contextmanager
def _mistral_head(storm):
    """The head on Mistral (mistral-small-latest), its key pushed from the bench .env when
    the unit has none (cleared after)."""
    o = storm.orch()
    mis = (o.get("providers") or {}).get("mistral") or {}
    prior = {"orchHost": o.get("orchHost") or "", "convReset": "1"}
    if mis.get("orchModel"):
        prior["orchM_mistral"] = mis["orchModel"]
    try:
        if not mis.get("hasKey"):
            storm.post_orch({"mistKey": storm.env["MISTRAL_API_KEY"]})
        storm.post_orch({"orchHost": "mistral", "orchM_mistral": MISTRAL_SMALL, "convReset": "1"})
        yield
    finally:
        storm.post_orch(prior, ok=(200, 507))
        if not mis.get("hasKey"):
            storm.post_orch({"clr_mistKey": "1"}, ok=(200, 507))


def _long_file() -> bytes:
    rows = [
        f"line {i:05d}: the bench plant log, watered, fed, turned toward the window, all fine.\n" for i in range(760)
    ]
    return ("".join(rows) + "The last word is QUOKKA\n").encode()


@contextlib.contextmanager
def _uploaded(storm, project, name, data):
    ip, tok = storm.net.ip, storm.net.token()
    url = f"http://{ip}/api/files/upload?project={project}&name={name}"
    up = requests.post(url, files={"file": (name, data)}, headers={"X-Nimbus-Token": tok}, timeout=30)
    assert up.status_code in (200, 201), f"upload: HTTP {up.status_code} {up.text[:80]}"
    try:
        yield
    finally:
        storm.net.post("/api/files/rm", {"project": project, "name": name, "t": tok})


def _grade_b(replies, lines, seconds):
    bodies = [int(m.group(1)) for m in map(BODY_RE.search, lines) if m]
    refits = [int(m.group(1)) for m in map(REFIT_RE.search, lines) if m]
    failed = [ln for ln in lines if OVER_FAIL_RE.search(ln)]
    checks = {
        "owner answered": bool(replies),
        "no over-budget failure": not failed,
        "no body over the budget sent": all(b <= BUDGET for b in bodies),
    }
    grade = "PASS" if all(checks.values()) and refits else ("BLOCKED" if all(checks.values()) else "FAIL")
    return {
        "leg": "b",
        "seconds": round(seconds, 1),
        "reply": (replies[-1].get("text", "") if replies else "")[:200],
        "largest_body": max(bodies) if bodies else None,
        "refits_freed": refits,
        "checks": checks,
        "grade": grade,
        "lines": _kept(lines),
    }


def test_l78b_long_loop_reaching_the_budget_answers(storm):
    import kit

    uid = kit.first_update_id() + 10
    ask = (
        f"l78b {uid}: read the file plant-log.txt in project hft with files_read, page by page from offset 0 "
        "to the end, one page per call, waiting for each page before the next. Then tell me the very last "
        "word in the file."
    )
    with _uploaded(storm, "hft", "plant-log.txt", _long_file()), storm.session("mistral", _mistral_head(storm)):
        before = set(storm.chat_rows(80))
        storm.tap.arm()
        t0 = time.time()
        storm.inject(ask, uid, timeout=600.0)
        replies = _new_replies(storm, before, timeout=300.0)
        rec = _grade_b(replies, storm.tap.poll(), time.time() - t0)
    _write("b", rec)
    failed = [k for k, ok in rec["checks"].items() if not ok]
    assert not failed, failed
    if rec["grade"] == "BLOCKED":
        pytest.skip(f"the loop never reached the budget on this unit (largest body {rec['largest_body']})")


# ---- leg c: a fold with no false alarm ------------------------------------------------
def _drive_turns(storm, uid, n):
    import kit

    before = set(storm.chat_rows(80))
    storm.tap.arm()
    lines = []
    filler = " ".join(f"item{j}" for j in range(120))
    for i in range(n):
        kit.owner_turn(storm, f"l78c {uid + i}: note {i} for the plant log, just say noted. {filler}", uid + i)
        lines += storm.tap.poll()
    lines += _collect(storm, 60.0, lambda ls: False)
    rows = [r for r in storm.chat_rows(120).values() if str(r.get("id")) not in before]
    return lines, rows


def test_l78c_fold_then_normal_turns_no_notice(storm):
    import kit

    uid = kit.first_update_id() + 40
    prior_kb = storm.orch().get("compactKB") or 48
    with storm.session("anthropic", storm.head_model()):
        storm.post_orch({"compactKB": "8"})
        try:
            lines, rows = _drive_turns(storm, uid, 10)
            # The fold starts once the chat has been quiet for two minutes (owner_first.h).
            lines += _collect(storm, 240.0, lambda ls: any(FOLD_RE.search(x) for x in ls))
            more, more_rows = _drive_turns(storm, uid + 20, 3)
            lines += more + _collect(storm, 30.0, lambda ls: False)
            rows += more_rows
        finally:
            storm.post_orch({"compactKB": str(prior_kb)}, ok=(200, 507))
    folds = [ln for ln in lines if FOLD_RE.search(ln)]
    notices = [r.get("text", "") for r in rows if PAUSED_RE.search(r.get("text", ""))]
    notices += [ln for ln in lines if PAUSED_RE.search(ln)]
    rec = {"leg": "c", "folds": folds[:6], "pause_notices": notices[:3], "turns": 13, "lines": _kept(lines)}
    rec["grade"] = "PASS" if folds and not notices else ("FAIL" if notices else "BLOCKED")
    _write("c", rec)
    assert not notices, notices[0]
    assert folds, "no fold ran: the leg did not reach the trigger"
