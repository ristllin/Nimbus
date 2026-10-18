"""L42 - Telegram on a full settings storage: no replay, commands on their own.

The 14 Oct 2026 loop: a desk device's NVS reached 0 free entries, the Telegram
offset write failed unnoticed, and the poll loop re-read the stale stored offset
after every drain, so Telegram served the same page every ~2.5 s (a photo taken in
17 times, a 4-message batch handled 11 times). The fix (host-tested in
test/test_telegram_offset, test_orch_msg_batch, test_tg_backoff, test_nvs_full):
the offset lives in RAM while the device runs and only moves forward; its stored
copy is written once per new page and retried at most once a minute; a slash
command in a burst runs as its own turn; NVS failures are visible
(``nvs.recentFails``, ``GET /api/nvs/stats``, a once-per-boot owner notice).

Legs (unit L, the fill unit, on the TEST build so the ``NVSFILL`` console verb exists):

* ``test_l42_state_fields`` (net): ``/api/state`` carries ``tgOffsetSaved``,
  ``tgPoll`` and ``nvs.recentFails``.
* ``test_l42_nvs_stats_names_only`` (net): ``GET /api/nvs/stats`` needs the
  sign-in token, lists keys with names, types, sizes and entry counts, and never a
  value.
* ``test_l42_fill_burst_restart_erase`` (net + manual, the dedicated test bot): fill
  NVS to 0 free, the operator sends a burst from the test chat, then: one reply
  turn, ``pending_update_count`` 0 on the next cycle, ``tgOffset`` failing at most
  once per new page and once a minute idle, ``tgOffsetSaved`` false, a restart one
  cycle after the reply replays nothing, a restart right after a drain replays at
  most that page, and freeing NVS lets the write land within a minute.
* ``test_l42_v4513_control_loops`` (net + manual, ``NIMBUS_L42_CONTROL=1``, on a
  v4.5.13 image): the same fill replays the same message again and again.
* ``test_l42_injected_burst_splits_commands`` (hil + net, no phone): the console's
  ``TGINJECT`` feeds the owner's burst "one", "two", "/help", "four" as one raw
  getUpdates page (telegram::injectRawUpdates): three turns, /help answered by the
  device with no model turn, and the offset untouched. The real-phone burst stays the
  human gate (H1-1).

``NVSFILL`` and ``TGINJECT`` belong to the test console (lane N). Their exact forms
are read from ``NIMBUS_L42_FILL`` (default ``NVSFILL {n}``, filling ``n`` entries),
``NIMBUS_L42_FREE`` (default ``NVSFILL 0``, erasing the filler) and
``NIMBUS_L42_INJECT`` (default ``TGINJECT {json}``) so the legs follow the console
without an edit. Tokens are never printed.

The pure helpers are host-tested at the bottom (``-m host``).
"""

from __future__ import annotations

import json
import os
import time
import urllib.request
from typing import Dict, Iterable, List, Optional

import pytest

RETRY_QUIET_S = 60  # nimbus::core::TgOffsetCursor::kRetryQuietMs
LONG_POLL_S = 30  # TELEGRAM_LONG_POLL_TIMEOUT_S
BURST = ("repro one", "repro two", "/help", "repro four")
BURST2 = ("again one", "again two", "again three")


# ---- pure helpers (host-tested) ---------------------------------------------------


def offset_fail_numbers(state: dict, key: str = "tgOffset") -> List[int]:
    """The failure numbers (``n``) of ``key`` in one ``nvs.recentFails`` read."""
    fails = ((state or {}).get("nvs") or {}).get("recentFails") or []
    return sorted(int(f["n"]) for f in fails if f.get("key") == key)


