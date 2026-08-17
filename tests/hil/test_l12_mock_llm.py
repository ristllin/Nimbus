"""§L12 - mock-LLM on-device e2e (deterministic LAN backend, no provider bill).

A stdlib mock OpenAI-compatible server (``mock_llm.py``) is provisioned as the
device's CUSTOM endpoint (``custBase=http://<host>:<port>/v1``, ``custConv=
openai``, keyless) and sub-sessions are routed to it (``subPrio=custom``). Every
scenario is selected by MODEL NAME (``custModel``, read live per dispatch), and
every wire-level assertion runs against the mock's recorded requests - fully
deterministic on the mock side.

FIRMWARE REALITY this module is honest about (verified in source, quoted in the
docstrings; ``custom_adapter.cpp`` is a READ-ONLY contract):

  * The HEAD can now run on the custom endpoint too (Stage H of the harness
    extraction): ``orchestrator.cpp`` registers a "custom" ProviderTurnFn when
    ``store::hasCustom()`` at boot - a single-shot chat-completions structured
    turn (response_format json_schema, NO tool loop v1). ``orchHost=custom``
    routes the head to the mock (see ``test_head_custom_turn``). The sub-session
    tests still trigger spawns through ONE real head provider key (marked
    ``agent``) so their scenario under test stays the sub-session wire.
  * The custom sub-session wire is a single-shot ``POST /v1/chat/completions``
    (HTTP/1.0, keyless over http://): NO tools, NO structured output, NO conv
    id, NO usage parsing, NO HTTP-error retry, fixed 30 s timeout
    (``CUST_TIMEOUT_MS``). Tests assert THAT contract - not the richer one a
    future head-custom path would have.
  * ``adapter_factory.cpp`` registers backend "custom" only when
    ``store::hasCustom()`` at boot -> the fixture reboots once after
    provisioning and once after restore.

Run: python3 -m pytest tests/hil/test_l12_mock_llm.py -m "agent and net" --allow-hardware
Env: NIMBUS_TEST_IP (reuse a joined device), NIMBUS_TEST_STA_SSID/_PASS (join),
     NIMBUS_MOCK_SLOW_S (mock slow-scenario delay, default 35 > the device's 30).
"""

from __future__ import annotations

import os
import time

import pytest

# Importing the fixture into this module's namespace registers it for these
# tests without touching conftest.py (harness convention: sibling top-level
# imports; conftest puts tests/hil on sys.path).
from mock_llm import (  # noqa: F401  (mock_llm is a fixture)
    BADJSON_BODY,
    DEVICE_CUST_TIMEOUT_S,
    MOCK_REPLY,
    MockLLMServer,
    USAGE_COMPLETION_TOKENS,
    USAGE_PROMPT_TOKENS,
    find_lan_ip,
    mock_llm,
)
from net import WifiAuthFailure
from secrets import SecretsUnavailable

pytestmark = [pytest.mark.net, pytest.mark.agent]

# Head turns run a real LLM (trigger only) + the spawn queue dispatches on a
# 2.5 s cadence; the mock recording is the sync point.
SPAWN_WAIT_S = 150.0
# Preference order for the trigger head: AGENTS.md documents the OpenAI
# tool-loop chain-poisoning bug - prefer Anthropic when keyed.
HEAD_PREFERENCE = ("anthropic", "openai", "mistral")
HEAD_KEY_FIELD = {"anthropic": "antKey", "openai": "oaiKey", "mistral": "mistKey"}


# ---- low-level helpers (token-gated HTTP; owner-batch-2 gates ALL /api GETs) --
def _oget(net, ip, tok, path):
    sep = "&" if "?" in path else "?"
    return net.get_json(f"{path}{sep}t={tok}", ip=ip, timeout=8.0)


def _opost(net, ip, tok, fields):
    data = dict(fields)
    data["t"] = tok
    return net.post("/api/orch", data, ip=ip, timeout=8.0)


def _webtok(device) -> str:
    """WEBTOK? over serial (l9 pattern): the read retries because the shared
    diagnostics channel can tear lines under burst."""
    last = ""
    for _ in range(6):
        try:
            return device.cmd_re("WEBTOK?", r"([0-9a-fA-F]{24})", timeout=4.0).group(1)
        except Exception as exc:  # noqa: BLE001 - serial flake; retry
            last = str(exc)
    pytest.skip(f"could not read WEBTOK over serial after retries ({last})")


