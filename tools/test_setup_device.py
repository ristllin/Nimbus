#!/usr/bin/env python3
"""Host-only checks for the guarded device installer."""

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


def test_nvs_classification_does_not_need_to_decode_values():
    assert SETUP.classify_nvs(b"\xff" * 20480) == "blank"
    assert SETUP.classify_nvs(b"\xff" * 40 + b"nimbus_mode\x00scrModel" + b"\xff" * 40) == "nimbus"
    assert SETUP.classify_nvs(b"solide\x00staSsid") == "other"
    assert SETUP.classify_nvs(b"factory-demo-state") == "other"


def test_extracts_the_factory_mac_from_esptool_output():
    output = "Connected to ESP32-S3\nMAC:                AA:BB:CC:DD:EE:FF\n"
    assert SETUP.extract_mac(output) == "aa:bb:cc:dd:ee:ff"
    assert SETUP.extract_mac("no identity here") is None


def test_native_usb_is_rejected_as_a_first_flash_target():
    assert SETUP.is_native_usb_port("/dev/cu.usbmodem101")
    assert SETUP.is_native_usb_port("/dev/ttyACM0")
    assert not SETUP.is_native_usb_port("/dev/cu.usbserial-A5069RR4")


def test_freenove_cyd_native_usb_port_is_allowed_when_requested():
    # Default (--board solide_s3): a native-USB port is rejected outright - the
    # DevKitC-1 must use its UART bridge. This must fail BEFORE any filesystem
    # check (the port below does not exist on the test machine either way).
    try:
        SETUP.select_port("/dev/cu.usbmodem101")
    except RuntimeError as exc:
        assert "native USB port" in str(exc)
    else:
        raise AssertionError("a native USB port must be rejected without allow_native")

    # --board freenove_s3 passes allow_native=True (its only port IS native USB -
    # it has no UART bridge to require instead). The rejection above must be
    # skipped, reaching the NEXT check (the port existing) instead.
    try:
        SETUP.select_port("/dev/cu.usbmodem101", allow_native=True)
    except RuntimeError as exc:
        assert "native USB port" not in str(exc)
        assert "does not exist" in str(exc)
    else:
        raise AssertionError("a nonexistent test port must still fail on existence")


def test_bootstrap_commands_keep_hardware_and_mode_independent():
    assert SETUP.bootstrap_commands("tft", "orchestrator") == [
        ("SET scrModel=tft", "SET scrModel ok=1"),
        ("SETI tftFlip=1", "SETI tftFlip=1 ok=1"),
        ("SETI nimbus_mode=1", "SETI nimbus_mode=1 ok=1"),
    ]
    assert SETUP.bootstrap_commands("eink", "notifier") == [
        ("SET scrModel=eink", "SET scrModel ok=1"),
        ("SETI nimbus_mode=0", "SETI nimbus_mode=0 ok=1"),
    ]


def test_unattended_non_nimbus_board_requires_both_bootstrap_choices():
    args = argparse.Namespace(display="tft", mode=None, yes=True)
    try:
        SETUP.prompt_bootstrap(args, "other")
    except RuntimeError as exc:
        assert "explicit --display and --mode" in str(exc)
    else:
        raise AssertionError("non-Nimbus NVS was allowed to skip a mode choice")

    args.mode = "orchestrator"
    assert SETUP.prompt_bootstrap(args, "other") == ("tft", "orchestrator")


def test_show_token_is_an_explicit_recovery_mode():
    args = SETUP.parse_args(["--port", "/dev/cu.usbserial-test", "--show-token"])
    assert args.show_token is True
    assert args._show_token is False


if __name__ == "__main__":
    test_nvs_classification_does_not_need_to_decode_values()
    test_extracts_the_factory_mac_from_esptool_output()
    test_native_usb_is_rejected_as_a_first_flash_target()
    test_freenove_cyd_native_usb_port_is_allowed_when_requested()
    test_bootstrap_commands_keep_hardware_and_mode_independent()
    test_unattended_non_nimbus_board_requires_both_bootstrap_choices()
    test_show_token_is_an_explicit_recovery_mode()
    print("setup_device: NVS guard, MAC parsing, and UART target checks passed")
