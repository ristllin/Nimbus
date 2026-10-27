"""L50 (lane HFM, v4.6.1) - the device knows which model it runs, keeps its lists fresh,
and a pause never moves the owner's head.

Production, 26 Oct 2026 (Lumi on v4.6.0): no model list was ever fetched after the
update (the refresh queue waited for a resting largest block of 8,000 B; Lumi rests at
7,156 B), the device never recorded which concrete model answered, and the assistant
moved the head from a paused Anthropic to Mistral for good. Host-tested first
(test_models_refresh, test_orch_turn_spend, test_model_catalog, test_harness_turn,
test_harness_apply, test_orch_head_switch); these legs prove it on a bench unit (the
test build, Orchestrator mode, Anthropic and Mistral keyed). LAN legs need
``NIMBUS_TEST_IP`` and ``NIMBUS_TEST_TOKEN``; the console legs the board on its port.

1. ``test_hfm_lists_fetched_within_minutes_of_boot``: run on a unit flashed from a
   v4.5.16-state image and then updated to this build, within 20 minutes of the boot
   after the update. Every keyed, unrejected provider's list is fetched (``listSrc``
   api, ``fetchedAt`` set) by boot + 15 minutes, the Mistral list holds rows, and its
   ``-latest`` rows name the dated model behind them. Optional ``NIMBUS_HFM_BALLAST``
   (bytes) holds internal SRAM first, so the resting largest block sits under the old
   8,000 B gate as on Lumi (it is released afterwards). No paid call: the list GETs are
   free.
2. ``test_hfm_lastturn_names_the_concrete_model``: one web-chat turn on
   mistral/mistral-small-latest (about $0.0005): ``/api/lastturn`` says
   ``model: mistral-small-<date> (asked for mistral-small-latest)`` and the turn spend
   line carries ``id=mistral-small-<date>``. The head is put back afterwards.
4. ``test_hfm_refresh_runs_on_the_poll_task_on_a_fragmented_heap``: with ``NIMBUS_HFM_BALLAST``
   holding internal SRAM so the largest block sits under the refresh worker's 8 KB stack
   (as on Lumi), a key verify (which defers for low memory) still refreshes that list, on
   the Telegram poll task (the durable "model list refresh on the Telegram poll task" and
   "models stored" lines). No paid call. Run it right after a boot (its own pytest session:
   unit L resets when the console opens): a verified key's deferral asks within the hourly
   window, and the boot + 10 min pass has already refreshed every list in a longer session.
3. ``test_hfm_a_frozen_head_is_not_moved``: Anthropic as the head, frozen by a 1-cent
   monthly limit (its spend seeded over it); the owner writes "switch to mistral since
   anthropic is out of budget"; the turn fails over to Mistral (about $0.001) and
   ``orchHost`` is still anthropic afterwards. The limit, the seeded spend and the head
   are put back.

The vision leg (a photo read natively by a Mistral head) is ``test_l49_native_image``'s
``test_l49_label_numbers_read_from_pixels`` with the head pinned to
mistral/mistral-small-latest (lanes/HFM/PR_BODY.md, bench steps).
"""

from __future__ import annotations

import json
import os
import re
import time

import pytest

from webrig import ip_tok_or_skip, make_session, require_orchestrator

_S = None
SLUGS = ("cumulo", "openai", "anthropic", "mistral", "zai")
DATED = re.compile(r"-\d{4}$")


def _orch(ip: str) -> dict:
    return _S.get(f"http://{ip}/api/orch", timeout=10).json()


def _models(ip: str, prov: str) -> dict:
    try:
        j = _S.get(f"http://{ip}/api/models", params={"provider": prov}, timeout=15).json()
    except ValueError:  # a page refused for low memory (the ballast leg): no answer this time
        return {}
    return (j.get("providers") or {}).get(prov) or {}


def _keyed(ip: str) -> list:
    provs = _orch(ip).get("providers") or {}
    return [p for p in SLUGS if (provs.get(p) or {}).get("hasKey") and (provs.get(p) or {}).get("verify") != 0]


def _uptime_s(ip: str) -> int:
    st = _S.get(f"http://{ip}/api/state", timeout=10).json()
    return int(st.get("uptimeMs") or 0) // 1000


def _turn(ip: str, text: str, timeout_s: int = 240) -> str:
    """One web-chat turn to its own answer; returns the reply."""
    r = _S.post(f"http://{ip}/api/chat", data={"text": text}, timeout=10)
    assert r.status_code == 200, f"chat send {r.status_code}: {r.text[:200]}"
    turn = str(r.json().get("turn") or "")
    assert turn, f"/api/chat gave no turn id: {r.text[:200]}"
    deadline = time.time() + timeout_s
    while time.time() < deadline:
        j = _S.get(f"http://{ip}/api/chat", params={"turn": turn}, timeout=10).json()
        if j.get("done"):
            return str(j.get("reply") or "")
        time.sleep(2.0)
    pytest.fail(f"turn {turn} was not answered within {timeout_s}s: {text[:60]}")


