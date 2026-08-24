"""Host tests for the release-gate host checks (so the gate logic is itself tested).

Run: python3 -m pytest tools/release_gate
"""

import check_driver_pin as pin
import check_elf_symbols as elf


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
