"""L43 - the settings-storage (NVS) reserve on a filling device (v4.5.14).

A desk device's NVS filled in October 2026 and every write failed alike: the
Telegram offset (a replay loop of paid turns), the OTA rollback guard, the usage
ledger and the scratchpad. The fix (host-tested in test/test_nvs_full,
test_orch_journal, test_ota_logic, test_usbfw_proto, test_orch_usage_ledger):
every key has a class; the last 24 free entries are kept for critical writes (the
offset, the OTA guard, the credentials); erases and non-growing rewrites always
land; a refusal answers 507 storage_full; a main-loop pass evicts rebuildable
caches, and a device that still cannot get back to the reserve is "starved" (owner
settings save again down to a floor of 8 free entries, background saves stay
refused). The usage ledger and the scratchpad moved to flash files.

Legs (unit L, the fill unit, on the TEST build: ``NVSFILL``, ``TURN``, ``OTASIM``):

* ``test_l43_state_fields`` (net): ``/api/state`` carries ``nvsReserve`` and
  ``usageSaved``.
* ``test_l43_reserve_refuses_noncritical_while_critical_lands`` (hil + net): fill to
  reserve+1 free; owner saves that grow NVS (a long directive, the fallback rules)
  answer 507 storage_full and count in ``nvsReserve.refused``; the scratchpad (a
  flash file now) still saves; the OTA guard keys (critical) still land
  (``OTASIM arm``) and their clear still lands.
* ``test_l43_offset_lands_at_reserve`` (net + manual, the dedicated test bot): at
  reserve+1 the operator sends one message; the Telegram offset lands (no
  ``tgOffset`` in ``nvs.recentFails``) while a save answers 507.
* ``test_l43_spawn_routine_restart_at_reserve`` (hil + net + agent, about $0.06 on
  ``claude-haiku-5-5``; v4.6, lane C2): at reserve+1 a turn spawns two sub-agents and
  a routine fires; then the device restarts. Every moved store is a file in /data
  (``FLSTAT``: the sub-agent journal, routines, the usage ledger, the managed-agent
  ids), nothing new is refused in ``nvs.recentFails`` (no ``agjournal.*``, no file
  key), the settings storage did not shrink, and the jobs finish.
* ``test_l43_running_helper_survives_a_restart_at_reserve`` (hil + net + agent, about
  $0.03; SPAWN-JOURNAL-DURABILITY): at reserve+1 a helper is started and the device
  restarted while it runs; after the boot the helper is polled again and its result
  arrives (on v4.5.16 it was lost silently).
* ``test_l43_finished_jobs_listed_after_restart`` (hil + net + agent, about $0.06;
  L19-35): two helpers finish with saved results; after a restart an unrelated owner
  turn's context (``/api/lastturn``) lists both finished jobs and the turn starts no
  helper. The counter-leg (the record removed: the head re-runs the work) is the live
  harness-lab scenario ``finished-jobs``.
* ``test_l43_battmodel_flap_spends_nothing`` (hil; BATTMODEL-CHURN): after one warm-up
  move (the boot's first save may go at once), ``BATTFLAP 200`` moves the battery
  model's full-charge anchor by a few millivolts 200 times through the paced save: no
  save, no settings-storage entry spent, both on a roomy store and at reserve+1; a real
  step saves at most once.
* ``test_l43_otasim_rollback_at_reserve`` (hil): at reserve+1 the bad-image drill
  (``OTASIM arm`` + ``OTASIM crash``) still rolls back within 3 boots.
* ``test_l43_ota_rc1_to_rc2`` (hil + manual, ``NIMBUS_L43_OTA_BASE`` = a bench OTA
  server serving rc2, the device on rc1): at reserve+1 the install either arms the
  guard (the new image boots pending and is marked valid) or refuses with
  ``storage_full`` before the flip. Never a flip without a guard.

The console verbs' exact forms come from ``NIMBUS_L43_FILL`` (default
``NVSFILL {n}``) and ``NIMBUS_L43_FREE`` (default ``NVSFILL 0``). The model is
``NIMBUS_LAB_MODEL`` (default ``claude-haiku-5-5``). Tokens are never printed. The
pure helpers are host-tested at the bottom (``-m host``).
"""

from __future__ import annotations

import os
import time
from typing import List, Optional

import pytest

import bench_lock

