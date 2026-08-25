"""L30 - CUM-190: the first-run setup access point self-recovers, no reboot.

Owner field report: during first-time setup the wizard said "setup hotspot is
down, restart the device". A physical restart must never be required for a state
the firmware can recover. This leg forces the setup-AP teardown over the serial
console and asserts the firmware re-asserts the AP within a few seconds WITHOUT
resetting the device (uptime stays monotonic).

Non-destructive: it uses the WIFIAP escape hatch to enter a setup-like STA-down
state (the first-run condition), tears the AP down, watches it come back, then
resumes normal joining. No NVS is wiped; the board rejoins its LAN at the end.

The recovery DECISION is proven exhaustively at the unit tier in
test/test_wifi_recovery (decideSetupAp); this leg proves the live wiring on glass.
"""

from __future__ import annotations

import time

import pytest

pytestmark = pytest.mark.hil

# The reconcile watchdog runs every ~3 s; a restore should land well inside this.
AP_RECOVER_BUDGET_S = 15.0

_WIFIAP_RE = (
    r"WIFIAP\?\s+ssid=(?P<ssid>\S+)\s+ip=(?P<ip>\S+)\s+up=(?P<up>\d)\s+"
    r"sta=(?P<sta>\d)\s+onboarded=(?P<ob>\d)\s+uptime=(?P<up_ms>\d+)"
)


def _wifiap(device, timeout: float = 5.0):
    """Query the setup-AP state -> (up: bool, ip: str, sta: bool, onboarded: bool, uptime_ms: int)."""
    m = device.cmd_re("WIFIAP?", _WIFIAP_RE, timeout=timeout)
    return (m["up"] == "1", m["ip"], m["sta"] == "1", m["ob"] == "1", int(m["up_ms"]))


@pytest.mark.hil
def test_setup_ap_self_recovers_without_reboot(device):
    device.ensure_mode(1)  # the setup AP only exists in Orchestrator mode
    assert device.ping(), "console must be alive before the test"

    # Enter a setup-like state: stop the station so the AP is the only interface -
    # exactly the first-run condition. Non-destructive; WIFIAP off resumes joining.
    device.cmd("WIFIAP on", "WIFIAP on", timeout=8.0)
    try:
        up0, ip0, _sta0, _ob0, uptime0 = _wifiap(device)
        assert up0, f"setup AP should be up after WIFIAP on, got ip={ip0}"

        # Force a teardown path (mirrors dropSoftAP / a wedged AP): softAPIP -> 0.0.0.0.
        device.cmd("WIFIAP drop", "WIFIAP drop", timeout=8.0)
        down_up, down_ip, _s, _o, uptime_drop = _wifiap(device)
        assert not down_up, f"AP should read down right after the forced drop, got ip={down_ip}"
        assert uptime_drop >= uptime0, "device must not have reset during the forced drop"

        # THE assertion: the firmware brings the setup AP back on its own, in time,
        # with the device never resetting (uptime is monotonic across the recovery).
        deadline = time.monotonic() + AP_RECOVER_BUDGET_S
        recovered = False
        last = None
        while time.monotonic() < deadline:
            up, ip, _s, _o, uptime = _wifiap(device)
            last = (up, ip, uptime)
            assert uptime >= uptime_drop, (
                f"uptime went backwards ({uptime} < {uptime_drop}): the device RESET instead of self-recovering the AP"
            )
            if up and ip != "0.0.0.0":
                recovered = True
                break
            time.sleep(1.0)

        assert recovered, (
            f"setup AP did not self-recover within {AP_RECOVER_BUDGET_S}s "
            f"(last WIFIAP? = {last}); a first-run owner would be stranded"
        )
    finally:
        # Resume normal joining so the board returns to its LAN.
        device.cmd("WIFIAP off", "WIFIAP off", timeout=8.0)
