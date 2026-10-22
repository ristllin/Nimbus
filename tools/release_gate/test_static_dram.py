"""Host tests for the static DRAM ceiling gate (check_static_dram.py).

Run: python3 -m pytest tools/release_gate
"""

import os
import re
import struct

import check_static_dram as dram

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))


def make_elf(sections: dict) -> bytes:
    """A minimal ELF32 little-endian image with the named sections (name -> size)."""
    names = [""] + list(sections) + [".shstrtab"]
    strtab = b""
    offsets = []
    for n in names:
        offsets.append(len(strtab))
        strtab += n.encode() + b"\x00"
    header_size, entsize = 52, 40
    str_off = header_size
    shoff = str_off + len(strtab)
    shoff += (4 - shoff % 4) % 4
    shnum = len(names)
    ident = b"\x7fELF" + bytes([1, 1, 1]) + bytes(9)
    head = ident + struct.pack("<HHIIIIIHHHHHH", 2, 94, 1, 0, 0, shoff, 0, header_size, 0, 0, entsize, shnum, shnum - 1)
    blob = bytearray(head)
    blob += strtab
    blob += bytes(shoff - len(blob))
    for i, n in enumerate(names):
        size = len(strtab) if n == ".shstrtab" else sections.get(n, 0)
        off = str_off if n == ".shstrtab" else 0
        blob += struct.pack("<IIIIIIIIII", offsets[i], 3 if n == ".shstrtab" else 1, 0, 0, off, size, 0, 0, 1, 0)
    return bytes(blob)


CEIL = {"nimbus-tft": {"env": "esp32s3", "baseline": 1000, "ceiling": 1100}}


def test_sums_bss_and_data():
    blob = make_elf({".dram0.bss": 600, ".dram0.data": 400, ".iram0.text": 9999})
    assert dram.static_dram(blob) == 1000


def test_at_the_ceiling_passes_and_one_byte_over_fails():
    ok, _ = dram.judge("nimbus-tft", 1100, CEIL)
    assert ok
    ok, msg = dram.judge("nimbus-tft", 1101, CEIL)
    assert not ok and "OVER by 1 B" in msg


def test_growth_under_the_ceiling_is_reported():
    ok, msg = dram.judge("nimbus-tft", 1096, CEIL)
    assert ok and "+96 B" in msg


def test_unknown_variant_fails():
    ok, msg = dram.judge("new-board", 10, CEIL)
    assert not ok and "no recorded ceiling" in msg


def test_missing_section_or_not_an_elf_fails():
    try:
        dram.static_dram(make_elf({".dram0.bss": 1}))
        raise AssertionError("expected ValueError")
    except ValueError as e:
        assert ".dram0.data" in str(e)
    for bad in (b"", b"MZ" + bytes(60), b"\x7fELF\x02\x01" + bytes(60)):
        try:
            dram.static_dram(bad)
            raise AssertionError("expected ValueError")
        except ValueError:
            pass


def test_cli_checks_one_file(tmp_path):
    elf = tmp_path / "firmware.elf"
    elf.write_bytes(make_elf({".dram0.bss": 700, ".dram0.data": 500}))
    ceil = tmp_path / "ceil.json"
    ceil.write_text('{"variants": {"nimbus-tft": {"env": "x", "baseline": 1000, "ceiling": 1100}}}')
    assert dram.main(["--elf", str(elf), "--variant", "nimbus-tft", "--ceilings", str(ceil)]) == 1
    elf.write_bytes(make_elf({".dram0.bss": 600, ".dram0.data": 500}))
    assert dram.main(["--elf", str(elf), "--variant", "nimbus-tft", "--ceilings", str(ceil)]) == 0
    assert dram.main(["--elf", str(elf), "--ceilings", str(ceil)]) == 1  # --variant missing


def test_every_shipped_variant_has_a_bounded_ceiling():
    """The class rule: every variant release.yml ships has a recorded baseline and a
    ceiling no more than 2 KB above it; a new shipped variant with none fails here."""
    with open(os.path.join(REPO, ".github", "workflows", "release.yml"), encoding="utf-8") as f:
        yml = f.read()
    loop = re.search(r"for pair in ([^;]+);", yml)
    assert loop, "release.yml variant loop not found"
    shipped = {p.split(":")[0]: p.split(":")[1] for p in loop.group(1).split()}
    ceilings = dram.load_ceilings()
    assert set(shipped) == set(ceilings)
    for variant, env in shipped.items():
        row = ceilings[variant]
        assert row["env"] == env
        assert row["baseline"] <= row["ceiling"] <= row["baseline"] + 2048


def test_release_and_gate_run_it():
    with open(os.path.join(REPO, ".github", "workflows", "release.yml"), encoding="utf-8") as f:
        assert "check_static_dram.py" in f.read()
    with open(os.path.join(HERE, "run_gate.py"), encoding="utf-8") as f:
        assert "check_static_dram.main" in f.read()
