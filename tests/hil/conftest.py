"""Pytest configuration for the Nimbus HIL harness (the HIL test spec).

Guarantees:
  * ``pytest --collect-only tests/hil`` imports + collects with ZERO errors on a
    device-less box (no hardware touched at import/collection time).
  * Every hardware-touching test (hil/net/agent/audio) is a LOUD, REASONED SKIP
    unless ``--allow-hardware`` is passed - never a silent pass.
  * Manual steps fail LOUD if unconfirmed (see manual.py).

This directory is added to ``sys.path`` (below) so the sibling modules import as
top-level (``import device`` / ``from net import Net``), matching how the test
modules reference them.
"""

from __future__ import annotations

import os
import sys

import pytest

# Make the harness modules importable as top-level names from the test files.
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from device import Device, DeviceError  # noqa: E402
from manual import ManualStep  # noqa: E402
from net import IP_READY_WAIT_S, Net  # noqa: E402
from nsn import BrokerUnavailable, NsnInjector  # noqa: E402
from redact import scrub  # noqa: E402
from secrets import Secrets, SecretsUnavailable  # noqa: E402

HARDWARE_MARKERS = ("hil", "net", "agent", "audio", "destructive")
# Credentials some legs read straight from the environment: scrubbed from every test
# report even when no Secret was ever built from them.
SECRET_ENV = (
    "NIMBUS_TEST_TG_TOKEN",
    "NIMBUS_TEST_TG_CHAT",
    "NIMBUS_TEST_STA_PASS",
    "NIMBUS_TEST_STA_PASS2",
    "NIMBUS_TEST_TOKEN",
)
GATE_REASON = "hardware gated: pass --allow-hardware (board must be recovered first - F11/F12 brick)"

# ---- CUM-141: mid-run console-wedge detection + in-place auto-recovery ------
# The pure, host-testable core lives in wedge_guard.py; here it is wired to the
# pytest runtest hooks (below). On the FIRST console-death signature the sentinel
# attempts ONE in-place USB bus reset (clears the stalled endpoint without
# rebooting the chip); if the console does not come back the session is marked
# wedged and every remaining hardware test FAILS FAST with a clear marker instead
# of each burning a full read timeout (the 58-cascade this replaces).
from wedge_guard import WedgeSentinel, is_wedge_candidate as _is_wedge_candidate  # noqa: E402


# ---- markers (registered here AND in pytest.ini; belt + suspenders) --------
def pytest_configure(config: "pytest.Config") -> None:
    for name, desc in (
        ("host", "pure host-side test, no device"),
        ("hil", "needs the device on serial"),
        ("net", "needs the device on a LAN / HTTP-reachable"),
        ("agent", "needs provider API keys and/or network"),
        ("audio", "needs the audio board and a human"),
        ("manual", "human-assisted step; loud-fails if unconfirmed"),
        ("connectors", "live provider-connector E2E lifecycle; gated + agent + net"),
        ("qa", "comprehensive pre-OTA connector + follow-up QA (recorded + judged); gated"),
        ("destructive", "factory-resets the board; runs only with -m destructive --allow-hardware"),
    ):
        config.addinivalue_line("markers", f"{name}: {desc}")
    # One wedge sentinel per session, reachable from the runtest hooks below.
    config._nimbus_wedge = WedgeSentinel(enabled=not config.getoption("--no-wedge-recovery"))


