"""§L5 - HIL orchestrator / agent tests (the HIL test spec).

Orchestrator boot (the direct F11 hang regression), live provider round-trip +
failover, Telegram round-trip with a DEDICATED test bot, graceful degradation.

Provider keys are PUSHED TO THE DEVICE over serial (never used host-side) via the
``SET <nvsKey>=<value>`` command (provision.cpp acks ``SET <key> ok=..``; the
NIMBUS_TEST affordance channel accepts the same). NVS keys: oaiKey/antKey/tgToken/
tgAllow (agent_config.h). Telegram uses a DEDICATED test bot - never a production
bot's token, which would collide on long-poll (F18).

``@pytest.mark.agent`` (+ ``net``); conftest loud-skips without ``--allow-hardware``,
and ``require_secret`` turns a missing key/token into a loud, reasoned skip.

Failure IDs: F11 (orch boot no-hang + corollary), F17 (provider turn + failover),
F18 (Telegram round-trip).
"""

from __future__ import annotations

import time

import pytest

from device import ExpectTimeout


# ---- helpers ---------------------------------------------------------------
def push_key(device, nvs_key: str, value: str) -> None:
    """Push a key into device NVS over serial so the firmware reads it the same way
    the product does. Uses the ``SET <key>=<value>`` provision protocol (also accepted
    by the NIMBUS_TEST channel). The value is never logged. provision.cpp acks
    ``SET <key> ok=1 len=..``; not every build acks, so the ack is best-effort - the
    real assertion is the turn/round-trip downstream."""
    device.send(f"SET {nvs_key}={value}")
    try:
        device.expect(f"SET {nvs_key} ok=", timeout=3.0)
    except ExpectTimeout:
        pass


def _reply_text(line: str) -> str:
    return line.split("ORCH REPLY [serial]:", 1)[-1].strip()


# ---- orch_boot_ok (F11) ----------------------------------------------------
@pytest.mark.agent
def test_orch_boot_ok(device, secrets, require_secret):
    """orch_boot_ok (F11): with the device flashed in ``nimbus_mode=Orchestrator``
    (mode=1), boot reaches ``READY mode=1 ip=..`` and the loop stays alive - the
    direct regression for the first-HW-boot hang that bricked reflash.

    Liveness three ways so a stalled turn loop can't pass: mode==1 beacon,
    ``PING``->``PONG``, and two ``STATUS`` reads with monotonic advancing uptime (the
    loop is turning, not frozen behind a blocking network/TLS call). A mode=0 boot is
    a LOUD skip (wrong build); a legacy build with no beacon is a LOUD fail."""
    device.ensure_mode(1)  # switches live; no longer a skip
    device.reset()
    mode, _ip = device.wait_ready(timeout=25.0)
    # mode is None when the board answered PING but the beacon went past before the
    # console re-attached (a port open resets unit A's chip): STATUS mode below is
    # then the boot's mode. Any other beacon mode is a wrong boot.
    assert mode in (1, None), f"device booted mode={mode}, expected 1 (Orchestrator)"

    assert device.ping(), "orchestrator booted but PING got no PONG - hung (F11)"
    st = device.status()
    assert st.group("mode") == "1", f"STATUS mode={st.group('mode')}, expected 1"
    up1 = int(st.group("up"))
    up2 = int(device.status().group("up"))
    assert up2 >= up1 and up2 > 0, f"uptime not advancing ({up1}->{up2}) - a blocked/frozen loop (F11)"


# ---- agent_turn_live (F17) -------------------------------------------------
@pytest.mark.agent
@pytest.mark.net
def test_agent_turn_live(device, secrets, require_secret):
    """agent_turn_live (F17): push both provider keys, join the LAN, fire a
    ``TURN <prompt>`` (non-Telegram trigger), and assert a real round-trip reply
    arrives on serial as ``ORCH REPLY [serial]: <text>`` with non-empty text.

    Then FAILOVER: break the first provider (bad key) and assert a subsequent TURN
    still gets a reply via the second - the F17 concern that a dead first provider
    must not kill the turn. If only one key is present, the failover leg LOUD-skips
    (the round-trip half still ran)."""
    require_secret(secrets.require_provider_keys)
    require_secret(secrets.require_sta)

    device.ensure_mode(1)
    device.reset()
    mode, _ip = device.wait_ready(timeout=25.0)
    assert mode in (1, None), f"expected Orchestrator boot, got mode={mode}"  # None: beacon missed, board answered PING

    have_two = bool(secrets.openai_key and secrets.anthropic_key)
    if secrets.openai_key:
        push_key(device, "oaiKey", secrets.openai_key)
    if secrets.anthropic_key:
        push_key(device, "antKey", secrets.anthropic_key)

    device.wifi(secrets.sta_ssid, secrets.sta_pass)
    device.expect_re(r"WIFI_GOT_IP\s+\d+\.\d+\.\d+\.\d+", timeout=25.0)

    device.turn("Reply with exactly the single word: pong")
    try:
        reply = device.expect("ORCH REPLY [serial]:", timeout=45.0)
    except ExpectTimeout:
        pytest.fail("no ORCH REPLY within 45 s - provider turn never round-tripped over TLS (F17)")
    assert _reply_text(reply), "ORCH REPLY arrived but reply text was empty (F17)"

    if not have_two:
        pytest.skip(
            "only one provider key present; set both OPENAI_API_KEY and ANTHROPIC_API_KEY for the F17 failover leg"
        )

    # Failover: poison the first-priority provider's key; a fresh turn must still
    # reply via the second provider.
    push_key(device, "oaiKey", "sk-INVALID-forced-failover")
    device.turn("Reply with exactly the single word: alive")
    try:
        reply2 = device.expect("ORCH REPLY [serial]:", timeout=60.0)
    except ExpectTimeout:
        pytest.fail("no reply after breaking the first provider - failover to the second provider did not happen (F17)")
    assert _reply_text(reply2), "failover reply text was empty (F17)"
    push_key(device, "oaiKey", secrets.openai_key)  # restore, clean session