RESERVE = 24  # nimbus::store::kNvsReserveEntries
ENTRY_BYTES = 32
STORAGE_FULL = "storage_full"
MODEL = os.environ.get("NIMBUS_LAB_MODEL", "claude-haiku-5-5")
# An owner value that grows NVS by about 10 entries (refused at reserve+1).
GROWING_DIRECTIVE = "Reply briefly. " + "This bench directive only exists to grow the settings storage. " * 5


# ---- pure helpers (host-tested) ---------------------------------------------------


def free_entries(state: dict) -> int:
    """Free NVS entries from one ``/api/state`` read (``nvs.free`` is bytes)."""
    return int(((state or {}).get("nvs") or {}).get("free", 0)) // ENTRY_BYTES


def fill_count(state: dict, leave: int = RESERVE + 1) -> int:
    """How many one-entry filler keys leave exactly ``leave`` entries free."""
    return max(0, free_entries(state) - leave)


def failed_keys(state: dict, prefix: str = "") -> List[str]:
    """The keys in ``nvs.recentFails`` (newest first), optionally by prefix."""
    fails = ((state or {}).get("nvs") or {}).get("recentFails") or []
    return [f.get("key", "") for f in fails if f.get("key", "").startswith(prefix)]


def is_storage_full(resp) -> bool:
    try:
        body = resp.json()
    except Exception:  # noqa: BLE001 - a non-JSON body is not the contract
        return False
    return resp.status_code == 507 and body.get("error") == STORAGE_FULL and "ok" not in body


def rolled_back(ota_line: Optional[dict]) -> bool:
    """True when ``OTA?`` reports a finished rollback drill."""
    last = (ota_line or {}).get("last", "")
    return last.startswith("rollback") and (ota_line or {}).get("pend") == "0"


# ---- device plumbing ----------------------------------------------------------------


def state(net) -> dict:
    return net.get_json("/api/state", timeout=8.0)


def nvs_fill(device, n: int) -> None:
    device.cmd(os.environ.get("NIMBUS_L43_FILL", "NVSFILL {n}").format(n=n), expect="NVSFILL", timeout=20.0)


def nvs_free(device) -> None:
    device.cmd(os.environ.get("NIMBUS_L43_FREE", "NVSFILL 0"), expect="NVSFILL", timeout=20.0)


def fill_to_reserve_plus_one(device, net) -> dict:
    nvs_free(device)
    st = state(net)
    nvs_fill(device, fill_count(st))
    st = state(net)
    free = free_entries(st)
    # NVSFILL writes one-entry ints; page geometry can leave one or two entries of slack.
    assert RESERVE <= free <= RESERVE + 3, f"fill left {free} free entries, wanted {RESERVE + 1}"
    return st


def ota_state(device) -> dict:
    m = device.cmd_re(
        "OTA?",
        r"OTA state=(?P<state>\S+) err=(?P<err>\S+) pct=(?P<pct>-?\d+) latest=(?P<latest>\S*) "
        r"notes=(?P<notes>.*?) lastOta=(?P<last>.*?) pend=(?P<pend>\d+) "
        r"boots=(?P<boots>\d+) prev=(?P<prev>\S*) slot=(?P<slot>\S+)",
        timeout=6.0,
    )
    return m.groupdict()


def wait_until(pred, timeout: float, every: float = 2.0) -> bool:
    end = time.time() + timeout
    while time.time() < end:
        if pred():
            return True
        time.sleep(every)
    return False


@pytest.fixture
def orch(device, net, secrets, require_secret):
    """Orchestrator mode on Wi-Fi; the filler is removed afterwards whatever happens."""
    require_secret(secrets.require_sta)
    device.ensure_mode(1)
    st = device.status()
    if st.group("wifi") == "1":
        net.ip = st.group("ip")
    else:
        net.provision(secrets.sta_ssid, secrets.sta_pass)
        net.wait_got_ip(timeout=25.0)
    yield
    try:
        nvs_free(device)
    except Exception:  # noqa: BLE001 - the console may be gone after a failed leg
        pass


# ---- LAN legs -------------------------------------------------------------------------


@pytest.mark.net
def test_l43_state_fields(net):
    st = state(net)
    r = st.get("nvsReserve") or {}
    assert r.get("entries") == RESERVE, r
    for k in ("refused", "evicted"):
        assert isinstance(r.get(k), int), f"nvsReserve.{k} missing: {r}"
    assert isinstance(r.get("starved"), bool), r
    assert isinstance(st.get("usageSaved"), bool), st.keys()


