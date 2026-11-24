"""Host tests for the image projection tool (image_projection.py) on small fixture maps.

Run: python3 -m pytest tools/release_gate

The fixtures are the shapes a real ESP32-S3 firmware.map has: input sections on one line or
with the address on the next line, sections at address 0 (dropped by --gc-sections), fill,
archive members, a library folder with a space in its name, a merged string blob listed on
its first object with the others' string sections inside it, and the /DISCARD/ and cross
reference parts after the memory map.
"""

import json
import os

import pytest

import image_projection as proj

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))

HEADER = """Archive member included to satisfy reference by file (symbol)

.pio/build/{env}/libFrameworkArduino.a(Esp.cpp.o)
                              (ignored before the memory map)
 .text.notcounted 0x42000000 0x999 .pio/build/{env}/src/main.cpp.o

Linker script and memory map

"""

FOOTER = """
/DISCARD/
 .text.gone     0x42009000      0x500 .pio/build/{env}/src/main.cpp.o
OUTPUT(.pio/build/{env}/firmware.elf elf32-xtensa-le)

Cross Reference Table

Symbol                                            File
main                                              .pio/build/{env}/src/main.cpp.o
"""


def sections(env, objs):
    """Map text for objs: {object path: [(output section, input name, addr, size)]}."""
    by_out = {}
    for path, rows in objs.items():
        for out, name, addr, size in rows:
            by_out.setdefault(out, []).append((addr, name, size, path))
    text = HEADER.format(env=env)
    for out in (".iram0.text", ".dram0.data", ".dram0.bss", ".flash.text", ".flash.rodata"):
        text += f"{out}     0x40000000   0x100000\n *(.literal .text)\n"
        for addr, name, size, path in sorted(by_out.get(out, [])):
            full = path if path.startswith("/") else f".pio/build/{env}/{path}"
            if len(name) > 14:  # the linker wraps a long section name: address on the next line
                text += f" {name}\n                {addr:#010x} {size:#10x} {full}\n"
            else:
                text += f" {name:<14} {addr:#010x} {size:#10x} {full}\n"
        text += " *fill*         0x40000000        0x3 \n"
    return text + FOOTER.format(env=env)


ENGINE1 = {
    "src/main.cpp.o": [(".flash.text", ".text.main", 0x42000100, 1000), (".dram0.data", ".data.x", 0x3FC90000, 16)],
    "lib19d/harness/engine.cpp.o": [
        (".flash.text", ".text._ZN5agent6engine3runEv", 0x42001000, 5000),
        (".flash.rodata", ".rodata._ZN5agent6engine3kTE", 0x3C001000, 700),
        (".dram0.bss", ".bss.state", 0x3FCA0000, 64),
    ],
    "lib19d/harness/jobs.cpp.o": [(".flash.text", ".text.jobs", 0x42003000, 3000)],
    "lib0f9/core/orch_pdf_writer.cpp.o": [(".flash.text", ".text.pdf", 0x42004000, 900)],
    "/abs/framework/lib/libspi_flash.a(spi_flash_wrap.c.obj)": [(".iram0.text", ".iram1.wrap", 0x40378000, 200)],
    "lib220/Adafruit NeoPixel/esp.c.o": [(".flash.rodata", ".rodata.espShow.str1.1", 0x3C000000, 400)],
    # discarded by --gc-sections: address 0, never counted
    "lib19d/harness/dead.cpp.o": [(".flash.text", ".text.dead", 0x0, 4000)],
}

ENGINE2 = {
    "src/main.cpp.o": [(".flash.text", ".text.main", 0x42000100, 1100), (".dram0.data", ".data.x", 0x3FC90000, 16)],
    "lib19d/harness/plain_engine.cpp.o": [(".flash.text", ".text.plain", 0x42001000, 4000)],
    "lib19d/harness/jobs.cpp.o": [(".flash.text", ".text.jobs", 0x42003000, 500)],
    "/abs/framework/lib/libspi_flash.a(spi_flash_wrap.c.obj)": [(".iram0.text", ".iram1.wrap", 0x40378000, 200)],
    "lib220/Adafruit NeoPixel/esp.c.o": [(".flash.rodata", ".rodata.espShow.str1.1", 0x3C000000, 300)],
}

