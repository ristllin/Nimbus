"""L65 (lane QW4) - no paid call escapes the monthly spend ceiling, on a real board.

Live micro-amount legs (the 23 Oct pattern: tiny margins, real cheap calls), on the
TEST build (the SPENDSET console verb seeds the count):

  1. A custom endpoint on a paid host (Mistral's API in the OpenAI convention, on
     mistral-small-latest) is counted at the high estimate, never at 0: with the
     count seeded 5 cents under the ceiling, the next turn on it crosses the ceiling
     and paid calls pause; the turn after that is refused before any request.
  2. A running OpenAI helper when paid calls pause: the device stops it by itself
     (CUM-778, owner ruling: the freeze stops running paid helpers), the cancel goes out
     (no "refused - spend ceiling"), the job is read back and counted (OpenAI reports
     the tokens a cancelled run billed; 0 when it billed none), and OpenAI itself says
     the run is cancelled.
  3. The same for an Anthropic helper (a managed-agent session) still at work: the
     interrupt goes out by itself and the device counts exactly what the session reports
     (0 when it is stopped before it billed anything).

Every setting a leg changes is restored in a finally: the ceiling and this month's
count, the head, the helper order, the custom endpoint.

Markers: ``hil`` + ``net`` + ``agent``. Needs --allow-hardware, NIMBUS_TEST_IP and
NIMBUS_TEST_TOKEN for the board on the serial port, OpenAI and Anthropic keys on the
board, and NIMBUS_ENV_FILE (the Mistral key for leg 1; the OpenAI and Anthropic keys
for the read-backs). Spend: about $0.01 on mistral-small-latest, under $0.01 on the
OpenAI helper, a few cents on the Anthropic helper and the head turns.
"""

from __future__ import annotations

import json
import re
import time
import urllib.request

import pytest

from device import ExpectTimeout
from webrig import ip_tok_or_skip, make_session, require_orchestrator

pytestmark = [pytest.mark.hil, pytest.mark.net, pytest.mark.agent]

LIMIT_CENTS = 2000
CUSTOM_BASE = "https://api.mistral.ai"
CUSTOM_MODEL = "mistral-small-latest"
HELPER_TASK = (
    "Start exactly one helper (a sub-agent) on {prov} that writes a detailed 3000-word essay "
    "on the history of tea. Do not wait for it and do not do the work yourself: reply only 'started'."
)

_S = None
_IP = ""


# ---- small helpers -------------------------------------------------------------------


def _orch() -> dict:
    r = _S.get(f"http://{_IP}/api/orch", timeout=15)
    assert r.status_code == 200, f"/api/orch -> {r.status_code}"
    return r.json()


def _post(fields: dict) -> None:
    r = _S.post(f"http://{_IP}/api/orch", data=fields, timeout=15)
    assert r.status_code == 200, f"POST /api/orch {sorted(fields)} -> {r.status_code}: {r.text[:120]}"


def _spend(device) -> dict:
    device.drain(quiet=0.3)
    device.send("SPENDSET?")
    return json.loads(device.expect_re(r"SPENDSET (\{.*\})", timeout=8.0).group(1))


def _seed(device, line: str) -> None:
    device.cmd(f"SPENDSET {line}", "SPENDSET", timeout=10.0)


def _collect(device, until: str, timeout: float) -> list:
    """Console lines until one matches `until` (inclusive) or the time runs out."""
    out, rx, end = [], re.compile(until), time.time() + timeout
    while time.time() < end:
        try:
            line = device.expect_re(r".", timeout=max(0.2, end - time.time())).string
        except ExpectTimeout:
            break
        out.append(line)
        if rx.search(line):
            break
    return out


def _web_turn(text: str, timeout_s: int = 150):
    """One web chat turn to its own answer: (status, reply)."""
    r = _S.post(f"http://{_IP}/api/chat", data={"text": text}, timeout=10)
    if r.status_code != 200:
        return r.status_code, r.text
    turn = str(r.json().get("turn") or "")
    deadline = time.time() + timeout_s
    while time.time() < deadline:
        j = _S.get(f"http://{_IP}/api/chat", params={"turn": turn}, timeout=10).json()
        if j.get("done"):
            return 200, str(j.get("reply") or "")
        time.sleep(2.0)
    pytest.fail(f"turn {turn} was not answered within {timeout_s}s")