@pytest.mark.hil
@pytest.mark.net
def test_l43_reserve_refuses_noncritical_while_critical_lands(device, net, orch):
    before = (state(net).get("nvsReserve") or {}).get("refused", 0)
    fill_to_reserve_plus_one(device, net)

    # Owner config that grows NVS: refused with the storage-full answer, never ok.
    # A directive of ~10 entries grows NVS well past one entry (a small rewrite could
    # legally land at reserve+1: it would leave exactly the reserve).
    r = net.post("/api/orch", {"sysPrompt": GROWING_DIRECTIVE}, timeout=8.0)
    assert is_storage_full(r), f"a directive save at reserve+1: HTTP {r.status_code} {r.text[:200]}"
    rules = '{"v":1,"rules":[{"when":{"size":"large"},"use":["anthropic"]}]}'
    r = net.post("/api/fallbacks", {"rules": rules}, timeout=8.0)
    assert r.status_code in (400, 507), r.text[:200]  # 400 only if the bench rules shape is refused
    if r.status_code == 507:
        assert is_storage_full(r)
    st = state(net)
    assert (st.get("nvsReserve") or {}).get("refused", 0) > before, "no refusal was counted"
    assert (st.get("nvsReserve") or {}).get("starved") is False, "starved while the reserve still holds"

    # The scratchpad lives in a flash file now: it still saves on a full NVS.
    r = net.post("/api/mem/scratchpad", {"action": "add", "tier": "short", "text": "l43 reserve probe"}, timeout=8.0)
    assert r.status_code == 200, f"scratchpad save on a full NVS: HTTP {r.status_code} {r.text[:200]}"
    net.post("/api/mem/scratchpad", {"action": "clear", "tier": "short"}, timeout=8.0)

    # Critical writes still land: the OTA guard keys through the drill seam.
    try:
        device.cmd("OTASIM arm l43", expect="OTASIM armed", timeout=6.0)
        ota = ota_state(device)
        assert ota["pend"] == "1" and ota["prev"] == "l43", f"the OTA guard did not land: {ota}"
    finally:
        device.cmd("OTASIM clear", expect="OTASIM cleared", timeout=6.0)
    assert ota_state(device)["pend"] == "0", "the guard clear (a rewrite) did not land"
    assert "otaPend" not in failed_keys(state(net)), "a critical key failed at reserve+1"


@pytest.mark.net
@pytest.mark.manual
def test_l43_offset_lands_at_reserve(device, net, orch, secrets, require_secret, require_manual):
    require_secret(secrets.require_test_telegram)
    post = lambda f: net.post("/api/orch", f, timeout=8.0)  # noqa: E731
    with bench_lock.bot_on_unit(post, secrets.tg_test_token, secrets.tg_test_chat, "l43 offset"):
        _offset_at_reserve(device, net, require_manual)


def _offset_at_reserve(device, net, require_manual):
    try:
        fill_to_reserve_plus_one(device, net)
        require_manual.confirm("From the test chat, send: reserve one\nThen press y.", timeout=180.0)
        time.sleep(40)  # one long poll after the reply
        st = state(net)
        assert "tgOffset" not in failed_keys(st), f"tgOffset failed at reserve+1: {failed_keys(st)}"
        if "tgOffsetSaved" in st:
            assert st["tgOffsetSaved"] is True, "the stored offset is behind at reserve+1"
        r = net.post("/api/orch", {"sysPrompt": GROWING_DIRECTIVE}, timeout=8.0)
        assert is_storage_full(r), f"a growing save at reserve+1 was not refused: HTTP {r.status_code}"
    finally:
        nvs_free(device)


def use_haiku(net) -> None:
    """Head and helpers on the cheap model (owner config: set before the fill)."""
    for k, v in (
        ("orchHost", "anthropic"),
        ("orchM_anthropic", MODEL),
        ("subM_anthropic", MODEL),
        ("subPrio", "anthropic"),
    ):
        r = net.post("/api/orch", {k: v}, timeout=8.0)
        assert r.status_code == 200, f"{k}: HTTP {r.status_code} {r.text[:120]}"