# ---- CLI options -----------------------------------------------------------
def pytest_addoption(parser: "pytest.Parser") -> None:
    g = parser.getgroup("nimbus-hil")
    g.addoption(
        "--port",
        action="store",
        default=None,
        help="serial port (default: env NIMBUS_PORT or auto-glob /dev/cu.usbmodem*)",
    )
    g.addoption(
        "--flash-env",
        action="store",
        default=None,
        help="if set, flash this pio env once per session before tests "
        "(requires --allow-hardware + NIMBUS_HIL_FLASH_OK=1)",
    )
    g.addoption(
        "--allow-hardware",
        action="store_true",
        default=False,
        help="HARD GATE: without it every hil/net/agent/audio test is a loud skip (board is bricked/single-owner)",
    )
    g.addoption(
        "--manual-yes",
        action="store_true",
        default=False,
        help="pre-confirm the manual PROMPT keystroke for CI dry-runs; "
        "never bypasses the test's own assertion, refused under "
        "`-m manual`",
    )
    g.addoption(
        "--no-wedge-recovery",
        action="store_true",
        default=False,
        help="CUM-141: disable the automatic in-place USB bus-reset recovery on a "
        "mid-run console wedge (leave the raw cascade for debugging)",
    )


# ---- collection-safety gate (spec §2.4) ------------------------------------
def pytest_collection_modifyitems(config: "pytest.Config", items) -> None:
    """Attach a LOUD, REASONED skip to every hardware test when --allow-hardware
    is absent. This keeps ``--collect-only`` clean on a device-less box while making
    the skip explicit - never a silent green pass."""
    if config.getoption("--allow-hardware"):
        return
    skip_gate = pytest.mark.skip(reason=GATE_REASON)
    for item in items:
        if any(item.get_closest_marker(m) for m in HARDWARE_MARKERS):
            item.add_marker(skip_gate)


def _is_hardware_item(item) -> bool:
    return any(item.get_closest_marker(m) for m in HARDWARE_MARKERS)


# ---- CUM-141: fail-fast once the console is known-wedged -------------------
def pytest_runtest_setup(item: "pytest.Item") -> None:
    """Before a hardware test runs, if the console is already known-wedged and
    unrecovered, fail it IMMEDIATELY with a clear marker rather than letting each
    console command burn a full read timeout (the 58-cascade this replaces)."""
    sentinel = getattr(item.config, "_nimbus_wedge", None)
    if sentinel is None or not sentinel.wedged:
        return
    if _is_hardware_item(item):
        pytest.fail(
            "CONSOLE WEDGED (CUM-141): the USB-Serial-JTAG console stalled earlier "
            "and an in-place bus reset did not bring it back. Remaining hardware "
            "tests fail fast. Recover with `python3 tools/usb_reset.py --serial <MAC>` "
            "(or power-cycle / BOOT+RST) and rerun.",
            pytrace=False,
        )


def scrub_report(report) -> None:
    """Replace every known secret value in a test report with its redacted form
    before pytest prints it: the traceback text, a skip reason, and the captured
    output sections. Repr redaction (redact.Secret) cannot reach a value an f-string
    already copied into a URL, a library error quoting that URL, or pytest's
    character diff of a failed string comparison; this does. A report with nothing
    to scrub is left exactly as pytest built it."""
    extra = [os.environ.get(name, "") for name in SECRET_ENV]
    longrepr = report.longrepr
    if isinstance(longrepr, tuple) and len(longrepr) == 3:  # a skip: (path, line, reason)
        report.longrepr = (longrepr[0], longrepr[1], scrub(str(longrepr[2]), extra))
    elif longrepr is not None:
        text = str(longrepr)
        clean = scrub(text, extra)
        if clean != text:
            report.longrepr = clean
    report.sections = [(title, scrub(body, extra)) for title, body in report.sections]


@pytest.hookimpl(hookwrapper=True)
def pytest_runtest_makereport(item: "pytest.Item", call):
    """Scrub secrets from every report (scrub_report). Then detect a console-death
    signature in a hardware test's failure and, on the first one, attempt a single
    in-place recovery (CUM-141). A genuine assertion failure or a firmware
    boot-loop (BootError) is NOT a wedge and flows through untouched."""
    outcome = yield
    report = outcome.get_result()
    scrub_report(report)
    if report.when != "call" or not report.failed:
        return
    sentinel = getattr(item.config, "_nimbus_wedge", None)
    if sentinel is None or not sentinel.enabled or sentinel.wedged or sentinel.recovery_attempted:
        return
    if not _is_hardware_item(item) or getattr(call, "excinfo", None) is None:
        return
    # A candidate signature only ARMS the check; try_recover() confirms the console
    # is actually dead (via a ping) before any disruptive USB reset, so an ordinary
    # ExpectTimeout on a live console does not trigger a gratuitous reset.
    if _is_wedge_candidate(call.excinfo.value):
        sentinel.try_recover()