def _provider_get(url: str, headers: dict) -> dict:
    req = urllib.request.Request(url, headers=headers)
    try:
        with urllib.request.urlopen(req, timeout=30) as resp:
            return json.loads(resp.read())
    except urllib.error.HTTPError as e:
        return {"http_error": e.code}


def _wait_http(timeout: float = 60.0) -> None:
    end = time.time() + timeout
    while time.time() < end:
        try:
            if _S.get(f"http://{_IP}/api/state", timeout=5).status_code == 200:
                return
        except Exception:  # noqa: BLE001 - the board is still booting
            pass
        time.sleep(2.0)
    pytest.fail("the board's web page did not come back after the restart")


def _restart(device) -> None:
    """Restart the board and wait for its web page. The bench console sometimes misses the
    READY beacon after a reset (USB-CDC); the web page coming back is the check that
    matters, so a missed beacon is logged, not failed."""
    try:
        device.reset()
        device.wait_ready(timeout=40.0)
    except Exception as exc:  # noqa: BLE001 - the console, not the board
        print(f"[l65q] restart: no READY beacon on the console ({type(exc).__name__}); waiting for the web page")
    _wait_http()


# ---- fixture: the board, with the ceiling and count restored afterwards --------------


@pytest.fixture(scope="module")
def board(device):
    global _S, _IP
    _IP, tok = ip_tok_or_skip("L65 spend escapes")
    _S = make_session(tok)
    require_orchestrator(_S, _IP, "L65 spend escapes")
    try:
        before = _spend(device)
    except ExpectTimeout:
        pytest.skip("test console has no SPENDSET verb (flash the TEST build of this lane)")
    prior = _orch()
    print(
        f"\n[l65q] before: limit={before.get('limitCents')} ownerSet={before.get('ownerSet')} "
        f"spent={before.get('spentCents')} head={prior.get('orchHost')} sub={prior.get('subPrio')}"
    )
    yield device
    restore = {"orchHost": prior.get("orchHost", ""), "convReset": "1"}
    if prior.get("subPrio"):
        restore["subPrio"] = prior["subPrio"]
    try:
        _post(restore)
        if before.get("active") is False:
            device.cmd("SPENDSET limit off", "SPENDSET", timeout=10.0)
        else:
            _seed(device, f"limit {before.get('limitCents', LIMIT_CENTS)}")
        _seed(device, str(before.get("spentCents", 0)))
        after = _spend(device)
        print(
            f"[l65q] restored: limit={after.get('limitCents')} spent={after.get('spentCents')} "
            f"frozen={after.get('frozen')}"
        )
    except Exception as exc:  # noqa: BLE001 - logged for a hand restore
        print(f"[l65q] RESTORE INCOMPLETE: {exc!r} (before: {json.dumps(before)[:300]})")


# ---- leg 1: a custom endpoint on a paid host reaches the ceiling ---------------------


def _cross_on_custom(device) -> None:
    """Seeded 5 cents under the ceiling, one turn on the custom endpoint crosses it."""
    _seed(device, f"limit {LIMIT_CENTS}")
    _seed(device, str(LIMIT_CENTS - 5))
    st = _spend(device)
    assert st["frozen"] is False, f"seeded 5 cents under the ceiling but frozen: {st}"
    device.drain(quiet=0.3)
    status, reply = _web_turn("Say ok.")
    lines = _collect(device, r"turn spend: .*custom/", 30.0)
    spend_line = next((ln for ln in lines if "turn spend:" in ln and "custom/" in ln), "")
    print(f"[l65q] leg 1 turn: http={status} reply={reply[:60]!r}\n[l65q]   {spend_line}")
    assert status == 200 and reply, f"the custom endpoint turn did not answer: {status} {reply[:120]}"
    assert spend_line, "no turn spend line for the custom endpoint (did the turn run on it?)"
    deadline = time.time() + 20.0
    st = _spend(device)
    while time.time() < deadline and not st["frozen"]:
        time.sleep(2.0)
        st = _spend(device)
    print(f"[l65q] leg 1 after the turn: spent={st['spentCents']} of {st['limitCents']} frozen={st['frozen']}")
    assert st["frozen"] is True and st["spentCents"] >= LIMIT_CENTS, (
        f"the custom endpoint's spend did not reach the ceiling (priced at 0?): {st}"
    )


