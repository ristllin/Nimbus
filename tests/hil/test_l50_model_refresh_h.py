"""L50 (lane H) - models on demand: the refresh queue, the passive refresh, a model switch.

Bench legs for program-2026-10-18 lane H on unit L (the test build, Orchestrator mode,
an Anthropic head on claude-haiku-5-5, at least one more keyed provider). LAN legs need
``NIMBUS_TEST_IP`` and ``NIMBUS_TEST_TOKEN``; the console legs (``hil``) need the board
on its port (``--port`` or ``NIMBUS_PORT``). Lane F's ``test_l50_model_refresh.py``
covers the model store itself in phases ordered around flashes; these legs read that
store and run on their own, so they stay in this file.

1. ``POST /api/models/refresh`` twice inside the rate window starts one refresh per
   provider: the first answers queued (or recent), the second pending or recent, the
   console prints one ``models: <p> list refresh started`` line, ``GET /api/models``
   then carries a newer ``fetchedAt``, and a third ask answers recent (no spend: the
   refresh is the free ``GET /v1/models``).
2. A refresh asked during a turn waits for the turn: the ``list refresh started`` line
   comes after the turn's ``turn spend:`` line, never between ``turn host=`` and it
   (one web-chat turn on claude-haiku-5-5, about $0.002).
3. One web-chat turn "Switch to Sonnet 5.5." on claude-haiku-5-5 applies: ``GET
   /api/orch`` then names anthropic / claude-sonnet-5-5 (about $0.003); the head model
   is put back afterwards.
4. After an update the passive refresh runs once per provider: ``MODELS passive`` queues
   every keyed provider whose model store was written by another firmware version (or
   has none), each store then carries the running version in its header, and a second
   ``MODELS passive`` queues nothing. Run it first after the flash, before the device's
   own check at boot + 10 min. An app flash of the same version queues only providers
   with no store or a stale one; the version-change path is host-tested
   (test_models_refresh).

The page leg ("Updated <time>" after opening a dropdown) is the web harness spec
``tools/webui_harness/tests/74_model_picker.spec.mjs`` run with ``TARGET=device``.
"""

from __future__ import annotations

import re
import time

import pytest

from device import ExpectTimeout
from webrig import ip_tok_or_skip, make_session, require_orchestrator

_S = None
SLUGS = ("cumulo", "openai", "anthropic", "mistral", "zai")
REFRESH_DONE_S = 120  # a refresh is one GET /v1/models, queued behind any verify


def _serial_lines(device, window_s: float) -> list:
    """Every console line printed in the next ``window_s`` seconds."""
    lines = []
    end = time.time() + window_s
    while time.time() < end:
        try:
            lines.append(device.expect_re(r".", timeout=max(0.2, end - time.time())).string)
        except ExpectTimeout:
            break
    return lines


def _models(ip: str, prov: str) -> dict:
    j = _S.get(f"http://{ip}/api/models", params={"provider": prov}, timeout=10).json()
    return (j.get("providers") or {}).get(prov) or {}


def _refresh(ip: str, prov: str) -> dict:
    r = _S.post(f"http://{ip}/api/models/refresh", data={"provider": prov}, timeout=10)
    assert r.status_code == 200, f"refresh {prov}: HTTP {r.status_code} {r.text[:200]}"
    return r.json()


def _keyed(ip: str) -> list:
    provs = _S.get(f"http://{ip}/api/orch", timeout=10).json().get("providers") or {}
    return [p for p in SLUGS if (provs.get(p) or {}).get("hasKey") and (provs.get(p) or {}).get("verify") != 0]


def _wait_done(ip: str, prov: str, before: int, device=None) -> tuple:
    """Poll until the refresh of ``prov`` is no longer pending; (fetchedAt, console lines)."""
    lines = []
    deadline = time.time() + REFRESH_DONE_S
    while time.time() < deadline:
        lines += _serial_lines(device, 2.0) if device is not None else []
        m = _models(ip, prov)
        if not m.get("refreshPending") and int(m.get("fetchedAt") or 0) > before:
            return int(m.get("fetchedAt") or 0), lines
        if device is None:
            time.sleep(2.0)
    pytest.fail(f"the {prov} list refresh did not finish within {REFRESH_DONE_S}s (fetchedAt still {before})")


