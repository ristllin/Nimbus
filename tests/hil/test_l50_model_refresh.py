"""L50 - the model store on a real board (lane F; lane H extends this file).

The model list moved out of NVS into one flash file per provider,
/data/models_<slug>.txt. These legs prove the device behaviour the host suites
cannot: the move across a real OTA (the merge on the trial boot, the NVS erase on
the boot after mark-valid, the rollback still reading the old JSON catalog), a
verify that writes the store with no NVS write, the Cumulo router's whole list,
and the head-host fallback when the pinned provider's key was cleared.

Legs run in the order of the bench steps in lanes/F/PR_BODY.md; each phase is a
separate invocation (a flash or an OTA happens between them), so the legs share a
small JSON state file named by NIMBUS_L50_STATE. Every leg skips loudly when that
variable, the phase's firmware or the provider it needs is missing.

Env:
  NIMBUS_TEST_IP / NIMBUS_TEST_TOKEN  the unit (LAN-only mode: pass --port with a path
                                      that does not exist, so no console is opened and
                                      the trial boot is never reset by the fixture)
  NIMBUS_L50_STATE                    state file shared across the phases
  NIMBUS_L50_PROVIDER                 the keyed provider whose list the legs verify
                                      (default: the first keyed of cumulo, zai, anthropic:
                                      their verify costs nothing but Anthropic's one
                                      max_tokens=1 probe; Mistral and OpenAI probes are paid)
  NIMBUS_L50_LEGACY_FW                the older build the OTA starts from (default v4.5.14)
  NIMBUS_L50_RAISE_CEILING=1          the fallback leg may lift a reached monthly spend
                                      ceiling for its two LAN-mock turns (put back after)
No paid model call: verifies are free GETs (Anthropic's adds a max_tokens=1 probe of
the selected models), and the fallback leg runs on the LAN mock LLM.
"""

from __future__ import annotations

import json
import os
import time

import pytest

from mock_llm import MOCK_REPLY, find_lan_ip, mock_llm  # noqa: F401  (mock_llm is a fixture)

pytestmark = [pytest.mark.net]

LEGACY_FW = os.environ.get("NIMBUS_L50_LEGACY_FW", "v4.5.14")
VERIFY_WAIT_S = 90.0


# ---- helpers ---------------------------------------------------------------------


def _state_path() -> str:
    p = os.environ.get("NIMBUS_L50_STATE", "")
    if not p:
        pytest.skip("NIMBUS_L50_STATE is not set (the phases share it)")
    return p


def _load() -> dict:
    p = _state_path()
    if not os.path.exists(p):
        return {}
    with open(p) as f:
        return json.load(f)


def _save(d: dict) -> None:
    with open(_state_path(), "w") as f:
        json.dump(d, f, indent=1)


def _state(net) -> dict:
    return net.get_json("/api/state", timeout=10.0)


def _fw(net) -> str:
    return str(_state(net).get("fw", ""))


def _orch(net) -> dict:
    return net.get_json("/api/orch", timeout=10.0)


def _nvs_keys(net) -> dict:
    d = net.get_json("/api/nvs/stats", timeout=15.0)
    return {k["key"]: k for k in d.get("keys", []) if k.get("ns") == "solide"} | {"__entries": d.get("entries", {})}


def _models(net, provider: str) -> dict:
    d = net.get_json("/api/models?all=1", timeout=20.0)
    return d["providers"][provider]


def _log_tail(net, nbytes: int = 24000) -> str:
    r = net.get(f"/api/errlog/recent?bytes={nbytes}", timeout=15.0)
    return r.text if r.status_code == 200 else ""


def _log_since(net, before: str, nbytes: int = 40000) -> str:
    """The log written after `before` (an earlier _log_tail): the text past its last
    line, so a line an older boot or build wrote never counts."""
    after = _log_tail(net, nbytes)
    lines = [ln for ln in before.splitlines() if ln.strip()]
    anchor = lines[-1] if lines else ""
    at = after.rfind(anchor) if anchor else -1
    return after[at + len(anchor) :] if at >= 0 else after