def _log(ip: str) -> str:
    """The durable log's recent lines (the RAM ring of /api/log is too small to keep a turn)."""
    r = _S.get(f"http://{ip}/api/errlog/recent", params={"bytes": 16384}, timeout=15)
    return r.text if r.status_code == 200 else ""


def _mcp(ip: str, tool: str, args: dict) -> str:
    body = json.dumps({"jsonrpc": "2.0", "id": 1, "method": "tools/call", "params": {"name": tool, "arguments": args}})
    r = _S.post(f"http://{ip}/mcp", data=body, headers={"Content-Type": "application/json"}, timeout=20)
    assert r.status_code == 200, f"/mcp {tool} -> {r.status_code}: {r.text[:160]}"
    return " ".join(c.get("text", "") for c in r.json().get("result", {}).get("content", []))


def _set_head(ip: str, host: str, prov_model: dict) -> None:
    data = {f"orchM_{p}": m for p, m in prov_model.items()}
    _S.post(f"http://{ip}/api/orch", data=data, timeout=10)
    _S.post(f"http://{ip}/api/orch", data={"orchHost": host}, timeout=10)


def _head(ip: str) -> tuple:
    o = _orch(ip)
    provs = o.get("providers") or {}
    return o.get("orchHost") or "", {p: (provs.get(p) or {}).get("orchModel") or "" for p in SLUGS}


@pytest.fixture(scope="module")
def board(device, net):
    """The unit's address once its web server answers (the session reset first)."""
    global _S
    ip, tok = ip_tok_or_skip("L50 lane HFM")
    _S = make_session(tok)
    deadline = time.time() + 90
    while time.time() < deadline:
        try:
            if _S.get(f"http://{ip}/api/state", timeout=5).status_code in (200, 401):
                break
        except Exception:  # noqa: BLE001 - not up yet
            pass
        time.sleep(3)
    require_orchestrator(_S, ip, "L50 lane HFM")
    return ip


def _lists_fetched(seen: dict) -> bool:
    return all(m.get("listSrc") == "api" and int(m.get("fetchedAt") or 0) > 0 for m in seen.values())


def _wait_lists(ip: str, keyed: list, until_s: float) -> dict:
    """Poll /api/models until every keyed list is fetched or the deadline passes."""
    seen = {}
    deadline = time.time() + until_s
    while time.time() < deadline:
        seen = {p: _models(ip, p) for p in keyed}
        if _lists_fetched(seen):
            break
        time.sleep(15)
    return seen


def _dated_latest_rows(mistral: dict) -> dict:
    rows = mistral.get("models") or []
    latest = {r.get("id"): r.get("name", "") for r in rows if str(r.get("id", "")).endswith("-latest")}
    return {i: n for i, n in latest.items() if DATED.search(n or "")}


def _require_update_state(ip: str, keyed: list, up: int) -> None:
    """Skip unless some list is still the update's legacy merge (or missing)."""
    start = {p: _models(ip, p).get("listSrc") for p in keyed}
    print(f"[L50hfm] uptime {up} s, lists at the start: {json.dumps(start)}")
    if not [p for p, src in start.items() if src in ("legacy", "none")]:
        pytest.skip(f"no legacy or missing list: not the state right after an update from v4.5.16 ({start})")


def _poll_routes(ip: str) -> list:
    """The durable lines of refreshes that ran on the Telegram poll task."""
    try:
        text = _log(ip)
    except Exception:  # noqa: BLE001 - a page refused for low memory
        text = ""
    routes = [ln for ln in text.splitlines() if "model list refresh on the Telegram poll task" in ln]
    print("[L50hfm] poll-task refreshes:\n" + "\n".join(routes[-6:]))
    return routes


