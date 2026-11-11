"""Host tests for the image-size gate (check_image_size.py).

Run: python3 -m pytest tools/release_gate

The last test runs the gate against the REAL tree's built images when they
exist (the battery builds esp32s3/esp32s3-cyd/test/test-cyd before the
citools leg, so there it gates for real and fails on an overflowing image,
e.g. test-cyd on ce65c154). In CI's gate-logic job there are no PlatformIO
builds, so that leg skips with a printed reason - never a silent pass.
"""

import glob
import os

import pytest

import check_image_size as gate

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))

SLOT = 0x640000  # 6,553,600 B, the default_16MB.csv app0/app1 size
BIG_SLOT = 0x700000  # 7,340,032 B, the test-only table's app slot

DEFAULT_CSV = (
    "# Name,   Type, SubType, Offset,  Size,     Flags\n"
    "nvs,      data, nvs,     0x9000,  0x5000,\n"
    "otadata,  data, ota,     0xe000,  0x2000,\n"
    f"app0,     app,  ota_0,   0x10000, {SLOT:#x},\n"
    f"app1,     app,  ota_1,   {SLOT + 0x10000:#x}, {SLOT:#x},\n"
    "spiffs,   data, spiffs,  0xc90000,0x360000,\n"
)

BIG_CSV = DEFAULT_CSV.replace(f"{SLOT:#x}", f"{BIG_SLOT:#x}")

INI = (
    "[s3]\n"
    "board_build.partitions = default_16MB.csv\n"
    "\n"
    "[env:esp32s3]\n"
    "extends = s3\n"
    "\n"
    "[env:test]\n"
    "extends = s3\n"
    "board_build.partitions = partitions_test_16MB.csv\n"
    "\n"
    "[env:testuart]\n"
    "extends = env:test\n"
)


def make_root(tmp_path, ini=INI, csvs=None, images=None, framework_csvs=None):
    """A fake repo: platformio.ini, partition CSVs, and .pio/build images."""
    root = tmp_path / "repo"
    root.mkdir(parents=True)
    (root / "platformio.ini").write_text(ini, encoding="utf-8")
    for name, text in (csvs or {"partitions_test_16MB.csv": BIG_CSV}).items():
        (root / name).write_text(text, encoding="utf-8")
    fw = tmp_path / "framework"
    fw.mkdir(parents=True, exist_ok=True)
    for name, text in (framework_csvs or {"default_16MB.csv": DEFAULT_CSV}).items():
        (fw / name).write_text(text, encoding="utf-8")
    build = root / ".pio" / "build"
    for env, size in (images or {}).items():
        d = build / env
        d.mkdir(parents=True, exist_ok=True)
        (d / "firmware.bin").write_bytes(b"\xe9" + b"\x00" * (size - 1))
    return str(root), [str(fw)]


def run_gate(root, fw, warn=gate.WARN_BYTES):
    build_dir = os.path.join(root, ".pio", "build")
    return gate.check(root, build_dir, warn, fw)


def test_overflow_fails_with_the_over_amount(tmp_path):
    root, fw = make_root(tmp_path, images={"esp32s3": SLOT + 336, "test": BIG_SLOT - 200000})
    ok, lines, fails, _ = run_gate(root, fw)
    assert not ok
    assert any("336 B over" in f and "esp32s3" in f for f in fails)
    assert any("test" in ln and "OK" in ln for ln in lines)


def test_headroom_under_64kb_warns_but_passes(tmp_path):
    root, fw = make_root(tmp_path, images={"esp32s3": SLOT - 1000})
    ok, _, fails, warns = run_gate(root, fw)
    assert ok and not fails
    assert any("headroom 1000 B" in w for w in warns)


def test_comfortable_headroom_passes_quietly(tmp_path):
    root, fw = make_root(tmp_path, images={"esp32s3": SLOT - 200000})
    ok, _, fails, warns = run_gate(root, fw)
    assert ok and not fails and not warns