def _turn(device, ip: str, text: str, timeout_s: int = 150) -> tuple:
    """Run one web-chat turn to its own answer; return (reply, the console lines it printed)."""
    device.drain(quiet=0.3)
    r = _S.post(f"http://{ip}/api/chat", data={"text": text}, timeout=10)
    assert r.status_code == 200, f"chat send {r.status_code}: {r.text[:200]}"
    turn = str(r.json().get("turn") or "")
    assert turn, f"/api/chat gave no turn id: {r.text[:200]}"
    lines = []
    deadline = time.time() + timeout_s
    while time.time() < deadline:
        lines += _serial_lines(device, 2.0)
        j = _S.get(f"http://{ip}/api/chat", params={"turn": turn}, timeout=10).json()
        if j.get("done"):
            lines += _serial_lines(device, 3.0)
            return str(j.get("reply") or ""), lines
    pytest.fail(f"turn {turn} was not answered within {timeout_s}s: {text[:60]}")


def _models_console(device) -> dict:
    """MODELS? -> {slug: (fetchedAt, fw, pending)}, plus the running version under "_build"."""
    device.drain(quiet=0.3)
    device.send("MODELS?")
    out = {}
    deadline = time.time() + 10
    while time.time() < deadline:
        m = device.expect_re(r"^MODELS(\?| )", timeout=max(0.5, deadline - time.time()))
        line = m.string.strip()
        b = re.match(r"^MODELS\? fw=(\S+)", line)
        if b:
            out["_build"] = b.group(1)
            return out
        s = re.match(r"^MODELS (\w+) fetchedAt=(\d+) fw=(\S*) pending=(\d)", line)
        if s:
            out[s.group(1)] = (int(s.group(2)), s.group(3), s.group(4) == "1")
    pytest.fail("MODELS? did not finish (flash the [env:test] build of lane H)")


@pytest.fixture(scope="module")
def board(device, net):
    """The unit's address once its web server answers. Depends on ``device`` and ``net``
    so the session reset and the station rejoin come first (right after a flash the
    first request was refused)."""
    global _S
    ip, tok = ip_tok_or_skip("L50 lane H")
    _S = make_session(tok)
    deadline = time.time() + 90
    while time.time() < deadline:
        try:
            if _S.get(f"http://{ip}/api/state", timeout=5).status_code in (200, 401):
                break
        except Exception:  # noqa: BLE001 - not up yet
            pass
        time.sleep(3)
    require_orchestrator(_S, ip, "L50 lane H")
    return ip


@pytest.mark.hil
def test_l50h_passive_refresh_runs_once_per_provider_after_a_flash(board, device):
    keyed = _keyed(board)
    assert keyed, "no keyed, unrejected provider on this unit"
    first = int(device.cmd_re("MODELS passive", r"^MODELS passive queued=(\d+)", timeout=10).group(1))
    deadline = time.time() + REFRESH_DONE_S * len(keyed)
    state = {}
    while time.time() < deadline:
        state = _models_console(device)
        build = state.get("_build", "")
        done = [p for p in keyed if state.get(p, (0, "", True))[1] == build and not state[p][2]]
        if len(done) == len(keyed):
            break
        time.sleep(5)
    build = state.get("_build", "")
    stale = [p for p in keyed if state.get(p, (0, "", True))[1] != build]
    assert not stale, f"stores not written by version {build}: {stale} (MODELS? {state})"
    again = int(device.cmd_re("MODELS passive", r"^MODELS passive queued=(\d+)", timeout=10).group(1))
    assert again == 0, f"a second passive check queued {again} more refresh(es)"
    print(f"\n[L50h] passive check queued {first} of {len(keyed)} keyed providers; all now stamped {build}")


@pytest.mark.hil
@pytest.mark.net
def test_l50h_two_refresh_asks_in_the_window_start_one_refresh(board, device):
    prov = "anthropic" if "anthropic" in _keyed(board) else _keyed(board)[0]
    before = int(_models(board, prov).get("fetchedAt") or 0)
    device.drain(quiet=0.3)
    a = _refresh(board, prov)
    b = _refresh(board, prov)
    assert a.get("refresh") in ("queued", "recent", "pending"), a
    assert b.get("refresh") in ("pending", "recent"), f"second ask inside the window: {b}"
    if a.get("refresh") == "recent":
        pytest.skip(f"{prov} was refreshed within the hour already (rerun after the window): {a}")
    fetched, lines = _wait_done(board, prov, before, device)
    lines += _serial_lines(device, 3.0)
    starts = [l for l in lines if re.search(rf"models: {prov} list refresh started", l)]
    assert len(starts) == 1, f"expected one refresh start for {prov}, saw {len(starts)}: {starts}"
    assert fetched > before, f"fetchedAt did not move ({before} -> {fetched})"
    c = _refresh(board, prov)
    assert c.get("refresh") == "recent", f"an ask after the refresh, inside the hour: {c}"
    assert int(c.get("fetchedAt") or 0) == fetched
    print(
        f"\n[L50h] {prov}: asks {a.get('refresh')}/{b.get('refresh')}/{c.get('refresh')}, "
        f"one start, fetchedAt {before} -> {fetched}"
    )


