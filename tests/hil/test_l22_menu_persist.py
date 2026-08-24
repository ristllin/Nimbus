"""L22 - a menu edit PERSISTS and APPLIES, on the touch path.

The gap this closes (found in the v3.9.0 review): the host suite proves the menu
FSM sets ``dirty()``, and the HIL suites prove screens transition - but nothing
asserted that a real menu edit travels through the device glue
(``settleMenuAfterMutation``: persist -> applyConfig -> re-read) and SURVIVES a
reboot. That exact seam broke once: the persist/apply block lived inside an
input pump's loop that a TFT board exits immediately, so the menu repainted
while every setting silently failed to save. A green here means that bug class
cannot come back unnoticed.

Oracle: the menu's OWN row text via the ``MENU?`` seam. ``openSettingsMenu``
re-reads the store into the FSM, so reopening the menu (and rebooting, then
reopening again) reads back what was actually persisted - a durable artifact,
not an echo (the tests-that-lie lesson).

The edited setting is Battery mode: a plain picker row reachable by tap, and
restorable out-of-band via the ``PROFILE <n>`` console setter (used ONLY in the
finally-restore, never as the test oracle - the point is to prove the MENU
writer).
"""

from __future__ import annotations

import re
import time

import pytest

# Landscape TFT layout constants - mirror lib/core/src/tft_screens.cpp
# (kGut=12, kBodyTop=kHeaderH+8=52) and theme.h (kHeaderH=44, kMinTap=44).
PANEL_W, PANEL_H = 320, 240
HEADER_H, MIN_TAP, GUT, BODY_TOP = 44, 44, 12, 52
# Value-editor stepper (tft_screens.cpp drawMenu adjusting branch):
#   card top cy = BODY_TOP + 20; buttons 56px at by = cy + 32; plus at
#   x = kW - kGut - 12 - 56; Save bar bottom-anchored at kH - kGut - kMinTap.
_BTN = 56
_CY = BODY_TOP + 20
_BY = _CY + 32
PLUS_CENTER = (PANEL_W - GUT - 12 - _BTN + _BTN // 2, _BY + _BTN // 2)
SAVE_CENTER = (PANEL_W // 2, PANEL_H - GUT - MIN_TAP + MIN_TAP // 2)
GEAR_TAP = (PANEL_W - MIN_TAP // 2, HEADER_H // 2)
BACK_TAP = (20, HEADER_H // 2)


def _scr_model(device) -> str:
    m = re.search(r"scr=(\w+)", device.cmd("STATUS", "STATUS ", timeout=8.0))
    return m.group(1) if m else "unknown"


def _menu_view(device):
    """Parse MENU? into (open, sel, items). Items may contain spaces; the item
    blob runs from ``items=`` to `` help=`` (if present) - split on '|' after."""
    raw = device.cmd("MENU?", "MENU ", timeout=8.0)
    line = raw[raw.index("MENU ") :].splitlines()[0]
    opened = "open=1" in line
    sel = int(re.search(r"sel=(-?\d+)", line).group(1))
    blob = line.split("items=", 1)[1] if "items=" in line else ""
    if " help=" in blob:
        blob = blob.split(" help=", 1)[0]
    items = blob.split("|") if blob else []
    return opened, sel, items


def _find_row(items, label):
    for i, it in enumerate(items):
        if label.lower() in it.lower():
            return i
    return -1


def _close_menu_tft(device):
    for _ in range(6):
        opened, _, _ = _menu_view(device)
        if not opened:
            return
        device.cmd(f"TAP {BACK_TAP[0]} {BACK_TAP[1]}", "TAP<", timeout=5.0)
        time.sleep(0.5)


# "Battery mode" row geometry (deterministic, from lib/core/src/tft_screens.cpp
# drawMenu: listTop=kBodyTop=52, kRowH=46 (+4 pitch), colW=(320-24-8)/2=144,
# COLUMN-MAJOR, page-aligned, 3 rows x 2 cols per page). Battery mode is Main
# row index 1 -> page 1, left column, slot 1: center (12+72, 52+50+23).
_BM_ROW_TAP = (GUT + 144 // 2, 52 + 1 * (46 + 4) + 23)
_BM_LABEL = "Battery mode"


@pytest.mark.hil
def test_menu_edit_persists_and_applies_tap(device):
    """Touch path: tap the "Battery mode" row (page 1 - Screensaver sits on
    page 2, which TAP injection cannot reach: there is no drag inject, and the
    two-column menu is page-aligned), tap [+], tap Save, Back out - then assert
    the edit persisted and survived a reboot.

    Restore goes through the PROFILE console command, which routes through the
    SAME selector path the menu uses - so the restore cannot mask a broken menu
    writer (the thing under test)."""
    if _scr_model(device) != "tft":
        pytest.skip("tap path needs the touch board (this one is not scr=tft)")

    device.cmd(f"TAP {GEAR_TAP[0]} {GEAR_TAP[1]}", "TAP<", timeout=5.0)  # gear
    time.sleep(0.7)
    opened, sel, items = _menu_view(device)
    assert opened, "menu did not open on the gear tap"
    idx = _find_row(items, _BM_LABEL)
    assert idx == 1, (
        f"'{_BM_LABEL}' is no longer Main row 1 (found {idx}): {items!r} - update _BM_ROW_TAP for the new layout"
    )

    orig_profile = None
    try:
        before = items[idx]
        # "Battery mode: Dark|Balanced|Full" -> remember the original for restore.
        val = before.split(":", 1)[1].strip().lower() if ":" in before else ""
        orig_profile = {"dark": 0, "balanced": 1, "full": 2}.get(val)

        device.cmd(f"TAP {_BM_ROW_TAP[0]} {_BM_ROW_TAP[1]}", "TAP<", timeout=5.0)
        time.sleep(0.7)
        o2, s2, it2 = _menu_view(device)
        # The row opens a PICKER submenu (measured on hardware): items are
        # Dark | Balanced | Full | < Back, current marked with '*'.
        in_picker = o2 and any("dark" in x.lower() for x in it2) and any("full" in x.lower() for x in it2)
        assert in_picker, (
            f"tapping ({_BM_ROW_TAP}) did not open the Battery mode picker: "
            f"sel={s2}, items={it2!r} - the menu-row geometry drifted; update "
            "_BM_ROW_TAP"
        )
        # Pick the first option that is NOT current ('*') and not Back. The
        # picker is the same two-column column-major layout: left column slots
        # 0..2 hold Dark/Balanced/Full.
        target = next(i for i, x in enumerate(it2) if "*" not in x and "back" not in x.lower())
        assert target <= 2, f"picker layout changed: {it2!r}"
        ty = 52 + target * (46 + 4) + 23
        device.cmd(f"TAP {_BM_ROW_TAP[0]} {ty}", "TAP<", timeout=5.0)
        time.sleep(0.7)
        _close_menu_tft(device)

        device.cmd(f"TAP {GEAR_TAP[0]} {GEAR_TAP[1]}", "TAP<", timeout=5.0)
        time.sleep(0.7)
        _, _, items2 = _menu_view(device)
        after = items2[_find_row(items2, _BM_LABEL)]
        assert after != before, (
            f"tap edit did not stick: row still {after!r} - the persist/apply "
            "drain (settleMenuAfterMutation) never ran for the touch path"
        )
        _close_menu_tft(device)

        # Reboot half. ⚠ Battery-mode overrides persist via the SD-backed config
        # blob - with NO SD card they do not survive a restart (known limitation
        # F10, canonical xfail R_F10 in test_regressions.py). On an SD-less
        # board the session-persist assertion above is the meaningful half;
        # xfail the reboot half rather than re-red-flagging a documented gap.
        sd_absent = "sd=absent" in device.cmd("STATUS", "STATUS ", timeout=8.0)
        device.reset()
        device.ensure_status_idle()
        device.cmd(f"TAP {GEAR_TAP[0]} {GEAR_TAP[1]}", "TAP<", timeout=5.0)
        time.sleep(0.7)
        _, _, items3 = _menu_view(device)
        rebooted = items3[_find_row(items3, _BM_LABEL)]
        if rebooted != after and sd_absent:
            pytest.xfail(
                f"F10: no SD card, so the battery-mode override reverted on "
                f"reboot ({after!r} -> {rebooted!r}) - session persist+apply "
                "PASSED; the reboot gap is the documented R_F10 limitation"
            )
        assert rebooted == after, f"survived the session but not the reboot: {after!r} -> {rebooted!r}"
    finally:
        _close_menu_tft(device)
        if orig_profile is not None:
            device.cmd(f"PROFILE {orig_profile}", "PROFILE", timeout=8.0)
        device.ensure_status_idle()