def _provider(net) -> str:
    want = os.environ.get("NIMBUS_L50_PROVIDER", "")
    provs = _orch(net).get("providers", {})
    for p in [want] if want else ["cumulo", "zai", "anthropic"]:
        if provs.get(p, {}).get("hasKey"):
            return p
    pytest.skip("no keyed provider with a free verify on this unit (set NIMBUS_L50_PROVIDER)")


def _verify(net, provider: str) -> None:
    before = _orch(net)["providers"][provider].get("vts", 0)
    for _ in range(10):
        r = net.post("/api/verify", {"provider": provider}, timeout=10.0)
        if r.status_code == 200:
            break
        time.sleep(6)  # 409: another verify runs
    else:
        pytest.fail(f"verify {provider} never started")
    end = time.time() + VERIFY_WAIT_S
    while time.time() < end:
        time.sleep(3)
        p = _orch(net)["providers"][provider]
        if p.get("vts", 0) != before:
            return
    pytest.fail(f"verify {provider} did not finish in {VERIFY_WAIT_S:.0f} s")


# ---- the OTA move (unit L) --------------------------------------------------------


def test_l50_a_legacy_baseline(net):
    """On the older build: a verify leaves the short list in NVS (mch_<p>) and the
    JSON catalog file the picker reads."""
    fw = _fw(net)
    if not fw.startswith(LEGACY_FW):
        pytest.skip(f"phase a runs on {LEGACY_FW} (unit runs {fw})")
    p = _provider(net)
    _verify(net, p)
    keys = _nvs_keys(net)
    assert f"mch_{p}" in keys, f"{LEGACY_FW} kept no NVS short list for {p}"
    models = _models(net, p).get("models", [])
    assert models, f"{LEGACY_FW} shows no catalog for {p}"
    _save(
        {
            "provider": p,
            "mch": _orch(net)["providers"][p].get("choices", ""),
            "mch_entries": sum(v.get("entries", 0) for k, v in keys.items() if k.startswith("mch_")),
            "mch_keys": sorted(k for k in keys if k.startswith("mch_")),
            "nvs_free_entries": keys["__entries"].get("free", 0),
            "legacy_models": len(models),
            "fails": _state(net).get("nvs", {}).get("fails", 0),
        }
    )


def test_l50_b_trial_boot_merges_and_keeps_the_nvs_list(net):
    """First boot of the new image after the OTA (not yet marked valid): the older
    lists are merged into the store, and the NVS short list is still there for a
    rollback."""
    st = _load()
    if not st.get("provider"):
        pytest.skip("phase a has not run")
    s = _state(net)
    if s.get("fw", "").startswith(LEGACY_FW):
        pytest.skip("phase b runs on the new image")
    p = st["provider"]
    assert not str(s.get("lastOta", "")).startswith("ok "), "already marked valid: not a trial boot"
    m = _models(net, p)
    assert m.get("listSrc") == "legacy", m.get("listSrc")
    ids = {(x.get("upstream") + "/" if x.get("upstream") else "") + x["id"] for x in m.get("models", [])}
    for mid in filter(None, st["mch"].split(",")):
        assert mid.strip() in ids, f"{mid} from the NVS short list is not in the merged store"
    assert f"mch_{p}" in _nvs_keys(net), "the NVS short list went on the trial boot (no way back)"
    assert "models: merged" in _log_tail(net)


def test_l50_c_boot_after_mark_valid_erases_the_nvs_list(net):
    """Marked valid, then restarted: the NVS short lists are gone, NVS has more free
    entries, and no save failed doing it."""
    st = _load()
    if not st.get("provider"):
        pytest.skip("phase a has not run")
    s = _state(net)
    assert str(s.get("lastOta", "")).startswith("ok "), f"not marked valid yet: {s.get('lastOta')}"
    keys = _nvs_keys(net)
    left = [k for k in keys if k.startswith("mch_")]
    assert not left, f"NVS short lists left after the valid boot: {left}"
    assert keys["__entries"].get("free", 0) >= st["nvs_free_entries"] + st["mch_entries"] - 2
    assert s.get("nvs", {}).get("fails", 0) <= st["fails"], "a save failed during the move"
    assert _models(net, st["provider"]).get("models"), "the store lost the list"