def flash_file(device, path: str) -> int:
    """Bytes of a file on the internal flash (``FLSTAT``), -1 when it is absent."""
    m = device.cmd_re(f"FLSTAT {path}", r"FLSTAT \S+ exists=(?P<e>[01]) bytes=(?P<b>\d+)", timeout=6.0)
    return int(m.group("b")) if m.group("e") == "1" else -1


def count_spawns(device, want: int, timeout: float) -> int:
    spawned = 0
    end = time.time() + timeout
    while time.time() < end and spawned < want:
        try:
            device.expect_re(r"orchestrator: spawned job\d+", timeout=max(1.0, end - time.time()))
            spawned += 1
        except Exception:  # noqa: BLE001 - fewer spawns than asked: judged by the caller
            break
    return spawned


def jobs_drained(net, timeout: float) -> bool:
    return wait_until(lambda: int(state(net).get("jobs", 0) or 0) == 0, timeout, every=5.0)


def restart(device, net) -> None:
    device.reboot_and_confirm(timeout=40.0)
    assert wait_until(lambda: device.status().group("wifi") == "1", 60, every=3.0), "no Wi-Fi after the restart"
    net.ip = device.status().group("ip")

    def up() -> bool:
        try:
            return bool(state(net))
        except Exception:  # noqa: BLE001 - the web server needs a few seconds after Wi-Fi
            return False

    assert wait_until(up, 60, every=3.0), "the web server did not come back"


MOVED_FILES = ("/data/agjournal.json", "/data/loops.json", "/data/usage_ledger.txt")
ANT_FILES = ("/data/ant_env.txt", "/data/ant_agents.txt")


def assert_no_new_store_failures(st: dict, before: set) -> None:
    new = [k for k in failed_keys(st) if k not in before]
    assert not [k for k in new if k.startswith("agjournal")], f"a journal write reached NVS: {new}"
    assert not [k for k in new if k.startswith(("lfs_", "ant"))], f"a file store failed: {new}"


def assert_on_flash(device, paths, when: str) -> None:
    for path in paths:
        assert flash_file(device, path) > 0, f"{path} is not on flash {when}"


def start_routine_and_spawns(device, net) -> None:
    """A routine that fires in two minutes, and a turn that spawns two helpers."""
    sched = '{"kind":"once","in_seconds":120}'
    r = net.post(
        "/api/loops",
        {"action": "create", "name": "l43 routine", "prompt": "Reply with exactly: routine ping", "schedule": sched},
        timeout=8.0,
    )
    assert r.status_code == 200, f"routine create: HTTP {r.status_code} {r.text[:120]}"
    device.drain(quiet=0.3)
    device.turn(
        "Spawn two sub-agents now, each answering in one sentence: the chemical symbol for gold, "
        "and the capital of Peru. Then tell me both answers."
    )
    assert count_spawns(device, 2, 180) >= 1, "the turn spawned no sub-agent (nothing to judge)"
    assert jobs_drained(net, 300), "the jobs did not finish"
    device.expect_re(r"loops: fired 'l43 routine'", timeout=240.0)


@pytest.mark.hil
@pytest.mark.net
@pytest.mark.agent
def test_l43_spawn_routine_restart_at_reserve(device, net, orch, secrets):
    if not secrets.get("ANTHROPIC_API_KEY"):
        pytest.skip("no ANTHROPIC_API_KEY for the sub-agent leg")
    use_haiku(net)
    st0 = fill_to_reserve_plus_one(device, net)
    free0, fails0 = free_entries(st0), set(failed_keys(st0))
    start_routine_and_spawns(device, net)
    st = state(net)
    assert_no_new_store_failures(st, fails0)
    assert free_entries(st) >= free0 - 1, f"the settings storage shrank: {free0} -> {free_entries(st)}"
    assert_on_flash(device, MOVED_FILES, "after the spawn and the routine")
    # The managed-agent ids exist once an Anthropic sub ran through managed agents.
    ant = [flash_file(device, p) for p in ANT_FILES]
    restart(device, net)
    st = state(net)
    assert_no_new_store_failures(st, set())
    assert free_entries(st) >= free0 - 1, f"the settings storage shrank across the restart: {free_entries(st)}"
    assert_on_flash(device, MOVED_FILES, "after the restart")
    assert [flash_file(device, p) for p in ANT_FILES] == ant
    assert device.ping(timeout=6.0)