GROUPS = {
    "groups": [
        {"name": "sub-agents", "returns": "yes", "patterns": ["^harness/jobs\\.cpp\\.o$"]},
        {"name": "pdf", "returns": "yes", "patterns": ["^core/orch_pdf_writer\\.cpp\\.o$"]},
        {"name": "old engine", "returns": "no", "patterns": ["^harness/engine\\.cpp\\.o$"]},
        {"name": "strings", "returns": "partly", "apportion": True, "patterns": ["^Adafruit NeoPixel/esp\\.c\\.o$"]},
    ]
}


@pytest.fixture()
def maps(tmp_path):
    a = tmp_path / "a.map"
    b = tmp_path / "b.map"
    a.write_text(sections("esp32s3-cyd", ENGINE1), encoding="utf-8")
    b.write_text(sections("esp32s3-cyd-e2", ENGINE2), encoding="utf-8")
    g = tmp_path / "groups.json"
    g.write_text(json.dumps(GROUPS), encoding="utf-8")
    return str(a), str(b), str(g)


def test_object_names_drop_the_build_folder_so_two_envs_match():
    assert proj.object_name(".pio/build/esp32s3-cyd/lib19d/harness/x.cpp.o") == "harness/x.cpp.o"
    assert proj.object_name(".pio/build/esp32s3-cyd-e2/src/agent/y.cpp.o") == "src/agent/y.cpp.o"
    assert proj.object_name("/p/q/libspi_flash.a(spi_flash_wrap.c.obj)") == "libspi_flash.a(spi_flash_wrap.c.obj)"
    assert proj.object_name(".pio/build/e/libFrameworkArduino.a(Esp.cpp.o)") == "libFrameworkArduino.a(Esp.cpp.o)"
    assert proj.object_name(".pio/build/e/lib220/Adafruit NeoPixel/esp.c.o") == "Adafruit NeoPixel/esp.c.o"


def test_bytes_per_object_and_kind(maps):
    a = proj.read_map(maps[0])
    eng = a["harness/engine.cpp.o"]
    assert (eng["text"], eng["rodata"], eng["bss"]) == (5000, 700, 64)  # the wrapped lines are read too
    assert a["src/main.cpp.o"]["text"] == 1000 and a["src/main.cpp.o"]["data"] == 16
    assert a["libspi_flash.a(spi_flash_wrap.c.obj)"]["text"] == 200
    assert a["Adafruit NeoPixel/esp.c.o"]["strings"] == 400
    assert proj.flash(eng) == 5700  # bss is not in the image


def test_counter_discarded_lines_and_text_outside_the_memory_map_are_not_counted(maps):
    a = proj.read_map(maps[0])
    assert "harness/dead.cpp.o" not in a  # address 0: dropped by the linker
    assert a["src/main.cpp.o"]["text"] == 1000  # not the 0x999 before the map, not the /DISCARD/ 0x500


def test_counter_a_file_without_a_memory_map_fails(tmp_path):
    p = tmp_path / "x.map"
    p.write_text("not a map\n", encoding="utf-8")
    with pytest.raises(ValueError, match="Linker script and memory map"):
        proj.read_map(str(p))


def test_merged_string_blob_is_counted_once():
    # A blob of 1000 B listed on its first object; two other objects' string sections listed
    # inside it at their pre-merge size (600 + 700 > 1000): the image holds 1000 B, not 2300.
    rows = [
        (0x3C000000, 1000, "first.o", "strings"),
        (0x3C000000, 600, "second.o", "strings"),
        (0x3C000100, 700, "third.o", "strings"),
        (0x3C002000, 50, "alone.o", "strings"),
    ]
    out = proj.attribute(rows)
    assert sum(out.values()) == 1050
    assert out[("alone.o", "strings")] == 50
    assert out[("second.o", "strings")] + out[("third.o", "strings")] <= 1000


def test_non_string_sections_inside_a_blob_keep_their_size():
    rows = [
        (0x3C000000, 1000, "first.o", "strings"),
        (0x3C000010, 300, "table.o", "rodata"),
        (0x3C000020, 900, "second.o", "strings"),
    ]
    out = proj.attribute(rows)
    assert out[("table.o", "rodata")] == 300
    assert sum(out.values()) == 1000