def _start_turn(device, ip: str, text: str) -> tuple:
    """Post a web-chat turn and wait until it runs on the device; (turn id, lines so far)."""
    device.drain(quiet=0.3)
    r = _S.post(f"http://{ip}/api/chat", data={"text": text}, timeout=10)
    assert r.status_code == 200, f"chat send {r.status_code}: {r.text[:200]}"
    turn = str(r.json().get("turn") or "")
    lines = []
    deadline = time.time() + 20
    while time.time() < deadline and not any("turn host=" in l for l in lines):
        lines += _serial_lines(device, 0.5)
    assert any("turn host=" in l for l in lines), "the web-chat turn did not start within 20 s"
    return turn, lines


def _queue_one(ip: str, cands: list) -> tuple:
    """Ask a refresh of the first candidate not refreshed within the hour; (provider, answer)."""
    for p in cands:
        ask = _refresh(ip, p)
        if ask.get("refresh") == "queued":
            return p, ask
    pytest.skip(f"every keyed provider was refreshed within the hour ({cands})")


def _finish_turn(device, ip: str, turn: str, lines: list) -> None:
    deadline = time.time() + 150
    while time.time() < deadline:
        lines += _serial_lines(device, 2.0)
        if _S.get(f"http://{ip}/api/chat", params={"turn": turn}, timeout=10).json().get("done"):
            return
    pytest.fail("the turn did not finish")


@pytest.mark.hil
@pytest.mark.net
@pytest.mark.agent
def test_l50h_a_refresh_asked_during_a_turn_runs_after_it(board, device):
    keyed = _keyed(board)
    cands = [p for p in keyed if p != "anthropic"] or keyed
    turn, lines = _start_turn(device, board, "Count from 1 to 30 in words, one per line, then say done.")
    prov, ask = _queue_one(board, cands)
    _finish_turn(device, board, turn, lines)
    _, more = _wait_done(board, prov, int(ask.get("fetchedAt") or 0), device)
    text = "\n".join(lines + more)
    i_start = text.find(f"models: {prov} list refresh started")
    assert i_start >= 0, f"no refresh start line for {prov}"
    i_end = text.rfind("turn spend:", 0, i_start)
    i_turn = text.find("turn host=")
    assert i_turn >= 0 and i_end > i_turn, "the turn's start and end lines were not both seen before the refresh"
    print(f"\n[L50h] {prov} refresh asked mid-turn started after the turn ended")


@pytest.mark.net
@pytest.mark.agent
def test_l50h_switch_to_sonnet_5_5_applies(board, device):
    orch = _S.get(f"http://{board}/api/orch", timeout=10).json()
    ant = (orch.get("providers") or {}).get("anthropic") or {}
    if not ant.get("hasKey"):
        pytest.skip("no Anthropic key on this unit")
    host0, model0 = orch.get("orchHost") or "", ant.get("orchModel") or ""
    try:
        reply, _ = _turn(device, board, "Switch to Sonnet 5.5.")
        after = _S.get(f"http://{board}/api/orch", timeout=10).json()
        got = ((after.get("providers") or {}).get("anthropic") or {}).get("orchModel")
        assert after.get("orchHost") == "anthropic" and got == "claude-sonnet-5-5", (
            f"switch did not apply: host={after.get('orchHost')} model={got}; reply: {reply[:200]}"
        )
        print(f"\n[L50h] switched: anthropic / {got}; reply: {reply[:160]}")
    finally:
        _S.post(f"http://{board}/api/orch", data={"orchM_anthropic": model0}, timeout=10)
        _S.post(f"http://{board}/api/orch", data={"orchHost": host0}, timeout=10)