@pytest.mark.hil
@pytest.mark.net
@pytest.mark.agent
def test_l43_running_helper_survives_a_restart_at_reserve(device, net, orch, secrets):
    if not secrets.get("ANTHROPIC_API_KEY"):
        pytest.skip("no ANTHROPIC_API_KEY for the sub-agent leg")
    use_haiku(net)
    fill_to_reserve_plus_one(device, net)
    device.drain(quiet=0.3)
    device.turn(
        "Spawn one sub-agent named tide-check to explain in three sentences why the sea has two "
        "high tides a day. Do not answer it yourself."
    )
    m = device.expect_re(r"orchestrator: spawned (?P<tag>job\d+)", timeout=180.0)
    tag = m.group("tag")
    device.drain(quiet=0.2)
    restart(device, net)  # while the helper runs: v4.5.16 lost it here on a full store
    device.expect_re(rf"orchestrator: poll {tag} ", timeout=120.0)
    assert jobs_drained(net, 300), f"{tag} never finished after the restart"
    st = state(net)
    assert not failed_keys(st, "agjournal"), failed_keys(st)


@pytest.mark.hil
@pytest.mark.net
@pytest.mark.agent
def test_l43_finished_jobs_listed_after_restart(device, net, orch, secrets):
    if not secrets.get("ANTHROPIC_API_KEY"):
        pytest.skip("no ANTHROPIC_API_KEY for the sub-agent leg")
    use_haiku(net)
    device.drain(quiet=0.3)
    device.turn(
        "Spawn two sub-agents in project tides: one named tide-research explaining in two sentences "
        "why the sea has two high tides a day, one named moon-facts giving the Moon's average "
        "distance from Earth in one sentence. Then tell me when both are done."
    )
    assert count_spawns(device, 2, 180) == 2, "the turn did not spawn both helpers"
    assert jobs_drained(net, 300), "the helpers did not finish"
    time.sleep(20)  # the synthesis turn reports them
    restart(device, net)
    device.drain(quiet=0.3)
    device.turn("Unrelated question: what's a good name for a grey kitten?")
    device.expect_re(r"turn end: ", timeout=180.0)
    turn = net.get("/api/lastturn", timeout=10.0).text
    assert "Finished before the device last restarted" in turn, "no finished jobs in the turn context"
    for name in ("tide-research", "moon-facts"):
        assert name in turn, f"{name} is not listed after the restart"
    time.sleep(5)
    assert int(state(net).get("jobs", 0) or 0) == 0, "the unrelated turn started a helper"


@pytest.mark.hil
def test_l43_battmodel_flap_spends_nothing(device, net, orch):
    def flap(n: int, step: int = 0) -> dict:
        line = device.cmd_re(
            f"BATTFLAP {n}" + (f" {step}" if step else ""),
            r"BATTFLAP moves=(?P<n>\d+) step=(?P<s>-?\d+) saves=(?P<saves>\d+) "
            r"freeBefore=(?P<a>\d+) freeAfter=(?P<b>\d+)",
            timeout=30.0,
        )
        return {k: int(v) for k, v in line.groupdict().items()}

    # The first move of a boot may save once (a model never stored, or a change pending
    # since the boot); after it, flapping by a few millivolts saves nothing.
    warm = flap(1)
    assert warm["saves"] <= 1, f"one move saved more than once: {warm}"
    roomy = flap(200)
    assert roomy["saves"] == 0, f"a flapping anchor saved the model: {roomy}"
    assert roomy["b"] >= roomy["a"], f"a flapping anchor spent entries: {roomy}"
    fill_to_reserve_plus_one(device, net)
    full = flap(200)
    assert full["saves"] == 0 and full["b"] >= full["a"], f"at reserve+1: {full}"
    step = flap(6, 25)  # real steps: at most one save (the hourly pace)
    assert step["saves"] <= 1, f"real steps saved more than once in an hour: {step}"


@pytest.mark.hil
def test_l43_otasim_rollback_at_reserve(device, net, orch):
    fill_to_reserve_plus_one(device, net)
    device.drain(quiet=0.2)
    device.send("OTASIM arm l43-missing")  # an unfindable previous slot: rollback-lost, no reflash
    device.expect("OTASIM armed", timeout=6.0)
    device.send("OTASIM crash")
    device.expect("OTASIM crash-on-boot ARMED", timeout=6.0)
    try:
        device.send("REBOOT")
        # Each pending boot aborts before mark-valid; the guard must give up within 3.
        assert wait_until(lambda: device.ping(timeout=3.0) and rolled_back(ota_state(device)), 120, every=4.0), (
            "the bad image did not roll back within its attempt budget at reserve+1"
        )
        assert int(ota_state(device)["boots"]) == 0
    finally:
        device.send("OTASIM clear")