@pytest.mark.hil
@pytest.mark.net
def test_hfm_lists_fetched_within_minutes_of_boot(board, device):
    up = _uptime_s(board)
    if up > 20 * 60:
        pytest.skip(f"run within 20 minutes of the boot after the update (uptime {up} s)")
    ballast = int(os.environ.get("NIMBUS_HFM_BALLAST", "0") or 0)
    if ballast:
        print("\n" + device.cmd(f"FAULT sram_ballast {ballast}", "FAULT", timeout=10.0).strip())
    keyed = _keyed(board)
    assert "mistral" in keyed, f"the leg needs a keyed Mistral (keyed: {keyed})"
    _require_update_state(board, keyed, up)
    try:
        seen = _wait_lists(board, keyed, max(60, 15 * 60 - up))
    finally:
        if ballast:
            device.cmd("FAULT sram_ballast 0", "FAULT", timeout=10.0)
    summary = {p: (m.get("listSrc"), m.get("fetchedAt"), m.get("total")) for p, m in seen.items()}
    print(f"[L50hfm] uptime {_uptime_s(board)} s, lists {json.dumps(summary)}")
    assert _lists_fetched(seen), f"lists not fetched by boot + 15 min: {summary}"
    routes = _poll_routes(board)
    if ballast:
        assert routes, "under the ballast no refresh ran on the Telegram poll task"
    mistral = seen["mistral"]
    assert int(mistral.get("total") or 0) > 0 and mistral.get("models"), "the Mistral list is empty"
    dated = _dated_latest_rows(mistral)
    print(f"[L50hfm] mistral -latest rows naming their model: {json.dumps(dated)}")
    assert dated, "no Mistral -latest row names its dated model"


def _unfreeze_ceiling(ip: str) -> None:
    """The bench setting: a frozen overall ceiling (a unit's real month spend over the 20 USD
    default) would pause every provider; raise it to 100 USD and leave it there."""
    ceiling = (_orch(ip).get("usage") or {}).get("ceiling") or {}
    if ceiling.get("frozen"):
        print(f"[L50hfm] overall ceiling frozen ({ceiling}); set to 100 USD (the bench setting)")
        _S.post(f"http://{ip}/api/orch", data={"spendCeil": "10000"}, timeout=10)


@pytest.mark.net
@pytest.mark.agent
def test_hfm_lastturn_names_the_concrete_model(board):
    if "mistral" not in _keyed(board):
        pytest.skip("no keyed Mistral on this unit")
    _unfreeze_ceiling(board)
    host0, models0 = _head(board)
    try:
        _set_head(board, "mistral", {"mistral": "mistral-small-latest"})
        reply = _turn(board, "Reply with the single word ok.")
        page = _S.get(f"http://{board}/api/lastturn", timeout=15).text
        m = re.search(r"^model: (\S+)(?: \(asked for (\S+)\))?$", page, re.M)
        print(f"\n[L50hfm] reply {reply[:60]!r}; lastturn {m.group(0) if m else page[:300]!r}")
        assert m, "no model line in /api/lastturn"
        assert m.group(2) == "mistral-small-latest" and DATED.search(m.group(1)), m.group(0)
        spend = [ln for ln in _log(board).splitlines() if "turn spend:" in ln and "mistral/" in ln]
        print(f"[L50hfm] {spend[-1] if spend else '(no turn spend line in /api/log)'}")
        assert spend and f"id={m.group(1)}" in spend[-1], "the turn spend line names no concrete model"
    finally:
        _set_head(board, host0, {"mistral": models0.get("mistral", "")})


def _spend_state(device) -> dict:
    out = device.cmd("SPENDSET?", "SPENDSET {", timeout=8.0)
    j = json.loads(out[out.index("{") : out.rindex("}") + 1])
    return {r["p"]: r for r in j.get("providers", [])}


def _budget_row(ip: str, prov: str) -> dict:
    usage = _orch(ip).get("usage") or {}
    for row in usage.get("byProvider") or []:
        if row.get("prov") == prov:
            return row
    return {}


