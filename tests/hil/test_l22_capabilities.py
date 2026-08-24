"""§L22 - Capabilities table availability (CUM-159).

Asserts the live ``GET /api/tools`` seam carries the where-it-runs tag the web
Capabilities > Tools panel badges each row with: registry + device rows are
``orchestrator-direct``, and every connector row is one of the three known
slugs. This is the real-seam check behind the UI badge, so a wrong or missing
classification fails here rather than only being visible in the browser.

Markers: ``net`` (+ ``hil``). Gated behind ``--allow-hardware``; collects clean
with no board attached. Reuse an already-joined board with ``NIMBUS_TEST_IP`` +
``NIMBUS_TEST_TOKEN``.

Run:  python3 -m pytest tests/hil -m "net and not manual" --allow-hardware
"""

from __future__ import annotations

import pytest

from test_l4_network import lan_ip_or_skip

pytestmark = [pytest.mark.hil, pytest.mark.net]

# The frozen machine slugs (lib/core connectors_wire.h capScopeSlug). If these
# change, the web badge map (ui_js.h AVL) and this test move together.
SCOPES = {"orchestrator-direct", "subsessions-only", "unavailable"}


def test_api_tools_carries_availability(device, net, secrets, require_secret):
    ip = lan_ip_or_skip(device, net, secrets, require_secret)
    doc = net.get_json("/api/tools", ip=ip, timeout=8.0)
    tools = doc.get("tools") or []
    assert tools, "/api/tools returned no rows"

    reg_dev = [t for t in tools if t.get("group") in ("registry", "device")]
    assert reg_dev, "no registry/device rows in /api/tools"
    for t in reg_dev:
        # The head runs its own registry + turn-contract device surface directly.
        assert t.get("availability") == "orchestrator-direct", (
            f"{t.get('group')} tool {t.get('name')!r} should be orchestrator-direct, got {t.get('availability')!r}"
        )

    for t in tools:
        av = t.get("availability")
        # Connector rows always classify; the low-memory placeholder row omits it.
        if t.get("group") == "connector" and t.get("name") != "(connectors hidden)":
            assert av in SCOPES, f"connector {t.get('name')!r} has bad availability {av!r}"
        # Any row that does carry the field must use a known slug.
        if av is not None:
            assert av in SCOPES, f"row {t.get('name')!r} has unknown availability {av!r}"