# ---- fixtures --------------------------------------------------------------
@pytest.fixture(scope="session")
def allow_hardware(request) -> bool:
    return bool(request.config.getoption("--allow-hardware"))


@pytest.fixture(scope="session")
def port(request) -> "str | None":
    return request.config.getoption("--port") or os.environ.get("NIMBUS_PORT")


@pytest.fixture(scope="session")
def secrets() -> Secrets:
    """Value-hiding view over the secrets .env + test env vars. Construction does no IO;
    individual property accesses raise SecretsUnavailable (which tests convert to a
    loud skip) when a required source is missing. Every value it returns is a
    redact.Secret, so a traceback never prints one."""
    return Secrets()


@pytest.fixture(scope="session")
def device(request, allow_hardware, port):
    """Session serial link. LOUD-skips when the hardware gate is closed (so a
    device-less run never touches a port). Opens, resets to a known boot state,
    yields, and closes on teardown."""
    if not allow_hardware:
        pytest.skip(GATE_REASON)

    flash_env = request.config.getoption("--flash-env")
    dev = Device(port=port, flash_env=flash_env)
    # CUM-141: hand the live device to the wedge sentinel so the runtest hooks can
    # attempt an in-place USB bus-reset recovery if the console stalls mid-run.
    sentinel = getattr(request.config, "_nimbus_wedge", None)
    if sentinel is not None:
        sentinel.device = dev

    # LAN-only mode: with NIMBUS_TEST_IP + NIMBUS_TEST_TOKEN set, a `net` suite
    # reaches the board entirely over HTTP and needs no console. Serial bring-up
    # here would still open + RESET the port - and repeated console opens wedge
    # the host CDC driver (seen three times in one session while building the
    # v3.6.0 suites: the board serves HTTP perfectly while pytest reports
    # "console unresponsive"). So a console failure is NON-FATAL in that mode:
    # yield the unopened device, and any test that actually calls device.cmd()
    # fails loudly on its own rather than taking the whole LAN suite down.
    lan_only = bool(os.environ.get("NIMBUS_TEST_IP") and os.environ.get("NIMBUS_TEST_TOKEN"))

    if flash_env:
        # Double-interlocked inside flash_env(); this raises loud if not armed.
        dev.flash_env(flash_env, allow_hardware=allow_hardware)
    else:
        try:
            dev.open()
        except DeviceError as exc:
            if lan_only:
                print(f"\n[conftest] serial unavailable ({exc}) - LAN-only mode, continuing")
                dev.lan_only = True  # console tests SKIP rather than error
                yield dev
                return
            pytest.skip(f"no device on serial: {exc}")

    # Reset to a clean boot and resync the stream before handing over.
    try:
        dev.reset()
    except DeviceError as exc:
        if lan_only:
            print(f"\n[conftest] console did not come up ({exc}) - LAN-only mode, continuing")
            dev.lan_only = True  # console tests SKIP rather than error
            yield dev
            return
        pytest.fail(f"device did not come up after reset: {exc}")

    yield dev
    dev.close()