def test_missing_features_by_group_and_the_projection(maps):
    a, b, g = maps
    groups = proj.load_groups(g)
    cmp = proj.compare(proj.read_map(a), proj.read_map(b))
    missing = proj.missing_by_group(cmp, groups)
    assert missing["sub-agents"]["bytes"] == 2500  # jobs: 3000 in A, 500 in B
    assert missing["pdf"]["bytes"] == 900  # only in A
    assert missing["old engine"]["bytes"] == 5700  # text and rodata; bss is not in the image
    assert missing["strings"]["bytes"] == 100
    p = proj.projection(missing, groups, 5_000_000, 6_553_600)
    plain = 2500 + 900 + 5700
    assert p["returns_bytes"] == 2500 + 900 + int(100 * (3400 / plain))
    assert p["projected"] == 5_000_000 + p["returns_bytes"]
    assert p["free"] == 6_553_600 - p["projected"]


def test_what_a_lane_added_is_listed_in_b(maps):
    a, b, _ = maps
    cmp = proj.compare(proj.read_map(a), proj.read_map(b))
    assert [r[0] for r in cmp["only_b"]] == ["harness/plain_engine.cpp.o"]
    assert cmp["larger_b"] == [("src/main.cpp.o", 1016, 1116)]


def test_main_prints_the_tables_and_writes_json(maps, tmp_path, capsys):
    a, b, g = maps
    binf = tmp_path / "firmware.bin"
    binf.write_bytes(b"\xe9" * 4096)
    out_json = tmp_path / "p.json"
    rc = proj.main([a, b, "--groups", g, "--bin", str(binf), "--slot", "10000", "--json", str(out_json)])
    out = capsys.readouterr().out
    assert rc == 0
    assert "| sub-agents | yes | 2,500 | 1 |" in out
    assert "Projected B image: 4,096 B" in out
    data = json.loads(out_json.read_text(encoding="utf-8"))
    assert data["projection"]["image_b"] == 4096
    assert data["missing_by_group"]["pdf"] == {"bytes": 900, "returns": "yes"}


def test_main_one_group_and_unknown_group(maps, capsys):
    a, b, g = maps
    assert proj.main([a, b, "--groups", g, "--group", "pdf"]) == 0
    out = capsys.readouterr().out
    assert "core/orch_pdf_writer.cpp.o" in out and "harness/jobs.cpp.o | sub-agents" not in out
    assert proj.main([a, b, "--groups", g, "--group", "nope"]) == 2


@pytest.mark.parametrize(
    "row",
    [
        {"name": "x", "returns": "maybe", "patterns": ["x"]},
        {"name": "x", "returns": "yes", "patterns": []},
        {"name": "x", "returns": "yes", "apportion": "yes", "patterns": ["x"]},
    ],
)
def test_counter_malformed_group_file_fails(tmp_path, row, maps, capsys):
    g = tmp_path / "bad.json"
    g.write_text(json.dumps({"groups": [row]}), encoding="utf-8")
    assert proj.main([maps[0], maps[1], "--groups", str(g)]) == 1
    assert "FAIL" in capsys.readouterr().out


def test_real_group_file_is_well_formed_and_names_are_unique():
    groups = proj.load_groups()
    names = [g["name"] for g in groups]
    assert len(names) == len(set(names)) and "other" not in names
    for g in groups:
        assert g.get("why"), g["name"]


def test_real_maps_when_built(capsys):
    """With both Freenove images built, the tool reads them and the ELF agrees with its totals."""
    build = os.path.join(REPO, ".pio", "build")
    a = os.path.join(build, "esp32s3-cyd", "firmware.map")
    b = os.path.join(build, "esp32s3-cyd-e2", "firmware.map")
    if not (os.path.exists(a) and os.path.exists(b)):
        pytest.skip("esp32s3-cyd and esp32s3-cyd-e2 are not built here (CI gate-logic has no PlatformIO builds)")
    assert proj.main([a, b, "--bin", os.path.join(build, "esp32s3-cyd-e2", "firmware.bin"), "--top", "5"]) == 0
    out = capsys.readouterr().out
    assert "Projected B image" in out
