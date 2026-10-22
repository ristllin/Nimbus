"""L29 host tier: the non-gated panel power verdict behind
test_l29_release_gate.TestRenderToGlass.test_panel_reports_display_on. No board.

The leg used to demand a non-zero RDDPM, a register this panel family reads as
0x00 even while it displays, so it failed on every non-gated board. The verdict now
rests on RDDST against the MADCTL written (the driver's own health check) and reads
RDDPM only when it answers. These cases keep every fault the old check could catch,
and the ones it could not, failing.
"""

from __future__ import annotations

import pytest

from test_l29_release_gate import panel_power_verdict

pytestmark = pytest.mark.host

MADCTL = 0xE8  # flipped Freenove panel: MY|MX|MV|BGR


def test_unit_a_reading_on_v4514_passes():
    # The 22 Oct release-run reading: RDDPM unreadable, RDDST 0x29 = MV|BGR + scan bit.
    assert panel_power_verdict(0x00000000, 0x29520000, MADCTL) == ""


def test_unflipped_panel_passes():
    assert panel_power_verdict(0, 0x28520000, 0x28) == ""


def test_dead_panel_fails():
    assert "not answering" in panel_power_verdict(0, 0, MADCTL)


def test_silently_reset_panel_fails():
    # A reset reverts MADCTL to 0x00: the status byte no longer mirrors what was written.
    assert "lost its setup" in panel_power_verdict(0, 0x00610000, MADCTL)


def test_raised_flip_bits_fail():
    # A partial state loss that raises MY/MX must not pass as healthy.
    assert "lost its setup" in panel_power_verdict(0, 0xE9520000, MADCTL)


def test_readable_rddpm_display_off_fails():
    assert "display off" in panel_power_verdict(0x98, 0x29520000, MADCTL)


def test_readable_rddpm_display_on_passes():
    assert panel_power_verdict(0x9C, 0x29520000, MADCTL) == ""