def _next_turn_refused(device) -> None:
    """The turn after the crossing is refused before any request leaves."""
    device.drain(quiet=0.3)
    status, reply = _web_turn("Say ok again.")
    window = _collect(device, r"turn refused|refused - spend ceiling", 20.0)
    print(f"[l65q] leg 1 next turn: http={status} reply={reply[:80]!r}")
    assert status == 409 or "paused" in reply.lower(), f"the next turn was not refused: {status} {reply[:120]}"
    assert not any("turn spend:" in ln and "custom/" in ln for ln in window), (
        "a turn ran on the custom endpoint after the freeze"
    )


def _restore_custom(device, prior: dict, prior_host: str) -> None:
    """The borrowed custom endpoint back as it was (base, wire, model, no key) and the head."""
    _post(
        {
            "custBase": prior.get("base", ""),
            "clr_custKey": "1",
            "custModel": prior.get("model", ""),
            "custConv": prior.get("conv", "openai") or "openai",
            "orchHost": prior_host,
            "convReset": "1",
        }
    )
    _restart(device)
    back = _orch().get("cust", {})
    print(f"[l65q] leg 1 restored: custom endpoint base={back.get('base')!r} key={back.get('hasKey')}")
    assert back.get("base", "") == prior.get("base", "") and not back.get("hasKey"), back


def test_1_custom_endpoint_on_a_paid_host_freezes_at_the_ceiling(board, secrets):
    device = board
    prior = _orch().get("cust", {})
    if prior.get("hasKey"):  # a key cannot be read back, so it could not be restored
        pytest.skip(f"the board's custom endpoint ({prior.get('base')}) has a key: not clobbered")
    key = secrets.get("MISTRAL_API_KEY")
    if not key:
        pytest.skip("no MISTRAL_API_KEY in NIMBUS_ENV_FILE for the custom endpoint leg")
    prior_host = _orch().get("orchHost", "")
    try:
        _post(
            {
                "custBase": CUSTOM_BASE,
                "custConv": "openai",
                "custKey": str(key),
                "custModel": CUSTOM_MODEL,
                "orchHost": "custom",
                "convReset": "1",
            }
        )
        _restart(device)  # the custom head registers at boot
        _cross_on_custom(device)
        _next_turn_refused(device)
    finally:
        _restore_custom(device, prior, prior_host)


# ---- legs 2 and 3: stopping a running helper while paid calls are paused --------------


def _spawn_helper(device, prov: str) -> tuple:
    """(tag, job id) of a helper the head started on `prov`."""
    for attempt in range(2):
        device.drain(quiet=0.3)
        status, reply = _web_turn(HELPER_TASK.format(prov=prov))
        lines = _collect(device, r"orchestrator: spawned \S+ -> " + prov + r":", 90.0)
        m = next(
            (
                re.search(r"spawned (\S+) -> (" + prov + r":\S+)", ln)
                for ln in lines
                if re.search(r"spawned \S+ -> " + prov + ":", ln)
            ),
            None,
        )
        print(
            f"[l65q] spawn on {prov} (try {attempt + 1}): http={status} reply={reply[:60]!r} "
            f"job={m.group(2) if m else None}"
        )
        if m:
            return m.group(1), m.group(2)
    pytest.fail(f"the head did not start a helper on {prov} in two turns")


def _cancel_under_freeze(device, prov: str, running) -> tuple:
    """Spawn on `prov`, wait until `running(job)` says the run is under way, freeze: the
    device stops the helper by itself (CUM-778; checked every 5 s), nothing else asks.
    (job id, console lines, spent before, spent after)."""
    _post({"subPrio": prov})
    _seed(device, f"limit {LIMIT_CENTS}")
    _seed(device, "0")
    tag, job = _spawn_helper(device, prov)
    deadline = time.time() + 90.0
    while time.time() < deadline and not running(job):
        time.sleep(3.0)
    device.drain(quiet=0.3)
    before = LIMIT_CENTS + 100
    _seed(device, str(before))
    # Round 3: either the stop is confirmed with the run read back ("cancelled job ...
    # counted"), or, for a session still running at that read, the stop list counts it
    # when its poll finds it stopped ("... counted at its end").
    lines = _collect(device, r"cancelled job .* counted|counted at its end", 120.0)
    for ln in lines:
        if any(k in ln for k in ("cancel", "stopping", "refused", "counted")):
            print(f"[l65q]   {ln}")
    st = _spend(device)
    assert st["frozen"] is True, f"the count over the ceiling did not freeze: {st}"
    stopping = [ln for ln in lines if f"{tag} stopping - paid calls to {prov} are paused" in ln]
    assert stopping, f"the freeze did not stop {tag} by itself"
    return job, lines, before, st["spentCents"]


