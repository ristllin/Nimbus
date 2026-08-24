#!/usr/bin/env python3
"""Host-only checks for the guarded device installer.

Covers the decision logic a board owner never sees fail loudly: which family a
port belongs to, what gets seeded, and the identify-and-confirm gate. The actual
flashing is hardware and is not exercised here."""

from __future__ import annotations

import argparse
import importlib.util
from pathlib import Path


def _load():
    path = Path(__file__).with_name("setup_device.py")
    spec = importlib.util.spec_from_file_location("setup_device", path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


SETUP = _load()


class _Input:
    """Feed a scripted sequence of answers to the module's input()."""

    def __init__(self, *answers):
        self.answers = list(answers)

    def __call__(self, _prompt=""):
        return self.answers.pop(0)


def _args(**kw):
    base = dict(display=None, mode=None, board=None, size=None, yes=False, port=None)
    base.update(kw)
    return argparse.Namespace(**base)


# ---- NVS + MAC (unchanged surface) ----------------------------------------


def test_nvs_classification_does_not_need_to_decode_values():
    assert SETUP.classify_nvs(b"\xff" * 20480) == "blank"
    assert SETUP.classify_nvs(b"\xff" * 40 + b"nimbus_mode\x00scrModel" + b"\xff" * 40) == "nimbus"
    assert SETUP.classify_nvs(b"solide\x00staSsid") == "other"


def test_extracts_the_factory_mac_from_esptool_output():
    output = "Connected to ESP32-S3\nMAC:                AA:BB:CC:DD:EE:FF\n"
    assert SETUP.extract_mac(output) == "aa:bb:cc:dd:ee:ff"
    assert SETUP.extract_mac("no identity here") is None


# ---- board-family autodetect ----------------------------------------------


def test_family_from_usb_bridge_is_solide_native_is_ambiguous():
    assert SETUP.family_from_usb(SETUP.VID_CP210X) == SETUP.FAMILY_SOLIDE
    assert SETUP.family_from_usb(SETUP.VID_CH34X) == SETUP.FAMILY_SOLIDE
    # The native ESP32-S3 USB is shared by the Freenove and a native-flashed
    # Solide, so USB alone cannot decide - NVS refines it.
    assert SETUP.family_from_usb(SETUP.VID_ESP32S3_NATIVE) is None
    assert SETUP.family_from_usb(None) is None


def test_family_from_nvs_marker_presence():
    assert SETUP.family_from_nvs(b"...otaType\x00freenove-35...") == SETUP.FAMILY_FREENOVE
    assert SETUP.family_from_nvs(b"...otaType\x00nimbus-tft...") == SETUP.FAMILY_SOLIDE
    assert SETUP.family_from_nvs(b"\xff" * 100) is None


def test_resolve_family_precedence_and_ambiguity():
    # Explicit --board always wins.
    assert SETUP.resolve_family("freenove_s3", SETUP.VID_CP210X, SETUP.FAMILY_SOLIDE) == "freenove_s3"
    # Then a configured board's own NVS marker beats the USB hint.
    assert SETUP.resolve_family(None, SETUP.VID_ESP32S3_NATIVE, SETUP.FAMILY_SOLIDE) == SETUP.FAMILY_SOLIDE
    # Then the USB bridge implies Solide.
    assert SETUP.resolve_family(None, SETUP.VID_CP210X, None) == SETUP.FAMILY_SOLIDE
    # A native board with no marker is AMBIGUOUS (could be a native-flashed Solide,
    # like Nimbus-4) -> None, so we never silently flash the wrong pinout.
    assert SETUP.resolve_family(None, SETUP.VID_ESP32S3_NATIVE, None) is None


def test_prompt_family_refuses_under_yes_and_asks_interactively():
    try:
        SETUP.prompt_family("/dev/cu.usbmodem1", assume_yes=True)
    except RuntimeError as exc:
        assert "--board" in str(exc)
    else:
        raise AssertionError("ambiguous family under --yes must demand --board")
    SETUP.input = _Input("1")
    try:
        assert SETUP.prompt_family("/dev/cu.usbmodem1", assume_yes=False) == SETUP.FAMILY_SOLIDE
    finally:
        del SETUP.input


def test_friendly_name_prefers_specific_product():
    assert SETUP.friendly_name(SETUP.FAMILY_SOLIDE, "") == "Nimbus board"
    assert SETUP.friendly_name(SETUP.FAMILY_FREENOVE, "USB Single Serial") == "Freenove CYD"
    assert SETUP.friendly_name(SETUP.FAMILY_SOLIDE, "Nimbus-4") == "Nimbus-4"


# ---- native USB accepted for every board ----------------------------------


def test_native_usb_ports_are_accepted_for_any_board():
    # The old hard rejection is gone: an explicit native-USB --port only fails on
    # existence now, never on being "the native port".
    args = _args(port="/dev/cu.usbmodem-does-not-exist")
    try:
        SETUP.resolve_port(args)
    except RuntimeError as exc:
        assert "does not exist" in str(exc)
        assert "native" not in str(exc).lower()
    else:
        raise AssertionError("a nonexistent port must still fail on existence")


# ---- seeding: e-ink gone, otaType added ------------------------------------


def test_bootstrap_commands_seed_display_mode_and_ota_type():
    # Solide TFT board: scrModel + the 180-degree flip + mode + typed-OTA slug.
    assert SETUP.bootstrap_commands("tft", "orchestrator", "solide_s3", "nimbus-tft") == [
        ("SET scrModel=tft", "SET scrModel ok=1"),
        ("SETI tftFlip=1", "SETI tftFlip=1 ok=1"),
        ("SETI nimbus_mode=1", "SETI nimbus_mode=1 ok=1"),
        ("SET otaType=nimbus-tft", "SET otaType ok=1"),
    ]
    # Freenove: no tftFlip (it owns its orientation), and a freenove-* slug.
    assert SETUP.bootstrap_commands("tft", "notifier", "freenove_s3", "freenove-35") == [
        ("SET scrModel=tft", "SET scrModel ok=1"),
        ("SETI nimbus_mode=0", "SETI nimbus_mode=0 ok=1"),
        ("SET otaType=freenove-35", "SET otaType ok=1"),
    ]


def test_freenove_ota_type_from_size_flag_and_default():
    assert SETUP.freenove_ota_type(_args(size="28")) == "freenove-28"
    assert SETUP.freenove_ota_type(_args(size="40")) == "freenove-40"
    # --yes with no size falls back to the base panel (all sizes share one image).
    assert SETUP.freenove_ota_type(_args(yes=True)) == "freenove-28"
    # Interactive prompt.
    SETUP.input = _Input("2")
    try:
        assert SETUP.freenove_ota_type(_args()) == "freenove-35"
    finally:
        del SETUP.input


def test_prompt_bootstrap_solide_and_freenove():
    # Solide fresh board: display seeded to tft, nimbus-tft slug.
    assert SETUP.prompt_bootstrap(_args(mode="orchestrator"), "blank", SETUP.FAMILY_SOLIDE) == (
        "tft",
        "orchestrator",
        "nimbus-tft",
    )
    # A configured Solide keeps its saved display (None) but is still re-seeded.
    assert SETUP.prompt_bootstrap(_args(mode="notifier"), "nimbus", SETUP.FAMILY_SOLIDE) == (
        None,
        "notifier",
        "nimbus-tft",
    )
    # Freenove derives display tft + size slug.
    assert SETUP.prompt_bootstrap(_args(mode="orchestrator", size="40"), "blank", SETUP.FAMILY_FREENOVE) == (
        "tft",
        "orchestrator",
        "freenove-40",
    )


def test_prompt_mode_yes_requires_mode_on_blank_board():
    try:
        SETUP.prompt_mode(_args(yes=True), known_nimbus=False)
    except RuntimeError as exc:
        assert "--mode" in str(exc)
    else:
        raise AssertionError("a blank board under --yes must demand a mode")
    # A configured board may keep its saved mode.
    assert SETUP.prompt_mode(_args(yes=True), known_nimbus=True) is None


# ---- identify-and-confirm --------------------------------------------------


def test_pick_port_single_and_ambiguous():
    one = [{"port": "/dev/cu.usbmodem1", "vid": SETUP.VID_ESP32S3_NATIVE, "pid": 1, "product": ""}]
    assert SETUP.pick_port(one, assume_yes=False)["port"] == "/dev/cu.usbmodem1"
    # Empty -> clear error.
    try:
        SETUP.pick_port([], assume_yes=False)
    except RuntimeError as exc:
        assert "No board" in str(exc)
    else:
        raise AssertionError("no boards must raise")
    # Several under --yes is ambiguous.
    two = one + [{"port": "/dev/cu.usbserial-9", "vid": SETUP.VID_CP210X, "pid": 2, "product": ""}]
    try:
        SETUP.pick_port(two, assume_yes=True)
    except RuntimeError as exc:
        assert "--port" in str(exc)
    else:
        raise AssertionError("--yes with several boards must demand --port")


def test_pick_port_identify_then_choose():
    two = [
        {"port": "/dev/cu.usbmodem1", "vid": SETUP.VID_ESP32S3_NATIVE, "pid": 1, "product": ""},
        {"port": "/dev/cu.usbserial-9", "vid": SETUP.VID_CP210X, "pid": 2, "product": ""},
    ]
    identified = []
    SETUP.identify = lambda port: identified.append(port) or True
    SETUP.input = _Input("i2", "1")  # identify board 2, then choose board 1
    try:
        chosen = SETUP.pick_port(two, assume_yes=False)
    finally:
        del SETUP.identify
        del SETUP.input
    assert identified == ["/dev/cu.usbserial-9"]
    assert chosen["port"] == "/dev/cu.usbmodem1"


def test_confirm_install_yes_and_decline():
    SETUP.confirm_install("Nimbus-4", SETUP.FAMILY_SOLIDE, True, "/dev/cu.usbserial-1", assume_yes=True)
    SETUP.input = _Input("")  # bare Enter == yes
    try:
        SETUP.confirm_install("Nimbus-4", SETUP.FAMILY_SOLIDE, True, "/dev/cu.x", assume_yes=False)
    finally:
        del SETUP.input
    SETUP.input = _Input("n")
    try:
        SETUP.confirm_install("blank", SETUP.FAMILY_FREENOVE, False, "/dev/cu.x", assume_yes=False)
    except RuntimeError as exc:
        assert "Cancelled" in str(exc)
    else:
        raise AssertionError("answering no must cancel")
    finally:
        del SETUP.input


def test_show_token_and_ota_type_args_plumb_through():
    args = SETUP.parse_args(["--port", "/dev/cu.usbserial-test", "--show-token"])
    assert args.show_token is True and args._show_token is False
    assert args.board is None  # auto-detect by default
    sub = SETUP.parse_args(["--_bootstrap-port", "/dev/x", "--board", "freenove_s3", "--_ota-type", "freenove-28"])
    assert sub._ota_type == "freenove-28" and sub.board == "freenove_s3"


if __name__ == "__main__":
    for name, fn in sorted(globals().items()):
        if name.startswith("test_") and callable(fn):
            fn()
    print("setup_device: family autodetect, otaType seed, identify-and-confirm, native-USB all passed")
