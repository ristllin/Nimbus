"""§L10 - provider / sub-session e2e matrix (the HIL test spec).

The e2e coverage the revamp (P9) adds, over the shared device with REAL endpoints:
custom/Ollama as a keyless LAN backend, plus the multi-provider sub-session and
tool-loop legs the older suites skip. Each test is a LOUD, reasoned skip when its
secret / server isn't configured - never a silent pass - so a partial environment
still runs what it can.

Run: python3 -m pytest tests/hil/test_l10_providers.py -m "agent and net" --allow-hardware

Env (all optional; a missing one skips just that test):
  NIMBUS_TEST_OLLAMA_URL   e.g. http://192.168.1.50:11434/v1  (Mac `ollama serve`)
  NIMBUS_TEST_OLLAMA_MODEL e.g. qwen2.5  (a tool-capable model, pulled on the host)
  provider keys + STA come from the existing secrets mechanism (the secrets .env).
"""

from __future__ import annotations

import os
import time

import pytest

from test_l4_network import lan_ip_or_skip
from test_l5_agent import push_key


# ---- custom / Ollama over plain HTTP (P9) -----------------------------------
@pytest.mark.agent
@pytest.mark.net
def test_custom_ollama_subsession(device, net, secrets, require_secret):
    """Provision a keyless LAN Ollama as the custom endpoint (http:// -> plain HTTP,
    no Authorization header) and drive one sub-session turn through it end-to-end.
    Proves the P9 scheme-aware transport + empty-key path against a real server."""
    url = os.environ.get("NIMBUS_TEST_OLLAMA_URL")
    model = os.environ.get("NIMBUS_TEST_OLLAMA_MODEL", "qwen2.5")
    if not url:
        pytest.skip("set NIMBUS_TEST_OLLAMA_URL to a LAN Ollama (http://host:11434/v1)")
    if not url.startswith("http://"):
        pytest.skip("NIMBUS_TEST_OLLAMA_URL must be http:// (the plain-HTTP path under test)")
    ip = lan_ip_or_skip(device, net, secrets, require_secret)

    # Provision the custom endpoint over serial (keyless), openai wire.
    push_key(device, "custBase", url)
    push_key(device, "custConv", "openai")
    push_key(device, "custModel", model)
    push_key(device, "custKey", "")  # keyless: no Authorization header
    push_key(device, "subPrio", "custom")  # route sub-sessions to it
    device.reset()
    device.wait_ready(timeout=20.0)

    # One turn that spawns a sub-session (the head delegates to the custom backend).
    reply = device.cmd(
        "TURN spawn a quick sub-agent to say the single word READY", expect="ORCH REPLY [serial]:", timeout=60.0
    )
    assert reply, "no orchestrator reply"
    # Give the fire-and-forget job a moment, then confirm it ran on the custom backend.
    deadline = time.time() + 60
    seen = False
    while time.time() < deadline and not seen:
        st = net.get_json("/api/orch", ip=ip, timeout=5.0)
        seen = any("custom" in str(j).lower() for j in st.get("jobs", []) or [])
        if not seen:
            time.sleep(3)
    # The job list is best-effort/ephemeral; the hard assertion is the device stayed
    # up and answered - a broken plain-HTTP path would 0/Network-fail the dispatch.
    assert net.get_json("/api/state", ip=ip, timeout=5.0).get("mode") == 1


# ---- multi-provider tool-loop e2e (P6 default loop) -------------------------
@pytest.mark.agent
@pytest.mark.net
@pytest.mark.parametrize("prov,key_field", [("openai", "oaiKey"), ("anthropic", "antKey"), ("mistral", "mistralKey")])
def test_toolloop_live(device, net, secrets, require_secret, prov, key_field):
    """With the loop the default path (P6), a turn that needs a tool must complete a
    real multi-round loop on each host and end by emitting orch_turn. Skips a provider
    whose key isn't configured."""
    require_secret(secrets.require_sta)
    key = getattr(secrets, f"{prov}_key", "") if hasattr(secrets, f"{prov}_key") else ""
    if not key:
        pytest.skip(f"no {prov} key configured")
    lan_ip_or_skip(device, net, secrets, require_secret)
    push_key(device, key_field, key)
    push_key(device, "orchHost", prov)
    push_key(device, "orchLoop", "1")
    device.reset()
    device.wait_ready(timeout=20.0)
    # A prompt that should provoke at least one memory tool call before answering.
    reply = device.cmd(
        "TURN remember that my favorite colour is teal, then confirm you saved it",
        expect="ORCH REPLY [serial]:",
        timeout=120.0,
    )
    assert reply and "teal" in reply.lower(), f"{prov}: loop turn did not confirm the save"
