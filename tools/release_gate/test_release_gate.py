"""Host tests for the release-gate host checks (so the gate logic is itself tested).

Run: python3 -m pytest tools/release_gate
"""

import check_driver_pin as pin
import check_elf_symbols as elf
import check_ota_preserves_nvs as otanvs


# --- driver-pin gate (CUM-167) ----------------------------------------------
def test_denylisted_pin_fails():
    ok, msg = pin.judge("v0.6.0")
    assert not ok and "CUM-167" in msg


def test_fixed_pin_passes():
    ok, _ = pin.judge("v0.6.1")
    assert ok


def test_missing_pin_fails():
    ok, _ = pin.judge(None)
    assert not ok


def test_find_pin_ignores_commented_lines():
    ini = "\n".join(
        [
            "lib_deps =",
            "    ; https://github.com/ristllin/solide-drivers.git#v0.6.0",
            "    https://github.com/ristllin/solide-drivers.git#v0.6.1",
            "    ; symlink://../solide-drivers",
        ]
    )
    assert pin.find_pin(ini) == "v0.6.1"


def test_find_pin_none_when_only_commented():
    ini = "    ; https://github.com/ristllin/solide-drivers.git#v0.6.0\n"
    assert pin.find_pin(ini) is None


# --- ELF symbol gate (CUM-167 size win) -------------------------------------
def test_epaper_symbol_fails():
    syms = "42007eb0 T _ZN6solide11display_tft5beginEv\n00000000 T _ZN6GxEPD2_BW7_InitEv\n"
    ok, msgs = elf.check(syms)
    assert not ok
    assert any("e-paper" in m for m in msgs)


def test_clean_tft_image_passes():
    syms = "42007eb0 T _ZN6solide11display_tft5beginEv\n42007e7c T _ZN6solide11display_tft12setBacklightEh\n"
    ok, _ = elf.check(syms)
    assert ok


def test_empty_elf_fails_missing_plumbing():
    ok, msgs = elf.check("00000000 T some_other_symbol\n")
    assert not ok
    assert any("plumbing" in m for m in msgs)


# --- OTA-preserves-NVS gate (CUM-237) ---------------------------------------
# A minimal key registry standing in for src/agent/agent_config.h: two OTA
# bookkeeping keys and two user-data keys.
_KEYS_HDR = "\n".join(
    [
        '#define AKEY_OTA_PENDING    "otaPend"',
        '#define AKEY_OTA_TYPE       "otaType"',
        '#define AKEY_TOUCH_CAL      "tchCal"',
        '#define AKEY_WIFI_SSID      "wifiSsid"',
    ]
)


def test_ota_writing_only_bookkeeping_passes():
    src = (
        'static const char* kSimCrashKey = "otaSimCrash";\n'
        "  nvs_set_i32(h, AKEY_OTA_PENDING, 0);\n"
        "  nvs_set_str(h, AKEY_OTA_TYPE, derived);\n"
        "  nvs_set_i32(h, kSimCrashKey, 0);\n"
    )
    ok, msgs = otanvs.audit(src, _KEYS_HDR)
    assert ok, msgs


def test_ota_writing_a_user_key_by_macro_fails():
    # The regression: an update path that clobbers the measured touch calibration.
    src = "  nvs_set_str(h, AKEY_TOUCH_CAL, buf);\n"
    ok, msgs = otanvs.audit(src, _KEYS_HDR)
    assert not ok
    assert any("tchCal" in m and "USER data key" in m for m in msgs)


def test_ota_writing_a_user_key_by_literal_fails():
    # Same regression written as a bare string literal, not the macro.
    src = '  nvs_set_str(h, "wifiSsid", ssid);\n'
    ok, msgs = otanvs.audit(src, _KEYS_HDR)
    assert not ok
    assert any("wifiSsid" in m for m in msgs)


def test_ota_erasing_a_user_key_fails():
    src = "  nvs_erase_key(h, AKEY_WIFI_SSID);\n"
    ok, msgs = otanvs.audit(src, _KEYS_HDR)
    assert not ok
    assert any("wifiSsid" in m for m in msgs)


def test_ota_unresolvable_key_fails_closed():
    # A key the guard cannot resolve to a literal must fail, not be waved through.
    src = "  nvs_set_str(h, someRuntimeKey, v);\n"
    ok, msgs = otanvs.audit(src, _KEYS_HDR)
    assert not ok
    assert any("UNRESOLVABLE" in m for m in msgs)


def test_ota_no_writes_is_a_stale_guard():
    ok, msgs = otanvs.audit("// nothing here\n", _KEYS_HDR)
    assert not ok
    assert any("stale" in m for m in msgs)


def test_ota_real_source_passes():
    # The guard must pass against the actual shipping OTA glue.
    import os

    root = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
    with open(os.path.join(root, otanvs.OTA_SRC_REL), encoding="utf-8") as fh:
        src = fh.read()
    with open(os.path.join(root, otanvs.KEYS_HDR_REL), encoding="utf-8") as fh:
        hdr = fh.read()
    ok, msgs = otanvs.audit(src, hdr)
    assert ok, msgs