def test_no_built_images_fails_loudly(tmp_path):
    root, fw = make_root(tmp_path, images={})
    ok, _, fails, _ = run_gate(root, fw)
    assert not ok
    assert any("no built firmware images" in f for f in fails)


def test_extends_chain_and_child_override_are_followed(tmp_path):
    # env:test overrides the [s3] table; env:testuart inherits env:test's.
    images = {"test": BIG_SLOT - 1000, "testuart": BIG_SLOT - 1000, "esp32s3": SLOT - 200000}
    root, fw = make_root(tmp_path, images=images)
    ok, lines, fails, _ = run_gate(root, fw)
    assert ok and not fails
    by_env = {ln.split()[0]: ln for ln in lines if ln and not ln.startswith("env")}
    assert "partitions_test_16MB.csv" in by_env["test"]
    assert "partitions_test_16MB.csv" in by_env["testuart"]
    assert "default_16MB.csv" in by_env["esp32s3"]


def test_named_table_resolves_in_the_framework_dir(tmp_path):
    # default_16MB.csv is not in the fake repo; it must resolve via the
    # framework partitions dir, exactly like a fresh clone does.
    root, fw = make_root(tmp_path, images={"esp32s3": SLOT - 200000})
    ok, lines, _, _ = run_gate(root, fw)
    assert ok
    assert any("default_16MB.csv" in ln for ln in lines)


def test_smallest_app_slot_decides_when_slots_differ(tmp_path):
    small = DEFAULT_CSV.replace(
        f"app1,     app,  ota_1,   {SLOT + 0x10000:#x}, {SLOT:#x},",
        f"app1,     app,  ota_1,   {SLOT + 0x10000:#x}, {SLOT - 0x10000:#x},",
    )
    root, fw = make_root(tmp_path, images={"esp32s3": SLOT - 0x8000}, framework_csvs={"default_16MB.csv": small})
    ok, _, fails, _ = run_gate(root, fw)
    assert not ok
    assert any("over app partition app0/app1" in f for f in fails)


def test_table_without_app_partition_fails(tmp_path):
    csv = "nvs,      data, nvs,     0x9000,  0x5000,\n"
    root, fw = make_root(tmp_path, images={"esp32s3": 1000}, framework_csvs={"default_16MB.csv": csv})
    ok, _, fails, _ = run_gate(root, fw)
    assert not ok
    assert any("no app partition" in f for f in fails)


def test_unresolvable_table_fails(tmp_path):
    ini = "[env:esp32s3]\nboard_build.partitions = missing_table.csv\n"
    root, fw = make_root(tmp_path, ini=ini, images={"esp32s3": 1000}, framework_csvs={})
    ok, _, fails, _ = run_gate(root, fw)
    assert not ok
    assert any("not found" in f for f in fails)


def test_main_exit_codes(tmp_path, capsys):
    root, fw = make_root(tmp_path / "a", images={"esp32s3": SLOT + 336})
    rc = gate.main(["--root", root, "--framework-partitions", fw[0]])
    out = capsys.readouterr().out
    assert rc == 1 and "FAIL" in out and "336 B over" in out
    root, fw = make_root(tmp_path / "b", images={"test": BIG_SLOT - 100000})
    rc = gate.main(["--root", root, "--framework-partitions", fw[0]])
    out = capsys.readouterr().out
    assert rc == 0 and "PASS" in out


def test_real_tree_gate_when_images_are_built(capsys):
    """The battery leg: with .pio/build images present, the gate must pass.

    On ce65c154 with test-cyd built this FAILS (image 336 B over the app slot);
    after the TESTFIT partition fix it passes. In CI's gate-logic job no
    PlatformIO builds exist, so the leg skips with a reason, never silently.
    """
    images = glob.glob(os.path.join(REPO, ".pio", "build", "*", "firmware.bin"))
    if not images:
        pytest.skip(
            "no built firmware images under .pio/build (CI gate-logic job has no PlatformIO builds); "
            "the battery's citools leg runs this gate with images present"
        )
    rc = gate.main(["--root", REPO])
    out = capsys.readouterr().out
    assert rc == 0, out
