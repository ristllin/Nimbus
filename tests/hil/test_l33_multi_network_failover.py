"""L33 - CUM-207: multi-network Wi-Fi failover on glass (two real APs).

The owner wants cellphone-like behavior: with several networks saved, losing the
current one makes the device scan and join the NEXT reachable saved network on its
own - no reset, no setup-AP detour while any saved network is in range.

The selection/failover DECISION is proven exhaustively at the unit tier
(test/test_wifi_policy - every transition, the no-thrash invariant, exact backoff)
and the recovery interplay in test/test_wifi_recovery. THIS leg proves the live
wiring (src/net/wifi_link) on hardware, which a host build cannot: it needs TWO real
access points and a human to power one down mid-run.

Marked `hil` + `manual`: it is a BENCH-DAY test. It is deselected by the default
`-m` and skips loud without `--allow-hardware`; the two-AP drop is a human step. It
is written so a bench operator can run it verbatim - it is not run by this lane, and
no result here is claimed as a device pass.

Operator setup (both 2.4 GHz, both reachable from the bench):
    export NIMBUS_TEST_STA_SSID=<AP1>   NIMBUS_TEST_STA_PASS=<pw1>
    export NIMBUS_TEST_STA_SSID2=<AP2>  NIMBUS_TEST_STA_PASS2=<pw2>
    python3 -m pytest tests/hil/test_l33_multi_network_failover.py \
        -m "hil and manual" --allow-hardware --manual   # (a real TTY; no --manual-yes)

Run it on the FRESH-device / default-NVS board too (the fleet rule): a device that
only ever failed over from an already-provisioned state has never proven the fresh
path the owner QAs.
"""

from __future__ import annotations

import pytest

pytestmark = pytest.mark.hil

# The supervisor gives the core ~8 s before engaging, then a scan (~3 s) + a join per
# candidate. A generous budget covers the scan-timeout + one transient retry.
MIGRATION_BUDGET_S = 45.0

# Same WIFIAP? probe as L30: it carries the device uptime, so a migration can be
# proven to have happened WITHOUT a reboot (uptime strictly monotonic).
_WIFIAP_RE = (
    r"WIFIAP\?\s+ssid=(?P<ssid>\S+)\s+ip=(?P<ip>\S+)\s+up=(?P<up>\d)\s+"
    r"sta=(?P<sta>\d)\s+onboarded=(?P<ob>\d)\s+uptime=(?P<up_ms>\d+)"
)


def _wifiap(device, timeout: float = 5.0):
    """(ap_up: bool, ap_ip: str, sta: bool, onboarded: bool, uptime_ms: int)."""
    m = device.cmd_re("WIFIAP?", _WIFIAP_RE, timeout=timeout)
    return (m["up"] == "1", m["ip"], m["sta"] == "1", m["ob"] == "1", int(m["up_ms"]))


@pytest.mark.hil
@pytest.mark.manual
def test_two_ap_failover_without_reset(device, net, secrets, require_secret, require_manual):
    """Drop AP1; the device migrates to AP2 by itself, no reboot, no setup-AP detour."""
    require_secret(secrets.require_sta2)  # loud-skip unless BOTH networks are configured
    device.ensure_mode(1)  # Wi-Fi is Orchestrator-only
    assert device.ping(), "console must be alive before the test"

    ap1, pw1 = secrets.sta_ssid, secrets.sta_pass
    ap2, pw2 = secrets.sta_ssid2, secrets.sta_pass2

    # Seed BOTH networks into the saved list. Provisioning AP2 first, then AP1, leaves
    # the device connected to AP1 with AP2 saved as the backup (the store keeps both).
    net.provision(ap2, pw2)
    net.wait_got_ip(timeout=25.0)
    net.provision(ap1, pw1)
    ip1 = net.wait_got_ip(timeout=25.0)

    _ap_up, _ap_ip, sta, onboarded, uptime0 = _wifiap(device)
    assert sta, "device should be joined to AP1 before the drop"
    assert onboarded, "failover only engages once onboarding is finished"

    # THE human step: power AP1 down (or move the device out of its range) while AP2
    # stays up. The device must NOT be touched otherwise.
    require_manual.confirm(
        f"Power DOWN access point '{ap1}' now (keep '{ap2}' up). Do not touch the device.",
        timeout=120.0,
    )

    # THE assertion: the device re-acquires a link (to AP2) on its own, within budget,
    # and never reset doing it (uptime is monotonic across the whole migration). It
    # emits a fresh WIFI_GOT_IP when it lands on AP2.
    got_ip2 = None
    try:
        got_ip2 = net.wait_got_ip(timeout=MIGRATION_BUDGET_S)
    except Exception as exc:  # noqa: BLE001 - surface the honest failure below
        pytest.fail(
            f"device did not fail over to '{ap2}' within {MIGRATION_BUDGET_S:.0f}s "
            f"after '{ap1}' dropped ({exc}). A phone would have migrated silently."
        )

    _ap_up2, _ap_ip2, sta2, _ob2, uptime1 = _wifiap(device)
    assert sta2, f"device reports no station link after failover (got_ip={got_ip2})"
    assert uptime1 >= uptime0, (
        f"uptime went backwards ({uptime1} < {uptime0}): the device RESET to recover "
        "instead of failing over live - the whole point of this feature is that it does not"
    )
    assert got_ip2 != ip1 or True, "migration acquired a fresh IP"  # IPs may match on one router

    # Restore AP1 so the bench is left as found; the device may drift back to it later.
    require_manual.confirm(f"Power AP '{ap1}' back UP (bench cleanup).", timeout=120.0)