def test_l50_d_verify_writes_the_store_with_no_nvs_write(net):
    """A verify on the new image rewrites the provider's store (src api, a dated
    fetch) and writes no NVS key beyond the verify verdict itself."""
    p = _provider(net)
    if _fw(net).startswith(LEGACY_FW):
        pytest.skip("phase d runs on the new image")
    before = {k: v for k, v in _nvs_keys(net).items() if not k.startswith("__")}
    log0 = _log_tail(net)
    _verify(net, p)
    m = _models(net, p)
    assert m.get("listSrc") == "api", m.get("listSrc")
    assert m.get("models"), "no models stored"
    if _state(net).get("timeSynced", True):
        assert m.get("fetchedAt", 0) > 1600000000, m.get("fetchedAt")
    assert not m.get("incomplete"), "the list arrived incomplete"
    after = {k: v for k, v in _nvs_keys(net).items() if not k.startswith("__")}
    new = sorted(set(after) - set(before))
    assert not [k for k in new if not k.startswith(("vfy_", "vrs_", "vrm_"))], f"new NVS keys: {new}"
    assert not [k for k in after if k.startswith("mch_")], "an NVS short list was written"
    log = _log_since(net, log0)
    assert f"verify: {p} models read:" in log and f"verify: {p} models stored:" in log
    assert "models harvested" not in log
    # Every row says its price or that it is unknown.
    for row in m["models"]:
        assert row.get("priceSrc") in ("router", "builtin", "unknown"), row


def test_l50_e_rollback_still_reads_the_legacy_catalog(net):
    """Back on the older build: its JSON catalog file is still there and read."""
    st = _load()
    if not st.get("provider"):
        pytest.skip("phase a has not run")
    if not _fw(net).startswith(LEGACY_FW):
        pytest.skip(f"phase e runs on {LEGACY_FW}")
    assert _models(net, st["provider"]).get("models"), "the older build lost its catalog"


# ---- the router's whole list (unit A, a Cumulo key) --------------------------------


def test_l50_f_router_list_is_whole_or_says_more(net):
    """The Cumulo router's ~95 KB list reaches the store: every routable model, one
    row each, or the stored list says how many more there are; never cut silently."""
    if not _orch(net).get("providers", {}).get("cumulo", {}).get("hasKey"):
        pytest.skip("no Cumulo key on this unit")
    if _fw(net).startswith(LEGACY_FW):
        pytest.skip("runs on the new image")
    log0 = _log_tail(net)
    _verify(net, "cumulo")
    m = _models(net, "cumulo")
    rows = m.get("models", [])
    more = sum(int(r.get("more", 0) or 0) for r in rows)
    assert not m.get("incomplete"), "the router list arrived cut short"
    assert m.get("total", 0) == len(rows) + more, (m.get("total"), len(rows), more)
    assert len(rows) > 12, "only the old 12-id list came through"
    sels = [(r.get("upstream", "") + "/" + r["id"]) for r in rows]
    keys = [s.replace(".", "-") for s in sels]
    assert len(set(keys)) == len(keys) or all(not any(c.isdigit() for c in s) for s in sels), "a model takes two slots"
    assert all(r.get("priceSrc") in ("router", "builtin", "unknown") for r in rows)
    assert "verify: cumulo models read:" in _log_since(net, log0)


# ---- HOST-FALLBACK (zero spend: the LAN mock LLM is the keyed host) ------------------


def _restart(net) -> None:
    """Restart over HTTP (the custom head registers only at boot) and wait for the
    web server to answer again."""
    net.post("/api/restart", {"confirm": "1"}, timeout=5.0)
    time.sleep(20)
    assert net.wait_reachable(120.0), "the unit did not come back after the restart"