def _lan_ip(device, net, secrets, timeout: float = 30.0) -> str:
    """Resolve the device's LAN IP. NIMBUS_TEST_IP fast path first (any JSON
    answer proves reachability - even a 401 body); else provision + join."""
    override = os.environ.get("NIMBUS_TEST_IP")
    if override:
        deadline = time.time() + timeout
        while time.time() < deadline:
            try:
                net.get_json("/api/state", ip=override, timeout=4.0)
                return override
            except Exception:  # noqa: BLE001 - still rejoining
                time.sleep(2.0)
    try:
        secrets.require_sta()
    except SecretsUnavailable as exc:
        pytest.skip(str(exc))
    device.reset()
    device.wait_ready(timeout=20.0)
    net.provision(secrets.sta_ssid, secrets.sta_pass)
    try:
        return net.wait_got_ip(timeout=25.0)
    except WifiAuthFailure as exc:
        pytest.skip(f"could not join LAN for mock-llm tests (reason={exc.reason})")


def _wait_http(net, ip, timeout: float = 45.0) -> None:
    deadline = time.time() + timeout
    last = "no answer"
    while time.time() < deadline:
        try:
            net.get_json("/api/state", ip=ip, timeout=4.0)
            return
        except Exception as exc:  # noqa: BLE001
            last = str(exc)
            time.sleep(2.0)
    pytest.fail(f"device HTTP did not come back after reboot ({last})")


class Backend:
    """Handle the tests drive: mock server + provisioned device + helpers."""

    def __init__(self, device, net, server: MockLLMServer, ip: str, tok: str, host_ip: str):
        self.device = device
        self.net = net
        self.server = server
        self.ip = ip
        self.tok = tok
        self.host_ip = host_ip

    def orch(self) -> dict:
        return _oget(self.net, self.ip, self.tok, "/api/orch")

    def state(self) -> dict:
        return _oget(self.net, self.ip, self.tok, "/api/state")

    def set_model(self, model: str) -> None:
        """Switch scenario live: the adapter reads store::customModel() at
        EVERY dispatch (custom_adapter.cpp: ``String model =
        store::customModel();``) - no reboot needed."""
        r = _opost(self.net, self.ip, self.tok, {"custModel": model})
        assert r.status_code == 200, f"custModel={model} -> {r.status_code}"
        self.server.reset()

    def trigger_spawn(self, task: str) -> None:
        """Fire ONE head turn (real provider - the only spawn trigger the
        firmware has) that delegates ``task`` to a sub-agent. subPrio=custom
        routes the spawn to the mock even if the model omits the provider."""
        prompt = (
            "Delegate this to a sub-agent RIGHT NOW: spawn exactly one "
            f"sub-agent on provider custom with the task: {task}. "
            "Do not ask for confirmation and do not answer the task "
            "yourself - spawn it."
        )
        try:
            self.device.cmd(f"TURN {prompt}", expect="TURN <-", timeout=6.0)
        except Exception:  # noqa: BLE001 - the echo is advisory; turn still fires
            pass

    def assert_healthy(self) -> None:
        """The resilience bar every scenario ends on: the loop answers PING and
        the web surface still serves Orchestrator state."""
        assert self.device.ping(timeout=6.0), "device stopped answering PING"
        st = self.state()
        assert st.get("mode") == 1, f"/api/state mode={st.get('mode')} (want 1)"

    def assert_request_shape(self, rec) -> None:
        """The custom-adapter wire contract, byte-for-byte the custRequest()/
        dispatch() build (custom_adapter.cpp)."""
        assert rec.method == "POST"
        assert rec.path == "/v1/chat/completions", rec.path
        # Keyless-over-http contract: "NEVER send a key over plain HTTP".
        assert "authorization" not in rec.headers, "auth header on the keyless http:// wire"
        assert "x-api-key" not in rec.headers
        assert rec.headers.get("content-type", "").startswith("application/json")
        msgs = rec.messages
        assert len(msgs) == 2, f"want [system, user], got {msgs}"
        assert msgs[0]["role"] == "system" and "autonomous" in msgs[0]["content"]
        assert msgs[1]["role"] == "user" and msgs[1]["content"]
        # Single-shot wire: no tools / structured output / conv id ride-alongs.
        for absent in ("tools", "tool_choice", "response_format", "conversation"):
            assert absent not in (rec.json or {}), f"unexpected {absent!r} on sub-session wire"


