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

# radio= (CUM-468) is the driver's own AP state, which the reconcile now decides on;
# up= is only the netif address, which stays set when the AP never started. Optional
# so the leg still runs against firmware that predates the field.
_WIFIAP_RE = (
    r"WIFIAP\?\s+ssid=(?P<ssid>\S+)\s+ip=(?P<ip>\S+)\s+up=(?P<up>\d)\s+"
    r"sta=(?P<sta>\d)\s+onboarded=(?P<ob>\d)\s+uptime=(?P<up_ms>\d+)"
    r"(?:\s+radio=(?P<radio>\d))?"
)


def _wifiap(device, timeout: float = 5.0):
    """Query the setup-AP state -> (up, ip, sta, onboarded, uptime_ms, radio).

    radio is None when the firmware does not report it, else a bool.
    """
    m = device.cmd_re("WIFIAP?", _WIFIAP_RE, timeout=timeout)
    radio = None if m["radio"] is None else m["radio"] == "1"
    return (m["up"] == "1", m["ip"], m["sta"] == "1", m["ob"] == "1", int(m["up_ms"]), radio)


@pytest.mark.hil
def test_setup_ap_self_recovers_without_reboot(device):
    device.ensure_mode(1)  # the setup AP only exists in Orchestrator mode
    assert device.ping(), "console must be alive before the test"

    # Enter a setup-like state: stop the station so the AP is the only interface -
    # exactly the first-run condition. Non-destructive; WIFIAP off resumes joining.
    device.cmd("WIFIAP on", "WIFIAP on", timeout=8.0)
    try:
        up0, ip0, _sta0, _ob0, uptime0, radio0 = _wifiap(device)
        assert up0, f"setup AP should be up after WIFIAP on, got ip={ip0}"
        assert radio0 is not False, "setup AP has an address but the radio says it is not running"

        # Force a teardown path (mirrors dropSoftAP / a wedged AP): softAPIP -> 0.0.0.0.
        device.cmd("WIFIAP drop", "WIFIAP drop", timeout=8.0)
        down_up, down_ip, _s, _o, uptime_drop, down_radio = _wifiap(device)
        assert not down_up, f"AP should read down right after the forced drop, got ip={down_ip}"
        assert down_radio is not True, "the radio still reports the AP right after the forced drop"
        assert uptime_drop >= uptime0, "device must not have reset during the forced drop"

        # THE assertion: the firmware brings the setup AP back on its own, in time,
        # with the device never resetting (uptime is monotonic across the recovery).
        deadline = time.monotonic() + AP_RECOVER_BUDGET_S
        recovered = False
        last = None
        while time.monotonic() < deadline:
            up, ip, _s, _o, uptime, radio = _wifiap(device)
            last = (up, ip, uptime, radio)
            assert uptime >= uptime_drop, (
                f"uptime went backwards ({uptime} < {uptime_drop}): the device RESET instead of self-recovering the AP"
            )
            # Recovered means on the air, not just addressed (CUM-468).
            if up and ip != "0.0.0.0" and radio is not False:
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