@pytest.fixture(scope="session")
def net(device, secrets) -> Net:
    """LAN helper over the serial device. Depends on ``device`` so it inherits the
    same hardware gate.

    Seeded with ``NIMBUS_TEST_IP`` when it is set, so a plain ``net`` leg runs
    standalone (the bench run on 19 Oct 2026 failed every such leg with "no IP
    known"). With a console, the ``device`` fixture has just restarted the board,
    so the fixture first lets the station rejoin (Net.settle_after_reset: bounded,
    never provisions, the console's own IP wins over a disagreeing seed), then
    waits, best effort, for the address to answer before the first test fires.
    Without the variable or a console, ``ip`` comes from wait_got_ip() or
    ensure_station() as before."""
    n = Net.from_env(device, secrets)
    seen = None if device.lan_only else n.settle_after_reset()
    notifier = seen is not None and seen.mode == 0  # no Wi-Fi and no web server to wait for
    if n.ip and not notifier and not n.wait_reachable(IP_READY_WAIT_S):
        print(f"\n[conftest] {n.ip} did not answer within {IP_READY_WAIT_S:.0f} s - continuing")
    return n


@pytest.fixture()
def bench(device, net, secrets, require_secret):
    """Orchestrator mode on Wi-Fi with the DEDICATED test bot, the test chat as the
    only (owner) chat; the bot token is cleared afterwards.

    Joins through Net.ensure_station, which waits out the rejoin after a restart and
    provisions only when the device cannot rejoin by itself. Yields a BenchBot: it
    unpacks as ``token, chat`` and its repr never shows either value."""
    require_secret(secrets.require_test_telegram)
    require_secret(secrets.require_sta)
    device.ensure_mode(1)
    net.ensure_station(secrets.sta_ssid, secrets.sta_pass)
    bot = secrets.bench_bot()
    r = net.post("/api/orch", {"tgToken": bot.token, "tgAllow": bot.chat}, timeout=8.0)
    assert r.status_code == 200, f"setting the test bot token failed: HTTP {r.status_code}"
    yield bot
    r = net.post("/api/orch", {"clr_tgToken": "1"}, timeout=8.0)
    assert r.status_code == 200, (
        f"clearing the test bot token failed (HTTP {r.status_code}); the unit may still poll it"
    )


@pytest.fixture()
def nsn(device):
    """nsn frame injector over BLE (the tools/nsn_send.py encoder, driven through
    the broker's own BleTransport - see nsn.py). Depends on ``device`` so it
    inherits the hardware gate; a missing broker package raises
    BrokerUnavailable, which the test converts to a LOUD skip.

    MUST close() the BLE connection on teardown: the device's GATT link is
    exclusive (one central at a time, v1 has no pairing/bonding queue) - a
    leaked connection from one test would block every test after it from ever
    connecting, a much worse failure mode than the old serial fixture had."""
    try:
        injector = NsnInjector(device)
    except BrokerUnavailable as exc:
        pytest.skip(str(exc))
    yield injector
    injector.close()


@pytest.fixture()
def wake_home(device):
    """Return a callable that brings the panel to a known StatusIdle 'home' state
    (taps Back out of any menu, dismisses screensaver/detail screens). Use at the
    top of a test that assumes the idle screen so it does not inherit whatever the
    previous test left on the glass. LAN-only runs (no console) skip cleanly via
    the device's own guard."""

    def _wake(timeout: float = 25.0):
        device.ensure_status_idle(timeout=timeout)

    return _wake


@pytest.fixture()
def require_manual(request) -> ManualStep:
    """Inject the loud manual-step runner. Any test using it is assertion-bearing:
    an unconfirmed step is a FAILURE, never a skip/pass."""
    return ManualStep(request.config)


# ---- helper: turn SecretsUnavailable into a loud, reasoned skip -------------
@pytest.fixture()
def require_secret(secrets):
    """Return a callable ``require(fn)`` that runs a Secrets guard (e.g.
    ``secrets.require_provider_keys``) and converts a SecretsUnavailable into a
    LOUD pytest.skip with its reason - never a silent pass."""

    def _require(guard):
        try:
            guard()
        except SecretsUnavailable as exc:
            pytest.skip(str(exc))

    return _require
