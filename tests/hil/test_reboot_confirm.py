"""CUM-418 item 4: host tests for the native-USB confirmed-reboot helper.

Pure host tests (no board): they prove the DECISION logic device.reboot_and_confirm
depends on - "did a reboot actually take?" and "escalate to a hard reset when the
soft console REBOOT is a no-op" - with a scripted fake so the hardening itself is
covered, not just exercised live. The bench symptom this guards against is a soft
REBOOT the S3 ignored over native USB-CDC: uptime kept climbing (uptime=3254s
"after a reboot") and the old reset()+status() read it as a pass.
"""

import pytest

from device import BootError, Device, DeviceError, FRESH_BOOT_UPTIME_S, is_fresh_boot

pytestmark = pytest.mark.host


# --- the pure decision function ---------------------------------------------
def test_is_fresh_boot_pure():
    assert is_fresh_boot(3254, 4) is True  # dropped AND small -> real reboot
    assert is_fresh_boot(None, 2) is True  # small, no baseline -> real reboot
    assert is_fresh_boot(3254, 3254) is False  # no-op soft reboot (the bench bug)
    assert is_fresh_boot(3254, 3260) is False  # uptime climbed -> never restarted
    assert is_fresh_boot(100, None) is False  # console never settled
    assert is_fresh_boot(20, FRESH_BOOT_UPTIME_S - 1) is True  # below the fresh bar
    # Above the bar and NOT below the pre-reboot reading -> no restart happened.
    assert is_fresh_boot(FRESH_BOOT_UPTIME_S - 5, FRESH_BOOT_UPTIME_S + 1) is False


# --- the escalation flow -----------------------------------------------------
class _Rig(Device):
    """Device with the serial-touching seams scripted, so reboot_and_confirm's real
    control flow (soft path -> confirm -> escalate -> confirm) runs host-side."""

    def __init__(self, before, soft_uptimes, esptool_ok, hard_uptimes, boot_error=False):
        super().__init__(port="/dev/cu.fake")
        self._before = before
        self._soft = list(soft_uptimes)
        self._hard = list(hard_uptimes)
        self._esptool_ok = esptool_ok
        self._boot_error = boot_error
        self._esptooled = False
        self.calls = []

    def _uptime_or_none(self, timeout=4.0):
        return self._before

    def reset(self):
        self.calls.append("reset")
        return self

    def wait_ready(self, timeout=20.0):
        if self._boot_error:
            raise BootError("panic during boot")
        return (None, None)

    def _settled_uptime(self, timeout=25.0):
        pool = self._hard if self._esptooled else self._soft
        return pool.pop(0) if pool else None

    def esptool_hard_reset(self):
        self.calls.append("esptool")
        self._esptooled = True
        return self._esptool_ok

    def reopen_after_reenumerate(self, drain_boot=True):
        self.calls.append("reopen")
        return self


def test_soft_reboot_that_takes_needs_no_hard_reset():
    rig = _Rig(before=3254, soft_uptimes=[4], esptool_ok=False, hard_uptimes=[])
    assert rig.reboot_and_confirm() == 4
    assert "esptool" not in rig.calls  # the soft path was enough


def test_no_op_soft_reboot_escalates_to_hard_reset():
    # Soft REBOOT ignored (uptime stayed 3254), esptool hard reset then brings it up.
    rig = _Rig(before=3254, soft_uptimes=[3254], esptool_ok=True, hard_uptimes=[5])
    assert rig.reboot_and_confirm() == 5
    assert rig.calls == ["reset", "esptool", "reopen"]


def test_raises_when_neither_soft_nor_hard_reset_takes():
    rig = _Rig(before=3254, soft_uptimes=[3254], esptool_ok=True, hard_uptimes=[3254])
    with pytest.raises(DeviceError):
        rig.reboot_and_confirm()


def test_raises_when_esptool_is_unavailable_and_soft_was_a_no_op():
    rig = _Rig(before=3254, soft_uptimes=[3254], esptool_ok=False, hard_uptimes=[])
    with pytest.raises(DeviceError):
        rig.reboot_and_confirm()
    assert "esptool" in rig.calls and "reopen" not in rig.calls  # no reopen after a failed escalation


def test_a_boot_loop_or_panic_propagates_not_swallowed():
    # wait_ready raising BootError is a real finding - reboot_and_confirm must not
    # mask it as "did not reboot".
    rig = _Rig(before=100, soft_uptimes=[2], esptool_ok=True, hard_uptimes=[2], boot_error=True)
    with pytest.raises(BootError):
        rig.reboot_and_confirm()