def fail_budget(new_pages: int, idle_s: float) -> int:
    """The most ``tgOffset`` write failures the cursor may cause: one per newly acked
    page, plus one retry per minute while a write is owed, plus one in flight."""
    return new_pages + int(idle_s // RETRY_QUIET_S) + 1


def dispatch_counts(rows: Iterable[dict], texts: Iterable[str]) -> Dict[str, int]:
    """How many user rows of the episodic log carry each sent text (a batched turn
    renders several texts in one row, so a text counts once per row it appears in)."""
    want = list(texts)
    counts = {t: 0 for t in want}
    for r in rows:
        if r.get("role") != "user":
            continue
        body = r.get("text") or ""
        for t in want:
            if t in body:
                counts[t] += 1
    return counts


BLOB_CHUNK_BYTES = 4000  # a blob chunk lives inside one 126-entry NVS page


def entries_consistent(stats: dict) -> List[str]:
    """Problems with a ``GET /api/nvs/stats`` body (empty list = fine).

    Bounds on the listed entry total against ``entries.used`` (nvs_get_stats): ints and
    strings are counted exactly; a blob is counted as one chunk, so a blob split across
    pages reads one entry low per extra chunk (at most ``ceil(bytes / 4000)``), and the
    namespace index entries (one per namespace) are not keys. So
    ``used - namespaces - blob slack <= listed <= used``."""
    problems = []
    keys = stats.get("keys") or []
    allowed = {"ns", "key", "type", "bytes", "entries"}
    for k in keys:
        extra = set(k) - allowed
        if extra:
            problems.append(f"{k.get('key')}: unexpected fields {sorted(extra)}")
        if k.get("type") not in ("int", "str", "blob", "other"):
            problems.append(f"{k.get('key')}: type {k.get('type')!r}")
        if not isinstance(k.get("entries"), int) or k["entries"] < 1:
            problems.append(f"{k.get('key')}: entries {k.get('entries')!r}")
    if stats.get("count") != len(keys):
        problems.append("count does not match keys")
    if stats.get("truncated"):
        return problems
    totals = stats.get("entries") or {}
    used = totals.get("used", 0)
    listed = sum(k.get("entries", 0) for k in keys)
    blob_slack = sum(-(-k.get("bytes", 0) // BLOB_CHUNK_BYTES) for k in keys if k.get("type") == "blob")
    floor = used - totals.get("namespaces", 0) - blob_slack
    if listed > used:
        problems.append(f"listed {listed} entries > used {used}")
    if listed < floor:
        problems.append(f"listed {listed} entries < {floor} (used {used} minus namespaces and blob chunks)")
    return problems


# ---- device plumbing ----------------------------------------------------------------


class Watch:
    """Polls ``/api/state`` and keeps every ``tgOffset`` failure number it saw (the
    ring holds 8 failures of any key, so it is read often)."""

    def __init__(self, net):
        self.net = net
        self.seen: set = set()

    def state(self) -> dict:
        st = self.net.get_json("/api/state", timeout=8.0)
        self.seen.update(offset_fail_numbers(st))
        return st

    def sample_for(self, seconds: float, every: float = 1.0) -> dict:
        end = time.time() + seconds
        st = self.state()
        while time.time() < end:
            time.sleep(every)
            st = self.state()
        return st


def pending_updates(bot_token: str) -> Optional[int]:
    url = f"https://api.telegram.org/bot{bot_token}/getWebhookInfo"
    try:
        with urllib.request.urlopen(url, timeout=10) as r:
            return json.loads(r.read()).get("result", {}).get("pending_update_count")
    except Exception:  # noqa: BLE001 - a network blip is a None reading, never a pass
        return None


def episodic(net, limit: int = 40) -> List[dict]:
    return net.get_json(f"/api/mem/episodic?limit={limit}", timeout=8.0).get("messages", [])


def wait_until(pred, timeout: float, every: float = 2.0) -> bool:
    end = time.time() + timeout
    while time.time() < end:
        if pred():
            return True
        time.sleep(every)
    return False


def restart_and_wait(net, timeout: float = 60.0) -> None:
    r = net.post("/api/restart", {}, timeout=8.0)
    assert r.status_code == 202, f"restart refused: HTTP {r.status_code}"
    time.sleep(5)

    def up_again():
        try:
            return net.get_json("/api/state", timeout=4.0).get("uptimeMs", 10**9) < 120000
        except Exception:  # noqa: BLE001 - still booting
            return False

    assert wait_until(up_again, timeout), "device did not come back after the restart"


def nvs_fill(device, n: int) -> None:
    device.cmd(os.environ.get("NIMBUS_L42_FILL", "NVSFILL {n}").format(n=n), expect="NVSFILL", timeout=20.0)


def nvs_free(device) -> None:
    device.cmd(os.environ.get("NIMBUS_L42_FREE", "NVSFILL 0"), expect="NVSFILL", timeout=20.0)


@pytest.fixture
def bench(device, net, secrets, require_secret):
    """Orchestrator mode on Wi-Fi with the DEDICATED test bot, the test chat as the
    only (owner) chat. The token is cleared afterwards."""
    require_secret(secrets.require_test_telegram)
    require_secret(secrets.require_sta)
    device.ensure_mode(1)
    st = device.status()
    if st.group("wifi") == "1":
        net.ip = st.group("ip")
    else:
        net.provision(secrets.sta_ssid, secrets.sta_pass)
        net.wait_got_ip(timeout=25.0)
    r = net.post("/api/orch", {"tgToken": secrets.tg_test_token, "tgAllow": secrets.tg_test_chat}, timeout=8.0)
    assert r.status_code == 200, f"setting the test bot token failed: HTTP {r.status_code}"
    yield secrets.tg_test_token, secrets.tg_test_chat
    net.post("/api/orch", {"clr_tgToken": "1"}, timeout=8.0)


# ---- LAN legs -------------------------------------------------------------------------


@pytest.mark.net
def test_l42_state_fields(net):
    st = net.get_json("/api/state", timeout=8.0)
    assert isinstance(st.get("tgOffsetSaved"), bool), st.keys()
    tp = st.get("tgPoll") or {}
    for k in ("offsetWrites", "offsetWriteFails", "staleDrops", "backoffMs", "fails"):
        assert isinstance(tp.get(k), int), f"tgPoll.{k} missing: {tp}"
    assert tp["staleDrops"] == 0, "an update below the offset cursor was dropped"
    rf = (st.get("nvs") or {}).get("recentFails")
    assert isinstance(rf, list) and len(rf) <= 8, rf
    for f in rf:
        assert set(f) == {"key", "n", "ageS"}, f


@pytest.mark.net
def test_l42_nvs_stats_names_only(net):
    r = net.get("/api/nvs/stats", auth=False, timeout=8.0)
    assert r.status_code == 401, "the NVS listing must need the sign-in token"
    stats = net.get_json("/api/nvs/stats", timeout=15.0)
    assert stats.get("ok") is True
    assert stats["keys"], "no keys listed"
    assert any(k["ns"] == "solide" for k in stats["keys"]), "the settings namespace is missing"
    problems = entries_consistent(stats)
    assert not problems, problems


# ---- the fill run (human sends the bursts) ----------------------------------------------


@pytest.mark.net
@pytest.mark.manual
def test_l42_fill_burst_restart_erase(device, net, bench, require_manual):
    bot, _chat = bench
    watch = Watch(net)
    stats = net.get_json("/api/nvs/stats", timeout=15.0)
    nvs_fill(device, int(stats["entries"]["free"]))
    stats = net.get_json("/api/nvs/stats", timeout=15.0)
    assert stats["entries"]["free"] == 0, f"fill left {stats['entries']['free']} free entries"
    st0 = watch.state()
    writes0 = st0["tgPoll"]["offsetWrites"]

    # Leg 1: the burst. One reply turn for the texts, /help on its own, pending 0.
    require_manual.confirm(
        "From the test chat, send these four messages quickly, one after another:\n"
        + "\n".join(BURST)
        + "\nThen press y.",
        timeout=180.0,
    )
    assert wait_until(lambda: dispatch_counts(episodic(net), BURST)["repro four"] >= 1, 120), "no turn for the burst"
    watch.sample_for(LONG_POLL_S + 5)
    assert wait_until(lambda: pending_updates(bot) == 0, LONG_POLL_S + 10), "Telegram still holds the burst"
    counts = dispatch_counts(episodic(net), BURST)
    assert counts["repro one"] == 1 and counts["repro two"] == 1 and counts["repro four"] == 1, counts
    require_manual.confirm(
        "On the phone: one reply to 'repro one' and 'repro two', then the /help list (it names the device and "
        "firmware), then a reply to 'repro four', in that order. Press y if so.",
        timeout=120.0,
    )
    st = watch.state()
    pages = st["tgPoll"]["offsetWrites"] - writes0
    assert st["tgOffsetSaved"] is False, "the stored offset reads saved on a full NVS"
    assert len(watch.seen) <= fail_budget(pages, LONG_POLL_S + 5), (watch.seen, pages)

    # Leg 2: idle on a full NVS - at most one retry a minute, and the retry happens.
    before = set(watch.seen)
    watch.sample_for(2 * RETRY_QUIET_S + 10)
    new = watch.seen - before
    assert 1 <= len(new) <= 3, f"idle tgOffset failures in 130 s: {sorted(new)}"

    # Leg 3: restart one cycle after the reply - nothing replays.
    restart_and_wait(net)
    watch.sample_for(LONG_POLL_S + 15)
    assert dispatch_counts(episodic(net), BURST) == counts, "a restart after the next poll replayed the burst"

    # Leg 4: restart right after a drain - at most that page replays.
    require_manual.confirm("From the test chat, send quickly:\n" + "\n".join(BURST2) + "\nThen press y.", timeout=180.0)
    assert wait_until(lambda: dispatch_counts(episodic(net), BURST2)["again one"] >= 1, 120, every=0.5)
    restart_and_wait(net)
    watch.sample_for(LONG_POLL_S + 30)
    again = dispatch_counts(episodic(net), BURST2)
    assert all(1 <= c <= 2 for c in again.values()), again
    assert dispatch_counts(episodic(net, 80), BURST) == counts, "an earlier page replayed"

    # Leg 5: free NVS - the owed write lands within a minute and the flag clears.
    nvs_free(device)
    assert wait_until(lambda: watch.state()["tgOffsetSaved"] is True, RETRY_QUIET_S + LONG_POLL_S), (
        "the offset write did not land after NVS was freed"
    )
    log = net.get("/api/errlog/recent?bytes=6000", timeout=8.0).text
    assert "nvs: write FAILED [tgOffset]" in log, "the failure line does not name the key"
    assert "key=***" not in log


@pytest.mark.net
@pytest.mark.manual
def test_l42_v4513_control_loops(device, net, bench, require_manual):
    if os.environ.get("NIMBUS_L42_CONTROL") != "1":
        pytest.skip("control leg: set NIMBUS_L42_CONTROL=1 and flash v4.5.13")
    stats = net.get_json("/api/state", timeout=8.0)
    assert "tgOffsetSaved" not in stats, "this is not a v4.5.13 image"
    nvs_fill(device, 600)
    require_manual.confirm("From the test chat, send: control one\nThen press y.", timeout=180.0)
    time.sleep(60)
    n = dispatch_counts(episodic(net, 80), ["control one"])["control one"]
    assert n >= 3, f"v4.5.13 did not loop on a full NVS (dispatched {n} times)"


def injected_page(chat_id: str, texts: Iterable[str], first_id: int = 900000001) -> str:
    """A raw getUpdates body carrying ``texts`` from ``chat_id``, one update each."""
    result = []
    for i, t in enumerate(texts):
        uid = first_id + i
        result.append(
            {
                "update_id": uid,
                "message": {
                    "message_id": uid,
                    "date": 1760000000 + i,
                    "chat": {"id": int(chat_id), "type": "private"},
                    "from": {"id": int(chat_id), "is_bot": False, "first_name": "Bench"},
                    "text": t,
                },
            }
        )
    return json.dumps({"ok": True, "result": result}, separators=(",", ":"))


@pytest.mark.hil
@pytest.mark.net
def test_l42_injected_burst_splits_commands(device, net, bench):
    _bot, chat = bench
    watch = Watch(net)
    before = watch.state()["tgPoll"]
    texts = ("inject one", "inject two", "/help", "inject four")
    line = os.environ.get("NIMBUS_L42_INJECT", "TGINJECT {json}").format(json=injected_page(chat, texts))
    device.cmd(line, expect="TGINJECT", timeout=10.0)
    device.expect("telegram: injected page: 4 messages batched into 3 turns", timeout=120.0)
    assert wait_until(lambda: dispatch_counts(episodic(net), texts)["inject four"] >= 1, 120)
    counts = dispatch_counts(episodic(net), texts)
    assert counts["inject one"] == 1 and counts["inject two"] == 1 and counts["inject four"] == 1, counts
    rows = [r for r in episodic(net) if r.get("role") == "user" and "inject" in (r.get("text") or "")]
    assert len(rows) == 2, f"expected the batch turn and the 'four' turn, got {len(rows)}"
    after = watch.state()["tgPoll"]
    assert after["staleDrops"] == before["staleDrops"], "an injected id reached the replay guard"


# ---- host tests for the helpers -----------------------------------------------------------


@pytest.mark.host
def test_host_offset_fail_numbers():
    st = {"nvs": {"recentFails": [{"key": "tgOffset", "n": 9, "ageS": 1}, {"key": "usgLedger", "n": 8, "ageS": 2}]}}
    assert offset_fail_numbers(st) == [9]
    assert offset_fail_numbers({}) == []


@pytest.mark.host
def test_host_fail_budget():
    assert fail_budget(2, 35) == 3
    assert fail_budget(0, 130) == 3


@pytest.mark.host
def test_host_dispatch_counts():
    rows = [
        {"role": "user", "text": "Message 1 of 2 from u:\nrepro one\n\nMessage 2 of 2 from u:\nrepro two"},
        {"role": "user", "text": "/help"},
        {"role": "assistant", "text": "repro one"},
        {"role": "user", "text": "repro four"},
    ]
    assert dispatch_counts(rows, BURST) == {"repro one": 1, "repro two": 1, "/help": 1, "repro four": 1}


@pytest.mark.host
def test_host_injected_page_shape():
    body = json.loads(injected_page("12345", ["a", "/help"]))
    assert body["ok"] is True
    assert [u["message"]["text"] for u in body["result"]] == ["a", "/help"]
    assert body["result"][1]["update_id"] == body["result"][0]["update_id"] + 1
    assert body["result"][0]["message"]["chat"]["id"] == 12345


@pytest.mark.host
def test_host_entries_consistent():
    good = {
        "count": 2,
        "entries": {"used": 9, "namespaces": 2},
        "keys": [
            {"ns": "solide", "key": "tgOffset", "type": "int", "bytes": 0, "entries": 1},
            {"ns": "phy", "key": "cal_data", "type": "blob", "bytes": 5000, "entries": 5},
        ],
    }
    assert entries_consistent(good) == []  # 6 listed >= 9 - 2 namespaces - 2 blob chunks
    leaky = dict(good, keys=[dict(good["keys"][0], value="123"), good["keys"][1]])
    assert entries_consistent(leaky)
    assert entries_consistent(dict(good, entries={"used": 5, "namespaces": 2}))  # over
    assert entries_consistent(dict(good, entries={"used": 20, "namespaces": 2}))  # under