# ---- fixture: provision the mock as the custom backend -----------------------
@pytest.fixture(scope="module")
def custom_backend(device, net, secrets, mock_llm):  # noqa: F811 - fixture name
    """Provision the mock as the device's custom endpoint + route sub-sessions
    to it; ensure a head key exists (trigger only); RESTORE EVERYTHING in
    teardown (the repo's break-state-must-restore rule) including the reboot
    that de-registers the custom adapter."""
    device.ensure_mode(1)
    ip = _lan_ip(device, net, secrets)
    tok = _webtok(device)

    host_ip = find_lan_ip(ip)
    if not host_ip:
        pytest.skip("could not determine a host LAN IP the device can reach (no route toward the device)")

    prior = _oget(net, ip, tok, "/api/orch")
    if "cust" not in prior:
        pytest.skip(
            f"/api/orch has no cust block (keys={sorted(prior)}) - firmware too old for the custom-endpoint surface"
        )
    prior_cust = dict(prior.get("cust") or {})
    prior_sub = prior.get("subPrio", "")
    prior_host = prior.get("orchHost", "")

    # Head trigger: prefer a key already on the device; else push one from
    # secrets (cleared again in teardown).
    provs = prior.get("providers", {})
    head = next((p for p in HEAD_PREFERENCE if provs.get(p, {}).get("hasKey")), "")
    pushed_key = ""
    if not head:
        for p in HEAD_PREFERENCE:
            key = getattr(secrets, f"{p}_key", "") if hasattr(secrets, f"{p}_key") else ""
            if key:
                r = _opost(net, ip, tok, {HEAD_KEY_FIELD[p]: key})
                assert r.status_code == 200, f"push {p} key -> {r.status_code}"
                head, pushed_key = p, HEAD_KEY_FIELD[p]
                break
    if not head:
        pytest.skip(
            "no head provider key on device or in secrets - the head "
            "turn is the only spawn trigger the firmware has "
            "(runTurnHost: openai/anthropic/mistral only)"
        )

    base = mock_llm.base_url(host_ip)
    r = _opost(
        net,
        ip,
        tok,
        {
            "custBase": base,
            "custConv": "openai",
            "custModel": "mock-echo",
            "subPrio": "custom",
            "orchHost": head,
            "convReset": "1",
        },
    )
    assert r.status_code == 200, f"provision POST /api/orch -> {r.status_code}"

    # Reboot: backend "custom" registers only when hasCustom() at fabricInit.
    device.reset()
    device.wait_ready(timeout=25.0)
    _wait_http(net, ip)

    after = _oget(net, ip, tok, "/api/orch")
    assert after.get("cust", {}).get("base") == base, f"custBase did not stick: {after.get('cust')}"

    be = Backend(device, net, mock_llm, ip, tok, host_ip)
    try:
        yield be
    finally:
        # RESTORE: prior custom endpoint + routing + head, drop any pushed key,
        # then reboot so the custom adapter de-registers again.
        fields = {
            "custBase": prior_cust.get("base", ""),
            "custConv": prior_cust.get("conv", "openai"),
            "custModel": prior_cust.get("model", ""),
            "orchHost": prior_host,
            "convReset": "1",
        }
        if prior_sub:
            fields["subPrio"] = prior_sub
        if pushed_key:
            fields[f"clr_{pushed_key}"] = "1"
        r = _opost(net, ip, tok, fields)
        assert r.status_code == 200, f"RESTORE POST /api/orch -> {r.status_code}"
        device.reset()
        device.wait_ready(timeout=25.0)
        _wait_http(net, ip)
        restored = _oget(net, ip, tok, "/api/orch").get("cust", {})
        assert restored.get("base", "") == prior_cust.get("base", ""), f"custBase not restored: {restored}"


# ---- tests -------------------------------------------------------------------
def test_turn_roundtrip_deterministic(custom_backend):
    """Head turn -> spawn -> the device POSTs the EXACT custom-adapter wire to
    the mock; the mock's fixed reply flows back through the adapter (Ok ->
    journal -> synthesis). Hard assertions: the recorded request shape + device
    health; the delivered phrasing rides a synthesis turn (real LLM) so the
    reply text is checked lenient."""
    be = custom_backend
    be.set_model("mock-echo")
    be.trigger_spawn(f'reply with exactly "{MOCK_REPLY}"')

    got = be.server.wait_for_request("mock-echo", count=1, timeout=SPAWN_WAIT_S)
    assert got, "the device never POSTed to the mock - head turn did not spawn, or the LAN path is broken"
    be.assert_request_shape(got[0])
    assert got[0].json["model"] == "mock-echo"

    # Best-effort: the sub-agent result surfaces via a synthesis turn (or the
    # 60 s raw fallback) as an ORCH REPLY line; a paraphrase is acceptable, so
    # only log-miss - the deterministic wire assertions above are the test.
    try:
        be.device.expect("ORCH REPLY", timeout=120.0)
    except Exception:  # noqa: BLE001 - synthesis phrasing/timing is the LLM's
        pass
    be.assert_healthy()


