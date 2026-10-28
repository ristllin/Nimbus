"""Storm leg a: the settings store full, with the same Telegram page re-served.

Reproduces the 19 Oct 2026 storm (triage items L19-1 and L19-33): the 20 KB settings
store filled, the Telegram position could not be saved, and the same short message was
served again on every poll (690 deliveries, 735 paid turns, no word to the owner).

Drive: the store filled to 0 free entries (``NVSFILL`` on the candidate, the web-API
filler on v4.5.13), then the same owner message delivered 20 times with a restart
before the 11th, then one fresh message. Unit L (the fill unit).

Bounds on the release candidate: at most one paid turn and one turn's head requests
for the re-served page, exactly one owner notice (the duplicate guard's), no log line
repeated more than 5 times, and the fresh message answered within 180 s while the store
is still full. v4.5.13 runs the same drive with the LAN mock as its head (no spend).

Run: NIMBUS_STORM_UNIT=L python3 -m pytest tests/hil/storm/test_storm_a_settings_full.py --allow-hardware -s
"""

from __future__ import annotations

import pytest

import kit

pytestmark = [pytest.mark.hil, pytest.mark.net]

LEG = "a_settings_full"
COPIES = 20
BOUNDS = kit.replay_bounds(COPIES)


def test_storm_a_settings_full(storm):
    with storm.session(kit.keep_for(storm), kit.head_for(storm)):
        with storm.settings_full() as full:
            print(f"[storm] settings store: {full}")
            m = kit.replay_storm(storm, LEG, COPIES, restart_at=COPIES // 2)
            m.extra["settings"] = full
            if not storm.control:
                # The 28 Oct ruling: a full settings store must not be silent. The
                # reserve-below trigger fires the storage notice (delivered, or
                # honestly deduped by the 24 h episode rule when the owner was already
                # told today - both prove the raise). Candidate images only.
                alive = m.extra.get("storageNoticeAlive", 0)
                print(
                    f"[storm] storage notice alive: {alive} "
                    f"(delivered {m.extra.get('storageNotices')}, deduped {m.extra.get('storageDedups')})"
                )
                assert alive >= 1, (
                    "the full settings store stayed silent: the storage notice was neither "
                    f"delivered nor deduped ({m.extra.get('storageNotices')}/"
                    f"{m.extra.get('storageDedups')}) - the reserve-below trigger "
                    "(src/agent/store.cpp) did not fire"
                )
    kit.assert_graded(storm.finish(LEG, m, BOUNDS))