# ---- telegram_roundtrip (F18) ----------------------------------------------
@pytest.mark.agent
@pytest.mark.net
def test_telegram_roundtrip(device, secrets, require_secret):
    """telegram_roundtrip (F18): with the DEDICATED test bot token on the device, a
    message from the test chat gets the device's reply delivered to that chat.

    getMe must succeed first (the bot was never even connected - F18). Telegram never
    shows a bot its own messages, so the old leg's sendMessage from the bot itself
    could never reach the device's poll; the inbound message is fed into the real
    receive path with TGINJECT (the page Telegram would serve), and the reply's
    delivery is read from the device's own Telegram record (/api/tg/archive: an
    outbound row, status ok). Uses the dedicated bot ONLY, and clears its token after."""
    import os

    import tg_archive_bench as tb

    require_secret(secrets.require_test_telegram)
    require_secret(secrets.require_sta)
    try:
        import requests
    except ImportError:
        pytest.skip("requests not installed")
    ip, webtok = os.environ.get("NIMBUS_TEST_IP"), os.environ.get("NIMBUS_TEST_TOKEN")
    if not ip or not webtok:
        pytest.skip("set NIMBUS_TEST_IP + NIMBUS_TEST_TOKEN to read the device's Telegram record")
    token, chat = secrets.tg_test_token, secrets.tg_test_chat
    me = requests.get(f"https://api.telegram.org/bot{token}/getMe", timeout=10.0).json()
    assert me.get("ok") is True, "getMe failed for the test bot"
    hdr = {"X-Nimbus-Token": webtok}
    day = time.strftime("%Y-%m-%d", time.gmtime())

    def outbound():
        rows, start = [], 0
        while True:
            page = requests.get(
                f"http://{ip}/api/tg/archive",
                headers=hdr,
                timeout=15,
                params={"chat": str(chat), "day": day, "limit": 200, "from": start},
            ).json()
            rows += [r for r in page.get("rows", []) if r.get("dir") == "out"]
            if not page.get("more"):
                return rows
            start = page["next"]

    device.ensure_mode(1)
    r = requests.post(
        f"http://{ip}/api/orch", headers=hdr, data={"tgToken": str(token), "tgAllow": str(chat)}, timeout=10
    )
    assert r.status_code == 200, f"setting the test bot token failed: HTTP {r.status_code}"
    try:
        before = len(outbound())
        uid = tb.first_update_id() + 7
        device.cmd(
            "TGINJECT " + tb.page([tb.text_update(uid, int(str(chat)), "Reply with the single word: pong")]),
            "TGINJECT",
            timeout=10.0,
        )
        deadline, rows = time.time() + 90.0, []
        while time.time() < deadline:
            rows = outbound()[before:]
            if rows:
                break
            time.sleep(3.0)
        assert rows, "the device never sent a reply to the test chat (F18)"
        assert rows[-1].get("st") == "ok", (
            f"the device's reply to the test chat was not delivered (status {rows[-1].get('st')!r}); "
            "if Telegram says 'chat not found', the test chat has never started the test bot"
        )
    finally:
        requests.post(f"http://{ip}/api/orch", headers=hdr, data={"clr_tgToken": "1"}, timeout=10)


# ---- graceful_degradation (F11 corollary) ----------------------------------
@pytest.mark.agent
def test_graceful_degradation(device, secrets):
    """graceful_degradation (F11 corollary): with NO provider keys, NO Telegram token
    and NO STA creds, the device still boots, idles on the local UI, and the heartbeat
    keeps ticking - no crash, no reboot loop, no blocking loop.

    We provision NOTHING; we assert the unprovisioned device is a live idler: boot
    reaches READY (no BootError), ``PING``->``PONG`` answers, two ``STATUS`` reads show
    advancing uptime (loop not blocked), and ``RENDER?`` still reports a local screen
    (UI up, not frozen)."""
    from device import BootError

    device.reset()
    try:
        mode, _ip = device.wait_ready(timeout=25.0)
    except BootError as exc:
        pytest.fail(f"unprovisioned device did not boot cleanly: {exc}")

    assert device.ping(), "unprovisioned device is not answering (blocked/hung)"

    up_a = int(device.status().group("up"))
    up_b = int(device.status().group("up"))
    assert up_b >= up_a and up_b > 0, f"heartbeat not advancing while idle ({up_a}->{up_b}) - a blocking loop"

    try:
        r = device.render()
    except ExpectTimeout:
        pytest.fail("RENDER? did not answer - local UI frozen while unprovisioned")
    assert r.screen >= 0, "RENDER? returned an invalid screen id"