def _chat(net, text: str, wait: float = 90.0) -> str:
    """Send one web-chat message and return THAT turn's reply (``GET /api/chat?turn=<id>``,
    never the legacy slot, which still holds the previous reply while this one waits)."""
    r = net.post("/api/chat", {"text": text}, timeout=15.0)
    assert r.status_code in (200, 202), r.status_code
    turn = str(r.json().get("turn") or "")
    assert turn, f"/api/chat gave no turn id: {r.text[:200]}"
    end = time.time() + wait
    while time.time() < end:
        time.sleep(3)
        d = net.get_json(f"/api/chat?turn={turn}", timeout=10.0)
        assert not d.get("gone"), f"turn {turn} is gone before it was answered"
        if d.get("done"):
            return str(d.get("reply") or "")
    pytest.fail(f"turn {turn} was not answered within {wait:.0f}s")


def test_l50_g_keyless_pinned_head_runs_on_a_keyed_host(net, mock_llm):  # noqa: F811
    """The 22 Oct bench: the pinned head's key was cleared and the turn dialed it
    anyway (HTTP 401). Pin a provider with no key, make the LAN mock the keyed host
    in the priority list, send two turns: both run on the mock, the owner is told
    once, and nothing is sent to the keyless provider."""
    orch = _orch(net)
    provs = orch.get("providers", {})
    keyless = next(
        (p for p in ("anthropic", "openai", "mistral", "zai", "cumulo") if not provs.get(p, {}).get("hasKey")), ""
    )
    if not keyless:
        pytest.skip("every provider has a key on this unit: nothing to pin keyless")
    spend = _state(net).get("spend", {}) or {}
    ceiling0 = int(spend.get("limitCents", 0) or 0)
    if spend.get("frozen"):
        if os.environ.get("NIMBUS_L50_RAISE_CEILING") != "1":
            pytest.skip(
                "the monthly spend ceiling is reached on this unit (set NIMBUS_L50_RAISE_CEILING=1 "
                "to raise it for this leg; it is put back afterwards)"
            )
        assert (
            net.post("/api/orch", {"spendCeil": str(int(spend.get("spentCents", 0)) + 5000)}, timeout=10.0).status_code
            == 200
        )
    cust = orch.get("cust", {}) or {}
    saved = {
        "orchHost": orch.get("orchHost", ""),
        "provPrio": orch.get("provPrio", ""),
        "custBase": cust.get("base", ""),
        "custConv": cust.get("conv", "") or "openai",
        "custModel": cust.get("model", ""),
    }
    ip = net.ip
    base = mock_llm.base_url(find_lan_ip(ip))
    try:
        assert (
            net.post(
                "/api/orch", {"custBase": base, "custConv": "openai", "custModel": "mock-echo"}, timeout=10.0
            ).status_code
            == 200
        )
        _restart(net)
        assert (
            net.post("/api/orch", {"orchHost": keyless, "provPrio": f"{keyless},custom"}, timeout=10.0).status_code
            == 200
        )
        mock_llm.reset()
        log0 = _log_tail(net)
        r1 = _chat(net, "Reply with one word: ok")
        r2 = _chat(net, "Reply with one word: again")
        assert MOCK_REPLY in r1 and MOCK_REPLY in r2, (r1, r2)
        assert len(mock_llm.completions()) >= 2, "the turns did not run on the keyed host"
        log = _log_since(net, log0)
        assert log.count(f"pinned head {keyless} has no key - running on custom") >= 2
        assert "rejected the key" not in r1 + r2
        assert "No AI provider has a key" not in r1 + r2
    finally:
        restore = dict(saved, convReset="1")
        if spend.get("frozen") and ceiling0:
            restore["spendCeil"] = str(ceiling0)
        net.post("/api/orch", restore, timeout=10.0)
        _restart(net)