def test_head_custom_turn(custom_backend):
    """Stage H FLIP of the old 'no head custom path' contract pin: the head
    now runs on the custom endpoint (providers::orchTurnCustom, registered in
    orchestrator.cpp when hasCustom() at boot - which the fixture's provision
    reboot satisfied). Route the HEAD to the mock live (orchHost=custom), fire
    a serial TURN, and assert the head-custom wire v1: EXACTLY ONE keyless
    POST /v1/chat/completions carrying response_format json_schema (orch_turn)
    + [system, user] messages and NO tools (single-shot - the mock's toolloop
    scenario would emit a tool_calls round IF tools were advertised, so one
    request IS the no-tool-loop proof). RESTORES orchHost in-test."""
    be = custom_backend
    be.set_model("mock-toolloop")
    prior_host = be.orch().get("orchHost", "")
    r = _opost(be.net, be.ip, be.tok, {"orchHost": "custom", "convReset": "1"})
    assert r.status_code == 200, f"orchHost=custom -> {r.status_code}"
    try:
        try:
            be.device.cmd("TURN say hello", expect="TURN <-", timeout=6.0)
        except Exception:  # noqa: BLE001 - the echo is advisory; turn still fires
            pass

        got = be.server.wait_for_request("mock-toolloop", count=1, timeout=90.0)
        assert got, (
            "the device never POSTed the HEAD turn to the mock - "
            "orchHost=custom did not route (head-custom path broken?)"
        )
        rec = got[0]
        assert rec.method == "POST" and rec.path == "/v1/chat/completions", rec.path
        # Keyless-over-http holds on the head wire too.
        assert "authorization" not in rec.headers
        assert "x-api-key" not in rec.headers
        j = rec.json or {}
        rf = j.get("response_format") or {}
        assert rf.get("type") == "json_schema", f"no json_schema response_format: {rf}"
        assert rf.get("json_schema", {}).get("name") == "orch_turn"
        msgs = j.get("messages", [])
        assert len(msgs) == 2, f"want [system, user], got {len(msgs)}"
        assert msgs[0]["role"] == "system" and msgs[1]["role"] == "user"
        assert "tools" not in j, "head-custom v1 is single-shot - no tool advertisement"

        # Settle: no second round (the loop never engages on head-custom v1).
        time.sleep(8.0)
        n = len(be.server.requests_for("mock-toolloop"))
        assert n == 1, f"expected the single-shot head wire (1 request), saw {n}"

        # The mock's structured reply parsed + applied end-to-end (advisory:
        # the serial echo can tear under burst).
        try:
            be.device.expect("ORCH REPLY", timeout=30.0)
        except Exception:  # noqa: BLE001
            pass
        be.assert_healthy()
    finally:
        r = _opost(be.net, be.ip, be.tok, {"orchHost": prior_host, "convReset": "1"})
        assert r.status_code == 200, f"RESTORE orchHost -> {r.status_code}"


def test_salvage_on_bad_json(custom_backend):
    """200 + truncated JSON -> the adapter's stream parse yields empty content
    -> FabricErr::ParseFail -> the owner gets the clean 'Couldn't start that
    agent on custom.' message, NEVER the raw JSON - and the device stays up."""
    be = custom_backend
    be.set_model("mock-badjson")
    be.trigger_spawn("say anything")

    got = be.server.wait_for_request("mock-badjson", count=1, timeout=SPAWN_WAIT_S)
    assert got, "the device never POSTed to the mock"

    # dispatchSpawn: FabricErr != Ok -> deliver("Couldn't start that agent on
    # <provider>.") to the serial chat.
    line = be.device.expect("Couldn't start that agent", timeout=90.0)
    assert "custom" in line

    # The truncated body must never surface to the owner.
    marker = BADJSON_BODY.decode()[:20]
    leaked = [l for l in be.device._transcript if "ORCH REPLY" in l and marker in l]
    assert not leaked, f"raw mock JSON leaked to the chat: {leaked}"
    be.assert_healthy()