def _grade_cancel(job: str, lines: list, path_part: str) -> re.Match:
    refused = [ln for ln in lines if "refused - spend ceiling" in ln and path_part in ln]
    assert not refused, f"the cancel was refused while paid calls were paused: {refused}"
    rid = job.split(":", 1)[1]
    counted = next(
        (
            re.search(r"(?:cancelled job \S+ counted|counted at its end) \((\w+) in=(\d+) out=(\d+)", ln)
            for ln in lines
            if ("cancelled job" in ln or "counted at its end" in ln) and rid in ln
        ),
        None,
    )
    assert counted, f"no 'counted' line for {job}"
    return counted


def _keyed(prov: str) -> bool:
    provs = _orch().get("providers", {})
    row = provs.get(prov) if isinstance(provs, dict) else next((p for p in provs if p.get("id") == prov), None)
    return bool(row and row.get("hasKey"))


def test_2_an_openai_helper_stops_while_paused_and_is_counted(board, secrets):
    device = board
    key = secrets.get("OPENAI_API_KEY")
    if not _keyed("openai"):
        pytest.skip("no OpenAI key on the board")
    auth = {"Authorization": f"Bearer {key}"} if key else None

    def running(job: str) -> bool:
        if not auth:
            time.sleep(10.0)
            return True
        rid = job.split(":", 1)[1]
        return _provider_get(f"https://api.openai.com/v1/responses/{rid}", auth).get("status") == "in_progress"

    job, lines, before, after = _cancel_under_freeze(device, "openai", running)
    counted = _grade_cancel(job, lines, "/cancel")
    rid = job.split(":", 1)[1]
    if auth:
        j = _provider_get(f"https://api.openai.com/v1/responses/{rid}", auth)
        usage = j.get("usage") or {}
        print(f"[l65q] OpenAI says: status={j.get('status')} usage={json.dumps(usage)[:200]}")
        assert j.get("status") in ("cancelled", "completed", "incomplete", "failed"), f"the run is still going: {j}"
        # The device counted what OpenAI reports for the run (or the estimate when it
        # listed work with no tokens).
        if "estimated" not in counted.group(0):
            assert int(counted.group(2)) == int(usage.get("input_tokens") or 0), (counted.group(0), usage)
    print(f"[l65q] leg 2: counted {counted.group(0)}; spent {before} -> {after}")


def test_3_an_anthropic_helper_stops_while_paused_and_is_counted(board, secrets):
    device = board
    key = secrets.get("ANTHROPIC_API_KEY")
    if not _keyed("anthropic"):
        pytest.skip("no Anthropic key on the board")
    if not key:
        pytest.skip("no ANTHROPIC_API_KEY in NIMBUS_ENV_FILE to see the session at work")
    hdr = {"x-api-key": str(key), "anthropic-version": "2023-06-01", "anthropic-beta": "managed-agents-2026-04-01"}

    def session(job: str) -> dict:
        return _provider_get(f"https://api.anthropic.com/v1/sessions/{job.split(':', 1)[1]}", hdr)

    def running(job: str) -> bool:  # still at work (an idle session has finished: nothing to stop)
        return session(job).get("status") == "running"

    job, lines, before, after = _cancel_under_freeze(device, "anthropic", running)
    counted = _grade_cancel(job, lines, "/events")
    j = session(job)
    usage = j.get("usage") or {}
    print(f"[l65q] Anthropic says: status={j.get('status')} usage={json.dumps(usage)[:240]}")
    print(f"[l65q] leg 3: counted {counted.group(0)}; spent {before} -> {after}")
    # The device counted what the session itself reports (stopped seconds in, a session
    # can report 0 tokens and a 0 list cost, as recorded live on 27 Oct 2026), or the
    # estimate when its read failed.
    if "estimated" not in counted.group(0):
        assert int(counted.group(2)) == int(usage.get("input_tokens") or 0), (counted.group(0), usage)
        assert int(counted.group(3)) == int(usage.get("output_tokens") or 0), (counted.group(0), usage)
