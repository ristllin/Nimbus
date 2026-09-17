"""Host tests for the hardware-config <-> image mapping gate (CUM-388 / CUM-392).

Run: python3 -m pytest tools/release_gate

The class rule under test: EVERY supported config maps consistently across the web
installer, the release workflow, the web-flash builder, the typed device-type slugs,
and the PlatformIO envs - and a NEW config wired into one source but not the others
(or into the table but no source, or vice-versa) FAILS the gate. These tests mutate
one dimension at a time and assert the gate catches it, so a real future drift cannot
slip through green.
"""

import dataclasses

import check_config_mapping as m


# --- baseline: the real repo is consistent -----------------------------------
def test_real_repo_passes():
    ok, errs = m.run()
    assert ok, errs


# --- parsers ------------------------------------------------------------------
def test_type_allowed_families_partition():
    fam = m.parse_type_allowed_families(m._read(m.OTA_CPP_REL))
    assert fam["kTypeNimbusTft"] == "solide"
    assert fam["kTypeFreenove28"] == "freenove"
    assert fam["kTypeFreenove35"] == "freenove"
    assert fam["kTypeFreenove40"] == "freenove"


def test_pio_env_board_resolution():
    envs = m.parse_pio_envs(m._read(m.PIO_INI_REL))
    # The Solide env keeps the [s3] base board; the Freenove env unflags + re-adds.
    assert envs["esp32s3"]["board"] == "solide_s3"
    assert envs["esp32s3-cyd"]["board"] == "freenove_s3"
    assert envs["esp32s3-cyd-35"]["panel"] == (480, 320)
    assert envs["esp32s3-cyd-40"]["panel"] == (480, 480)
    # A comment block that documents a sibling env's -DSOLIDE_BOARD must NOT leak in.
    assert envs["esp32s3"]["ota_variant"] == "esp32s3"


def test_release_yml_wiring():
    rel = m.parse_release_yml(m._read(m.RELEASE_YML_REL))
    assert rel["manifest_pairs"]["nimbus-tft"] == "firmware-nimbus-tft.bin"
    assert rel["webflash_dir_of_type"]["freenove-35"] == "esp32s3-cyd-35"
    assert "esp32s3-cyd-40" in rel["built_envs"]


# --- the class rule: an unmapped / mis-mapped config FAILS --------------------
_EXTRA = m.Config(
    "freenove-50", "esp32s3-cyd-50", "freenove_s3", "cyd-50", (800, 480), "ESP32-S3", "firmware-freenove-50.bin", True
)


def test_new_config_in_table_but_no_source_fails():
    # A 5th supported config added to the canonical table but wired into NO source.
    ok, errs = m.run(table=m.CONFIG_TABLE + (_EXTRA,))
    assert not ok
    assert any("freenove-50" in e and "missing" in e for e in errs)


def test_new_config_in_a_source_but_not_table_fails():
    # A 5th variant wired into the web installer + slugs but absent from the table:
    # the gate must reject the extra, not wave it through.
    real = m.read_sources()
    real["flash_variants"] = real["flash_variants"] + ["freenove-50"]
    real["hdr_slugs"] = real["hdr_slugs"] + ["freenove-50"]
    ok, errs = m.judge(m.CONFIG_TABLE, real)
    assert not ok
    assert any("freenove-50" in e and "extra" in e for e in errs)


def test_wrong_board_fails():
    # nimbus-tft re-declared as a freenove pinout: a Solide-image-on-Freenove class bug.
    bad = _table_with(0, board="freenove_s3", is_freenove=False)
    ok, errs = m.run(table=bad)
    assert not ok
    assert any("nimbus-tft" in e and "SOLIDE_BOARD" in e for e in errs)


def test_wrong_panel_size_fails():
    # freenove-35 pointed at the 4.0" geometry: the panel-size-served-to-wrong-board class.
    bad = _table_with(2, panel=(480, 480))
    ok, errs = m.run(table=bad)
    assert not ok
    assert any("freenove-35" in e and "panel" in e for e in errs)


def test_wrong_image_name_fails():
    bad = _table_with(1, ota_image="firmware-wrong.bin")
    ok, errs = m.run(table=bad)
    assert not ok
    assert any("freenove-28" in e for e in errs)


def test_wrong_env_fails():
    # A config pointed at another config's env => it would build/ship the wrong pinout.
    bad = _table_with(3, env="esp32s3")
    ok, errs = m.run(table=bad)
    assert not ok
    assert any("freenove-40" in e for e in errs)


def test_duplicate_type_key_fails():
    dup = dataclasses.replace(m.CONFIG_TABLE[3], type_slug="freenove-35")
    bad = m.CONFIG_TABLE[:3] + (dup,)
    ok, errs = m.run(table=bad)
    assert not ok
    assert any("duplicate device-type key" in e for e in errs)


def test_device_side_family_flip_fails():
    # If the device-side typeAllowedForBoard put a freenove slug in the solide family,
    # a Solide board would accept a Freenove image. Simulate by flipping is_freenove in
    # the table so it no longer matches the (real) cpp family partition.
    bad = _table_with(1, is_freenove=False)
    ok, errs = m.run(table=bad)
    assert not ok
    assert any("freenove-28" in e and "family" in e for e in errs)


def test_wrong_offsets_flagged():
    # If the web-flash builder ever moved the app offset, the gate must catch it.
    real = m.read_sources()
    real["web"] = dict(real["web"])
    real["web"]["parts"] = (
        (0x0, "bootloader.bin"),
        (0x8000, "partitions.bin"),
        (0xE000, "boot_app0.bin"),
        (0x20000, "firmware.bin"),
    )
    ok, errs = m.judge(m.CONFIG_TABLE, real)
    assert not ok
    assert any("PARTS" in e or "offset" in e for e in errs)


# --- helpers ------------------------------------------------------------------
def _table_with(idx, **changes):
    row = dataclasses.replace(m.CONFIG_TABLE[idx], **changes)
    return m.CONFIG_TABLE[:idx] + (row,) + m.CONFIG_TABLE[idx + 1 :]