@pytest.mark.hil
@pytest.mark.manual
def test_l43_ota_rc1_to_rc2(device, net, orch, require_manual):
    base = os.environ.get("NIMBUS_L43_OTA_BASE")
    if not base:
        pytest.skip("set NIMBUS_L43_OTA_BASE to the bench OTA server serving rc2 (device on rc1)")
    fill_to_reserve_plus_one(device, net)
    slot0 = ota_state(device)["slot"]
    device.cmd_re(f"OTAURL {base}/manifest.json", r"OTAURL ", timeout=5.0)
    device.cmd_re("OTACHECK", r"OTACHECK (started|refused.*)", timeout=6.0)
    assert wait_until(lambda: ota_state(device)["state"] == "available", 120), "rc2 not seen"
    device.cmd_re("OTAAPPLY", r"OTAAPPLY (started|refused.*)", timeout=6.0)
    end = time.time() + 600
    outcome = None
    while time.time() < end and outcome is None:
        time.sleep(5)
        try:
            st = ota_state(device)
        except Exception:  # noqa: BLE001 - rebooting into rc2
            continue
        if st["state"] == "error":
            outcome = ("refused", st)
        elif st["slot"] != slot0:
            outcome = ("flipped", st)
    assert outcome, "the install neither refused nor flipped in 10 minutes"
    kind, st = outcome
    if kind == "refused":
        assert st["err"] == STORAGE_FULL, f"refused for another reason: {st}"
        assert st["slot"] == slot0 and st["pend"] == "0", f"a refused install left the guard armed: {st}"
    else:
        # The new image booted with the guard armed, then marks itself valid when healthy.
        assert wait_until(lambda: ota_state(device)["last"].startswith("ok "), 700, every=10.0), (
            "rc2 did not mark itself valid"
        )
    require_manual.confirm("The device shows its normal home screen (not a rollback notice). Press y.", timeout=120.0)


# ---- host tests for the helpers -----------------------------------------------------------


@pytest.mark.host
def test_host_free_entries_and_fill_count():
    st = {"nvs": {"free": 32 * 100}}
    assert free_entries(st) == 100
    assert fill_count(st) == 100 - (RESERVE + 1)
    assert fill_count({"nvs": {"free": 32 * 10}}) == 0
    assert free_entries({}) == 0


@pytest.mark.host
def test_host_failed_keys():
    st = {"nvs": {"recentFails": [{"key": "agjournal.j1", "n": 3}, {"key": "orchConv", "n": 2}]}}
    assert failed_keys(st, "agjournal.") == ["agjournal.j1"]
    assert failed_keys(st) == ["agjournal.j1", "orchConv"]
    assert failed_keys({}) == []


class _Resp:
    def __init__(self, code, body):
        self.status_code = code
        self._body = body

    def json(self):
        if isinstance(self._body, Exception):
            raise self._body
        return self._body


@pytest.mark.host
def test_host_is_storage_full():
    assert is_storage_full(_Resp(507, {"error": "storage_full", "message": "x"}))
    assert not is_storage_full(_Resp(200, {"ok": True}))
    assert not is_storage_full(_Resp(507, {"error": "storage_full", "ok": True}))
    assert not is_storage_full(_Resp(507, ValueError("not json")))


@pytest.mark.host
def test_host_moved_store_paths_match_the_firmware_table():
    """The files the bench leg looks for are the ones the firmware's moved-store table names."""
    from pathlib import Path

    table = (Path(__file__).resolve().parents[2] / "lib/core/include/nimbus/store/moved_store.h").read_text()
    for path in ("/data/agjournal.json", "/data/usage_ledger.txt", "/data/ant_env.txt", "/data/ant_agents.txt"):
        assert f'"{path}"' in table, path


@pytest.mark.host
def test_host_rolled_back():
    assert rolled_back({"last": "rollback-lost", "pend": "0"})
    assert rolled_back({"last": "rollback v4.5.14", "pend": "0"})
    assert not rolled_back({"last": "sim-arm l43", "pend": "1"})
    assert not rolled_back(None)
