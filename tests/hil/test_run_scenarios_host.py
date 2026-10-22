"""Host test for the scenario runner's web auth: the token rides the X-Nimbus-Token
header on every request, never a ?t= query (the firmware ignores the query since
CUM-45, so the runner read every route as signed out)."""

from __future__ import annotations

import pytest

pytestmark = pytest.mark.host


def test_scenario_runner_sends_the_token_header_and_no_query():
    import run_scenarios

    dev = run_scenarios.Dev("192.0.2.10", "f" * 24)
    sent = []

    class Session:
        headers = dev.s.headers

        def get(self, url, timeout=None):
            sent.append(("GET", url, dict(self.headers)))

        def post(self, url, data=None, timeout=None):
            sent.append(("POST", url, dict(self.headers)))

    dev.s = Session()
    dev.get("/api/state")
    dev.get("/api/mem/episodic?session=web&limit=5")
    dev.post("/api/chat", {"text": "hi"})
    assert len(sent) == 3
    for method, url, headers in sent:
        assert headers.get("X-Nimbus-Token") == "f" * 24, method
        assert "t=ffff" not in url, f"{method} {url} carries the token in the query"
