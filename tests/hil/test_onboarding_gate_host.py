"""Host tier: the onboarding gate (bench row H27) against a fake device. No board.

The gate runs for real over HTTP to a fake on localhost, with the fake console standing in for the serial line, so
every step's assertion is exercised. Each way the gate can go wrong is one small test here: a unit that is not
fresh, a missing provider key, a refused first answer, a crash on the restart, no person at the bench in
production-image mode. A green gate here could also have gone red.
"""

from __future__ import annotations

import json
import os
from types import SimpleNamespace

import pytest

import bench_driver as bd
import fake_bench_device as fake
import onboarding_gate as og
import port_guard

pytestmark = pytest.mark.host

guard = port_guard.load()
BENCH_MAC = "AA:BB:CC:00:11:22"
# Built from the guard's own list, never written out: a MAC in a runner is how it gets pasted into a command.
FORBIDDEN = ":".join(next(iter(guard.FORBIDDEN_MACS))[i : i + 2] for i in range(0, 12, 2))
ENV_TEXT = (
    "NIMBUS_TEST_STA_SSID=bench-ssid\nNIMBUS_TEST_STA_PASS=bench-pass\nANTHROPIC_API_KEY=sk-fake-not-a-real-key\n"
)


@pytest.fixture
def bus(tmp_path, monkeypatch):
    """One bench board on a real node, as the guard's USB listing sees it."""
    monkeypatch.delenv(guard.ALLOW_ENV, raising=False)
    monkeypatch.delenv(guard.ANON_ENV, raising=False)
    node = tmp_path / "cu.usbmodem1101"
    node.write_text("")
    rows = [SimpleNamespace(device=str(node), serial_number=BENCH_MAC, vid=guard.VID_ESPRESSIF, pid=0x1001, product="")]
    monkeypatch.setattr(guard, "comports", lambda: list(rows))
    return str(node)


def make_gate(tmp_path, device, console, port, opts=None):
    """A Gate over the fake device. ``opts``: no_console, env_text, ask."""
    opts = opts or {}
    env = tmp_path / "env"
    env.write_text(opts.get("env_text", ENV_TEXT))
    out = tmp_path / "ev"
    argv = ["--out", str(out), "--env-file", str(env), "--ip", device.ip]
    if opts.get("no_console"):
        token_file = tmp_path / "tok"
        token_file.write_text(fake.TOKEN)
        argv += ["--no-console", "--token-file", str(token_file)]
    else:
        argv += ["--port", port, "--mac", BENCH_MAC]
    args = og.parse_args(argv)
    api = bd.Api(device.ip, fake.TOKEN)
    return og.Gate(
        args=args,
        ev=bd.Evidence(str(out), api.scrub),
        api=api,
        ask=opts.get("ask", lambda question: True),
        open_console=lambda: console,
        settle_s=0.01,
    )


def grades(verdict):
    return {row["step"]: row["grade"] for row in verdict["steps"]}


def console_gate(tmp_path, bus, device, opts=None):
    console = fake.FakeConsole(device, ip_on_join=device.ip)
    return console, og.run_gate(make_gate(tmp_path, device, console, bus, opts))


def test_a_fresh_unit_passes_the_whole_gate(tmp_path, bus):
    with fake.FakeDevice() as device:
        console, verdict = console_gate(tmp_path, bus, device)
    assert verdict["grade"] == "PASS", verdict["steps"]
    assert grades(verdict) == {name: "PASS" for name, _, _ in og.STEPS}
    out = tmp_path / "ev"
    assert json.loads((out / "gate.json").read_text())["grade"] == "PASS"
    assert len(list(out.glob("step-*.json"))) == len(og.STEPS)
    for root, _, names in os.walk(out):
        for name in names:
            text = open(os.path.join(root, name), encoding="utf-8", errors="replace").read()
            assert fake.TOKEN not in text, name
            assert "sk-fake-not-a-real-key" not in text, name
            assert "bench-pass" not in text, name
    assert "[secret]" in console.sent
    assert not any("bench-pass" in line for line in console.sent)


def test_a_unit_that_is_not_fresh_fails_and_the_rest_do_not_run(tmp_path, bus):
    with fake.FakeDevice() as device:
        device.state["needsOnboarding"] = False
        _, verdict = console_gate(tmp_path, bus, device)
    got = grades(verdict)
    assert verdict["grade"] == "FAIL"
    assert got["is-fresh"] == "FAIL"
    assert got["first-screen"] == "NOT RUN" and got["restart"] == "NOT RUN"


def test_a_missing_provider_key_blocks_the_gate_never_passes(tmp_path, bus):
    env_text = ENV_TEXT.replace("ANTHROPIC_API_KEY=sk-fake-not-a-real-key\n", "")
    with fake.FakeDevice() as device:
        _, verdict = console_gate(tmp_path, bus, device, {"env_text": env_text})
    got = grades(verdict)
    assert verdict["grade"] == "BLOCKED"
    assert got["provider"] == "BLOCKED"
    assert got["complete"] == "NOT RUN"


def test_a_refused_first_answer_fails_the_gate(tmp_path, bus):
    with fake.FakeDevice() as device:
        device.chat_code = 503
        _, verdict = console_gate(tmp_path, bus, device)
    got = grades(verdict)
    assert verdict["grade"] == "FAIL"
    assert got["first-answer"] == "FAIL"
    assert got["restart"] == "NOT RUN"


def test_a_crash_on_the_restart_fails_the_gate(tmp_path, bus):
    with fake.FakeDevice() as device:
        device.state["boot"]["crash"] = {"thisReset": True, "why": "panic"}
        _, verdict = console_gate(tmp_path, bus, device)
    got = grades(verdict)
    assert verdict["grade"] == "FAIL"
    assert got["restart"] == "FAIL"


def test_no_console_without_a_person_is_blocked_not_pass(tmp_path):
    with fake.FakeDevice() as device:
        opts = {"no_console": True, "ask": lambda question: None}
        verdict = og.run_gate(make_gate(tmp_path, device, None, None, opts))
    got = grades(verdict)
    assert verdict["grade"] == "BLOCKED"
    assert got["is-fresh"] == "BLOCKED"
    assert got["wifi"] == "NOT RUN"


def test_no_console_with_a_person_passes(tmp_path):
    with fake.FakeDevice() as device:
        device.state["sta"] = True  # the wizard joined Wi-Fi by hand

        def ask(question):
            if "power-cycle" in question:
                device.restart()
            return True

        verdict = og.run_gate(make_gate(tmp_path, device, None, None, {"no_console": True, "ask": ask}))
    assert verdict["grade"] == "PASS", verdict["steps"]


def test_main_refuses_a_forbidden_mac_before_anything_opens(tmp_path, bus):
    env = tmp_path / "env"
    env.write_text(ENV_TEXT)
    rc = og.main(["--port", bus, "--mac", FORBIDDEN, "--out", str(tmp_path / "ev"), "--env-file", str(env)])
    assert rc == guard.REFUSED_EXIT
    verdict = json.loads((tmp_path / "ev" / "gate.json").read_text())
    assert verdict["grade"] == "FAIL"
    assert "refused" in verdict["steps"][0]["detail"]
    assert not (tmp_path / "ev" / "console.log").exists()
