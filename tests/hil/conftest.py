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
from net import Net  # noqa: E402
from nsn import BrokerUnavailable, NsnInjector  # noqa: E402
from secrets import Secrets, SecretsUnavailable  # noqa: E402

HARDWARE_MARKERS = ("hil", "net", "agent", "audio")
GATE_REASON = "hardware gated: pass --allow-hardware (board must be recovered first - F11/F12 brick)"


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
    ):
        config.addinivalue_line("markers", f"{name}: {desc}")


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
    loud skip) when a required source is missing."""
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
    same hardware gate."""
    return Net(device, secrets)


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
