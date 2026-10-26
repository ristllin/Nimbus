"""L43 (web-only notice) - a device with no Telegram owner sees the storage notice.

The settings-storage notice ("settings storage is almost full, ...") is raised by a
failed settings write and sent by the Telegram poll task to the owner chat. A device
with no Telegram allowlist (a web-only setup, the usual first one) had no owner chat,
so the notice reached nobody. It now lands in the web chat as an assistant row tagged
``ev:storage`` (``nimbus/net/owner_notice.h``, host-tested in ``test/test_owner_notice``
and ``test/test_tg_hooks``), which the chat page renders.

Leg (unit A or L, TEST build: ``NVSFILL``; no bot token on the unit, no spend):

* ``test_l43_web_only_storage_notice``: the allowlist cleared (web-only), a fresh boot
  (the notice is once per boot), the store filled to the reserve plus one, a growing
  save refused (507); then the web chat holds the notice as an assistant row of chat
  ``web`` tagged ``ev:storage`` and the device logs it as captured for the web chat. The
  store is freed and the allowlist put back in a finally.

Run: ``python3 -m pytest tests/hil/test_l43_web_only_notice.py --allow-hardware -s``

The pure helper is host-tested at the bottom (``-m host``).
"""

from __future__ import annotations

import time
from typing import List, Optional

import pytest

import test_l43_nvs_reserve as l43
from device import ExpectTimeout

NOTICE_TEXT = "settings storage is almost full"
CAPTURED_LOG = "storage-full notice captured for the web chat (no Telegram owner)"


# ---- pure helpers (host-tested) ---------------------------------------------------


def web_notice(rows: List[dict]) -> Optional[dict]:
    """The newest web chat row that carries the storage notice, or None."""
    for r in rows:  # newest first, as /api/mem/episodic answers
        tags = [t.strip() for t in (r.get("tags") or "").split(",")]
        if (
            r.get("session") == "web"
            and r.get("role") == "assistant"
            and "ev:storage" in tags
            and NOTICE_TEXT in (r.get("text") or "")
        ):
            return r
    return None


# ---- device helpers -----------------------------------------------------------------


def _wait(pred, timeout: float, every: float = 2.0):
    end = time.time() + timeout
    while time.time() < end:
        got = pred()
        if got:
            return got
        time.sleep(every)
    return pred()


def _settled(net, uptime_ms: int = 30000, timeout: float = 120.0) -> None:
    def up():
        try:
            return net.get_json("/api/state", timeout=8.0).get("uptimeMs", 0) >= uptime_ms
        except Exception:  # noqa: BLE001 - still booting
            return False

    assert _wait(up, timeout, every=3.0), f"the device did not come back ({uptime_ms} ms up) within {timeout:.0f} s"


@pytest.mark.hil
@pytest.mark.net
def test_l43_web_only_storage_notice(device, net):
    device.ensure_mode(1)
    orch = net.get_json("/api/orch", timeout=8.0)
    if orch.get("tgLive"):
        pytest.skip("a Telegram bot token is on the unit: this leg needs a web-only unit (clear it first)")
    allow = str(orch.get("tgAllow") or "")
    r = net.post("/api/orch", {"tgAllow": ""}, timeout=8.0)
    assert r.status_code == 200, f"clearing the allowlist -> HTTP {r.status_code}"
    lines: List[str] = []
    try:
        l43.nvs_free(device)
        device.reboot_and_confirm()  # the notice is once per boot: start a fresh one
        _settled(net)
        assert net.get_json("/api/orch", timeout=8.0).get("tgAllow", "x") == "", "the unit still has an allowlist"
        before = web_notice(net.get_json("/api/mem/episodic?limit=40", timeout=8.0).get("messages", []))
        st = l43.fill_to_reserve_plus_one(device, net)
        print(f"[web-only] filled: {l43.free_entries(st)} entries free")
        r = net.post("/api/orch", {"sysPrompt": l43.GROWING_DIRECTIVE}, timeout=8.0)
        assert l43.is_storage_full(r), f"a growing save at reserve+1: HTTP {r.status_code} {r.text[:200]}"
        end = time.time() + 30.0
        while time.time() < end and not any(CAPTURED_LOG in ln for ln in lines):
            try:
                lines.append(device.expect_re(r".", timeout=end - time.time()).string)
            except ExpectTimeout:
                break

        def landed():
            row = web_notice(net.get_json("/api/mem/episodic?limit=40", timeout=8.0).get("messages", []))
            return row if row and (not before or row.get("id") != before.get("id")) else None

        row = _wait(landed, 60.0)
        for ln in lines:
            if "storage-full notice" in ln or "nvs: write FAILED" in ln:
                print(f"[web-only] log: {ln[:200]}")
        assert row, "the storage notice did not reach the web chat"
        print(f"[web-only] web chat row {row.get('id')} tags={row.get('tags')}: {row.get('text')}")
        assert any(CAPTURED_LOG in ln for ln in lines), "the device did not log the web chat capture"
    finally:
        try:
            l43.nvs_free(device)
        finally:
            net.post("/api/orch", {"tgAllow": allow}, timeout=8.0)


# ---- host tests ---------------------------------------------------------------------


@pytest.mark.host
def test_web_notice_picks_the_tagged_web_row():
    rows = [
        {"id": "m9", "session": "web", "role": "assistant", "tags": "ev:spend", "text": "spend"},
        {"id": "m8", "session": "web", "role": "assistant", "tags": "ev:storage", "text": "Nimbus: " + NOTICE_TEXT},
        {"id": "m6", "session": "web", "role": "assistant", "tags": "ev:storagex", "text": NOTICE_TEXT},
        {"id": "m7", "session": "123", "role": "assistant", "tags": "", "text": "Nimbus: " + NOTICE_TEXT},
    ]
    assert web_notice(rows)["id"] == "m8"
    assert web_notice(rows[:1] + rows[2:]) is None
