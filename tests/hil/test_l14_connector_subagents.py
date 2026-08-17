"""§L14 - Connector work via SUB-AGENTS, on all three providers (the offload path).

Proves the concept the streaming/offload work is built for: a spawned sub-agent
runs a provider connector on the lab's compute and returns a short result, so the
device never ingests a fat connector response. One deterministic dispatch per
provider via the /mcp session.spawn tool (connectors for that provider attach
automatically), then we confirm the job was accepted and ran to quiescence.

This is the LIVE counterpart to the host wire tests that already prove -
deterministically - that each provider attaches connectors on sub-agent dispatch
(test_harness_wire_{openai,anthropic,mistral} + test_harness_jobs routing). Those
host tests are the primary proof; this exercises the real device end to end.

Marker `connectors` (+ agent + net); gated. SKIPs when a provider's connector or
key is absent. Reuse an already-joined board with NIMBUS_TEST_IP + NIMBUS_TEST_TOKEN.
"""

from __future__ import annotations

import pytest

from connectors import find_connector, head_spawn_and_wait
from test_l4_network import lan_ip_or_skip

pytestmark = [pytest.mark.connectors, pytest.mark.agent, pytest.mark.net]


def _keyed(net, ip, provider: str) -> bool:
    return bool(net.get_json("/api/connectors", ip=ip, timeout=8.0).get("keyed", {}).get(provider))


def _head_anthropic(net, ip):
    """Put the head on a working provider (anthropic, loop on) so it can spawn."""
    net.post("/api/orch", {"orchHost": "anthropic", "orchLoop": "1", "t": net.token()}, ip=ip, timeout=8.0)


# ---- Mistral: Studio connector on a spawned sub-agent (Conversations offload) --
def test_mistral_connector_subagent(device, net, secrets, require_secret):
    ip = lan_ip_or_skip(device, net, secrets, require_secret)
    if not _keyed(net, ip, "mistral"):
        pytest.skip("no Mistral key on the device")
    if not find_connector(net, ip, prov="mistral", cid="github_app"):
        pytest.skip("Mistral GitHub connector (github_app) not configured on the device")
    _head_anthropic(net, ip)
    ok = head_spawn_and_wait(
        net,
        ip,
        "mistral",
        "using the GitHub connector, report how many open issues the public repo microsoft/vscode has.",
    )
    assert ok, "mistral sub-agent did not dispatch+complete (err=0 state=3)"


# ---- OpenAI: first-party / MCP connector on a spawned sub-agent -------------
def test_openai_connector_subagent(device, net, secrets, require_secret):
    ip = lan_ip_or_skip(device, net, secrets, require_secret)
    if not _keyed(net, ip, "openai"):
        pytest.skip("no OpenAI key on the device")
    if not find_connector(net, ip, prov="openai", cid="github"):
        pytest.skip("OpenAI GitHub (MCP) connector not configured on the device")
    _head_anthropic(net, ip)
    ok = head_spawn_and_wait(
        net, ip, "openai", "using the GitHub tool, report the open-issue count of the public repo microsoft/vscode."
    )
    assert ok, "openai sub-agent did not dispatch+complete (err=0 state=3)"


# ---- Anthropic: managed agent (mcp_servers/attachAnthropic path) ------------
def test_anthropic_connector_subagent(device, net, secrets, require_secret):
    ip = lan_ip_or_skip(device, net, secrets, require_secret)
    if not _keyed(net, ip, "anthropic"):
        pytest.skip("no Anthropic key on the device")
    _head_anthropic(net, ip)
    # The managed agent carries web tools; an enabled anthropic MCP connector (if
    # any) also rides via mcp_servers. Either way the sub-agent must run to Done.
    ok = head_spawn_and_wait(
        net, ip, "anthropic", "use your web tools to look up one current fact and report it in one sentence."
    )
    assert ok, "anthropic sub-agent did not dispatch+complete (err=0 state=3)"