@pytest.mark.hil
@pytest.mark.net
@pytest.mark.agent
def test_hfm_a_frozen_head_is_not_moved(board, device):
    keyed = _keyed(board)
    if "anthropic" not in keyed or "mistral" not in keyed:
        pytest.skip(f"needs Anthropic and Mistral keyed (keyed: {keyed})")
    _unfreeze_ceiling(board)
    host0, models0 = _head(board)
    prio0 = _orch(board).get("provPrio") or ""
    budget0 = _budget_row(board, "anthropic")
    caps0 = _spend_state(device).get("anthropic", {})
    spent0 = int(caps0.get("spentCents", 0))
    restore = "anthropic:%d:%d:%d:%d" % (
        int(budget0.get("tokenLimit", 0)),
        int(budget0.get("callLimit", 0)),
        int(budget0.get("resetDay", 1)) or 1,
        int(budget0.get("centsLimit", 0)),
    )
    print(f"\n[L50hfm] before: head {host0}, anthropic cap {caps0}, budget {restore}")
    try:
        _set_head(
            board,
            "anthropic",
            {
                "anthropic": models0.get("anthropic") or "claude-haiku-5-5",
                "mistral": models0.get("mistral") or "mistral-small-latest",
            },
        )
        # Lumi's order (a head that fails over to Mistral); the unit's own order comes back after.
        _S.post(f"http://{board}/api/orch", data={"provPrio": "anthropic,mistral"}, timeout=10)
        _S.post(f"http://{board}/api/orch", data={"budget": "anthropic:0:0:1:1"}, timeout=10)
        device.cmd(f"SPENDSET prov anthropic {max(spent0, 100)}", "SPENDSET", timeout=8.0)
        frozen = _spend_state(device).get("anthropic", {})
        assert frozen.get("frozen"), f"Anthropic is not frozen by the micro cap: {frozen}"
        reply = _turn(board, "switch to mistral since anthropic is out of budget")
        after = _orch(board).get("orchHost")
        tail = "\n".join(
            ln
            for ln in _log(board).splitlines()[-80:]
            if "orch_model" in ln or "turn host=" in ln or "over token budget" in ln
        )
        print(f"[L50hfm] reply: {reply[:300]}\n[L50hfm] log:\n{tail}\n[L50hfm] orchHost after: {after}")
        assert after == "anthropic", f"the head moved to {after}"
        assert "turn host=mistral" in tail, "the turn did not fail over to Mistral (the leg proves nothing then)"
        # The guard itself (orch_model(no change) while the chosen head is paused) is pinned host-side in
        # test_harness_apply: device.control waits on the network, so the LAN endpoint refuses it (web_guard.h)
        # and cannot move the head either.
        action = {"type": "orch_model", "provider": "mistral", "model": "mistral-small-latest"}
        body = json.dumps(
            {
                "jsonrpc": "2.0",
                "id": 1,
                "method": "tools/call",
                "params": {"name": "device.control", "arguments": {"action": action}},
            }
        )
        r = _S.post(f"http://{board}/mcp", data=body, headers={"Content-Type": "application/json"}, timeout=20)
        print(f"[L50hfm] LAN device.control orch_model -> {r.status_code} {r.text[:300]}")
        assert r.status_code == 200 and (r.json().get("error") or {}).get("code") == -32003, r.text[:300]
        assert _orch(board).get("orchHost") == "anthropic"
    finally:
        if prio0:
            _S.post(f"http://{board}/api/orch", data={"provPrio": prio0}, timeout=10)
        _S.post(f"http://{board}/api/orch", data={"budget": restore}, timeout=10)
        device.cmd(f"SPENDSET prov anthropic {spent0}", "SPENDSET", timeout=8.0)
        _set_head(board, host0, {"anthropic": models0.get("anthropic", ""), "mistral": models0.get("mistral", "")})
        print(
            f"[L50hfm] restored: head {_orch(board).get('orchHost')}, anthropic cap {_spend_state(device).get('anthropic')}"
        )


@pytest.mark.hil
@pytest.mark.net
def test_hfm_refresh_runs_on_the_poll_task_on_a_fragmented_heap(board, device):
    ballast = int(os.environ.get("NIMBUS_HFM_BALLAST", "0") or 0)
    if not ballast:
        pytest.skip("set NIMBUS_HFM_BALLAST (bytes) so the largest internal block sits under 8.7 KB")
    up = _uptime_s(board)
    if up > 540:
        pytest.skip(f"run right after a boot, before the boot + 10 min pass (uptime {up} s)")
    prov = "mistral" if "mistral" in _keyed(board) else _keyed(board)[0]
    before = int(_models(board, prov).get("fetchedAt") or 0)
    print("\n" + device.cmd(f"FAULT sram_ballast {ballast}", "FAULT", timeout=10.0).strip())
    try:
        time.sleep(25)  # the resting sample settles under the ballast
        # A verify the memory gate defers answers 409; it queues the list refresh all the same.
        r = _S.post(f"http://{board}/api/verify", data={"provider": prov}, timeout=10)
        print(f"[L50hfm] verify {prov} -> HTTP {r.status_code}")
        deadline = time.time() + 120
        while time.time() < deadline and not _poll_routes(board):
            time.sleep(5)
    finally:
        device.cmd("FAULT sram_ballast 0", "FAULT", timeout=10.0)
    time.sleep(5)
    after = int(_models(board, prov).get("fetchedAt") or 0)
    lines = [ln for ln in _log(board).splitlines() if prov in ln and ("verify:" in ln or "models stored" in ln)]
    print("[L50hfm] " + "\n[L50hfm] ".join(lines[-8:]))
    assert after > before, f"the {prov} list was not refreshed ({before} -> {after})"
    assert _poll_routes(board), "the refresh did not run on the Telegram poll task"