def test_retry_on_500(custom_backend):
    """CONTRACT PIN: the custom adapter does NOT retry HTTP errors (only the
    TCP connect is retried 3x; ``code != 200 -> RemoteFail`` immediately). So a
    transient 500 fails THAT dispatch with the clean owner message and exactly
    ONE request - and the NEXT dispatch (the 'then-ok' leg) succeeds, proving
    the failure left no poisoned state behind."""
    be = custom_backend
    be.set_model("mock-500-then-ok")
    be.trigger_spawn("say ok")

    got = be.server.wait_for_request("mock-500-then-ok", count=1, timeout=SPAWN_WAIT_S)
    assert got, "the device never POSTed to the mock"
    be.device.expect("Couldn't start that agent", timeout=90.0)
    time.sleep(5.0)
    assert len(be.server.requests_for("mock-500-then-ok")) == 1, (
        "adapter unexpectedly retried the 500 (contract: no HTTP retry)"
    )

    # Second head turn -> second dispatch -> the mock now answers 200.
    be.trigger_spawn("say ok again")
    got = be.server.wait_for_request("mock-500-then-ok", count=2, timeout=SPAWN_WAIT_S)
    assert len(got) >= 2, "no second dispatch after the transient 500"
    be.assert_request_shape(got[1])
    be.assert_healthy()


def test_timeout_handled(custom_backend):
    """The mock sleeps past CUST_TIMEOUT_MS (30 s, compile-time - the MOCK's
    delay is the configurable side: NIMBUS_MOCK_SLOW_S) -> the read deadline
    lapses -> code 0 -> FabricErr::Network -> clean owner message. The 30 s
    block runs INLINE on the tg_poll task, so the main loop + web server must
    stay responsive throughout."""
    be = custom_backend
    assert be.server.slow_delay_s > DEVICE_CUST_TIMEOUT_S, (
        f"mock slow delay {be.server.slow_delay_s}s must exceed the device's "
        f"{DEVICE_CUST_TIMEOUT_S}s timeout (set NIMBUS_MOCK_SLOW_S)"
    )
    be.set_model("mock-slow")
    be.trigger_spawn("say something slowly")

    got = be.server.wait_for_request("mock-slow", count=1, timeout=SPAWN_WAIT_S)
    assert got, "the device never POSTed to the mock"

    # While the dispatch blocks its 30 s, the AsyncTCP web surface still serves.
    st = be.state()
    assert st.get("mode") == 1, "web surface died during the blocking dispatch"

    be.device.expect("Couldn't start that agent", timeout=DEVICE_CUST_TIMEOUT_S + 90.0)
    be.assert_healthy()


def test_usage_recorded(custom_backend):
    """CONTRACT PIN (token attribution): the custom wire parses NO ``usage``
    (the ArduinoJson filter admits only choices[0].message.content), and
    AGENTS.md documents that sub-agent spend is not yet attributed back. So
    the mock's usage {prompt:1234, completion:56} must be visible in the
    mock's own response (scenario sanity) while /api/orch usage.lastIn/lastOut
    keep tracking the HEAD turn - they must NOT become the mock's numbers.
    usage.turns must still count the head trigger turn."""
    be = custom_backend
    turns_before = be.orch().get("usage", {}).get("turns", 0)
    be.set_model("mock-usage")
    be.trigger_spawn("count some tokens")

    got = be.server.wait_for_request("mock-usage", count=1, timeout=SPAWN_WAIT_S)
    assert got, "the device never POSTed to the mock"
    be.assert_request_shape(got[0])

    # Let the head turn book its usage; then read the ledger.
    deadline = time.time() + 90.0
    usage = {}
    while time.time() < deadline:
        usage = be.orch().get("usage", {})
        if usage.get("turns", 0) > turns_before:
            break
        time.sleep(3.0)
    assert usage.get("turns", 0) > turns_before, f"head turn not counted (turns stayed {turns_before})"
    for key in ("lastIn", "lastOut", "sessIn", "sessOut"):
        assert key in usage, f"/api/orch usage missing {key!r}: {sorted(usage)}"
    assert (usage.get("lastIn"), usage.get("lastOut")) != (USAGE_PROMPT_TOKENS, USAGE_COMPLETION_TOKENS), (
        "lastIn/lastOut took the SUB-SESSION mock numbers - the custom wire "
        "grew usage parsing; update this contract pin (and celebrate: "
        "attribute it to the sub-session, not the head turn)"
    )
    be.assert_healthy()
