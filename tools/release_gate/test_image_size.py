"""Host tests for the image-size gate (check_image_size.py).

Run: python3 -m pytest tools/release_gate

The last test runs the gate against the REAL tree's built images when they
exist (the battery builds esp32s3/esp32s3-cyd/test/test-cyd before the
citools leg, so there it gates for real and fails on an overflowing image,
e.g. test-cyd on ce65c154). In CI's gate-logic job there are no PlatformIO
builds, so that leg skips with a printed reason - never a silent pass.
"""

import glob
import json
import os
import shutil
import struct
import subprocess

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


# --- per-lane image allowance (lane RB-GATE0) ---------------------------------------
#
# The allowance leg measures each built image as code bytes plus docs pack bytes. The
# docs pack is read from the ELF (the pack's tables and every string they point at), so
# these tests build a small ELF32 with a symbol table and a rodata section holding a
# fake pack, write a firmware.bin of a chosen size beside it, and move code and docs
# bytes independently. Each counter-test is a change that must turn the leg red.

RODATA_ADDR = 0x3C000000
TABLES = [
    {"symbol": "_ZN6nimbus4docs13kDocsSectionsE", "entry_bytes": 12, "string_offsets": [0, 4, 8]},
    {"symbol": "_ZN6nimbus4docs10kDocsFilesE", "entry_bytes": 12, "string_offsets": [0, 4]},
]


def fake_elf(section_bodies, file_entries=(("slug", "Title"),), with_symbols=True):
    """ELF32 LE: .flash.rodata holding the pack's strings and both tables, .symtab, .strtab, .shstrtab."""
    rodata = bytearray()
    addr = {}

    def put_str(s):
        if s not in addr:  # identical literals are merged, as the linker does
            addr[s] = RODATA_ADDR + len(rodata)
            rodata.extend(s.encode() + b"\x00")
        return addr[s]

    rows = [(put_str(i), put_str(t), put_str(b)) for i, t, b in section_bodies]
    files = [(put_str(s), put_str(t)) for s, t in file_entries]
    while len(rodata) % 4:
        rodata.append(0)
    sec_off = len(rodata)
    for row in rows:
        rodata.extend(struct.pack("<III", *row))
    files_off = len(rodata)
    for slug, title in files:
        rodata.extend(struct.pack("<IIHH", slug, title, 0, 1))
    strtab = b"\x00" + b"\x00".join(t["symbol"].encode() for t in TABLES) + b"\x00"
    symtab = bytearray(16)  # the null symbol
    if with_symbols:
        name1 = 1
        name2 = 1 + len(TABLES[0]["symbol"]) + 1
        symtab += struct.pack("<IIIBBH", name1, RODATA_ADDR + sec_off, 12 * len(rows), 0x11, 0, 1)
        symtab += struct.pack("<IIIBBH", name2, RODATA_ADDR + files_off, 12 * len(files), 0x11, 0, 1)
    shstr = b"\x00.flash.rodata\x00.symtab\x00.strtab\x00.shstrtab\x00"
    body = bytes(rodata)
    offs = {}
    blob = bytearray(52)
    for name, data in (("ro", body), ("sym", bytes(symtab)), ("str", strtab), ("shstr", shstr)):
        offs[name] = len(blob)
        blob.extend(data)
    shoff = len(blob)
    sh = [
        (0, 0, 0, 0, 0, 0, 0, 0, 0, 0),
        (1, 1, 2, RODATA_ADDR, offs["ro"], len(body), 0, 0, 4, 0),
        (15, 2, 0, 0, offs["sym"], len(symtab), 3, 1, 4, 16),
        (23, 3, 0, 0, offs["str"], len(strtab), 0, 0, 1, 0),
        (31, 3, 0, 0, offs["shstr"], len(shstr), 0, 0, 1, 0),
    ]
    for row in sh:
        blob.extend(struct.pack("<10I", *row))
    struct.pack_into("<4sBB", blob, 0, b"\x7fELF", 1, 1)
    struct.pack_into("<I", blob, 0x20, shoff)
    struct.pack_into("<HHH", blob, 0x2E, 40, len(sh), 4)
    return bytes(blob)


def pack_bytes(section_bodies, file_entries=(("slug", "Title"),)):
    strings = {s for row in section_bodies for s in row} | {s for row in file_entries for s in row}
    return 12 * len(section_bodies) + 12 * len(file_entries) + sum(len(s) + 1 for s in strings)


DOCS = [("a#one", "One", "x" * 300), ("a#two", "Two", "y" * 500)]


def build_image(root, env, image_bytes, sections=DOCS):
    d = os.path.join(root, ".pio", "build", env)
    os.makedirs(d, exist_ok=True)
    with open(os.path.join(d, "firmware.bin"), "wb") as f:
        f.write(b"\xe9" + b"\x00" * (image_bytes - 1))
    with open(os.path.join(d, "firmware.elf"), "wb") as f:
        f.write(fake_elf(sections))


def allowance_root(tmp_path, budgets=None, base_image=6_000_000, base_docs=DOCS):
    """A fake repo with a recorded baseline for esp32s3-cyd and the given budget files."""
    root, fw = make_root(tmp_path, ini=INI + "\n[env:esp32s3-cyd]\nextends = s3\n", images={})
    build_image(root, "esp32s3-cyd", base_image, base_docs)
    baseline = os.path.join(root, "image_baseline.json")
    gate.record_baseline(os.path.join(root, ".pio", "build"), "a" * 40, baseline, TABLES)
    budget_dir = os.path.join(root, "image_budget.d")
    os.makedirs(budget_dir)
    for lane, row in (budgets or {}).items():
        with open(os.path.join(budget_dir, f"{lane}.json"), "w", encoding="utf-8") as f:
            json.dump(row, f)
    return root, baseline, budget_dir


def allowance(root, baseline, budget_dir, lane, build_id=None):
    build_dir = os.path.join(root, ".pio", "build")
    return gate.check_allowance(build_dir, lane, baseline, budget_dir, build_id=build_id)


def budget(lane, n, why="measured from the map delta", **envs):
    row = {"lane": lane, "allowance_bytes": n, "why": why}
    if envs:
        row["envs"] = envs
    return row


def test_docs_pack_is_read_from_the_elf_tables_and_strings():
    blob = fake_elf(DOCS)
    n, notes = gate.docs_pack_bytes(blob, TABLES)
    assert n == pack_bytes(DOCS) and notes == []


def test_merged_identical_strings_count_once():
    rows = [("a#one", "Same", "body"), ("a#two", "Same", "body")]
    n, _ = gate.docs_pack_bytes(fake_elf(rows), TABLES)
    assert n == pack_bytes(rows)


def test_missing_pack_symbol_counts_zero_with_a_note():
    n, notes = gate.docs_pack_bytes(fake_elf(DOCS, with_symbols=False), TABLES)
    assert n == 0 and any("not in the image" in s for s in notes)


def test_growth_inside_the_allowance_passes(tmp_path):
    root, baseline, bdir = allowance_root(tmp_path, {"RB-X": budget("RB-X", 6144)})
    build_image(root, "esp32s3-cyd", 6_000_000 + 6144)
    ok, lines, fails = allowance(root, baseline, bdir, "RB-X")
    assert ok, fails
    assert any("+6144 B" in ln and "allowance 6144 B" in ln for ln in lines)


def test_counter_one_byte_over_the_allowance_fails(tmp_path):
    root, baseline, bdir = allowance_root(tmp_path, {"RB-X": budget("RB-X", 6144)})
    build_image(root, "esp32s3-cyd", 6_000_000 + 6145)
    ok, _, fails = allowance(root, baseline, bdir, "RB-X")
    assert not ok
    assert any("esp32s3-cyd" in f and "1 B over" in f for f in fails)


def test_counter_no_budget_file_means_zero_allowance(tmp_path):
    root, baseline, bdir = allowance_root(tmp_path)
    build_image(root, "esp32s3-cyd", 6_000_001)
    ok, lines, fails = allowance(root, baseline, bdir, "RB-Y")
    assert not ok
    assert any("no budget file" in ln for ln in lines)


def test_docs_pack_growth_is_counted_apart(tmp_path):
    # 4 KB more docs and the image grows by exactly that: the code delta is 0.
    bigger = DOCS + [("a#three", "Three", "z" * 4000)]
    grow = pack_bytes(bigger) - pack_bytes(DOCS)
    root, baseline, bdir = allowance_root(tmp_path)
    build_image(root, "esp32s3-cyd", 6_000_000 + grow, bigger)
    ok, lines, fails = allowance(root, baseline, bdir, "RB-Y")
    assert ok, fails
    assert any(f"docs pack {pack_bytes(bigger)} B (+{grow} B, counted apart)" in ln for ln in lines)


def test_counter_code_hidden_behind_a_docs_cut_still_fails(tmp_path):
    # Docs shrink by 800 B while the image stays the same size: 800 B of code grew.
    smaller = [("a#one", "One", "x" * 300)]
    root, baseline, bdir = allowance_root(tmp_path)
    build_image(root, "esp32s3-cyd", 6_000_000, smaller)
    ok, _, fails = allowance(root, baseline, bdir, "RB-Y")
    assert not ok
    assert any(f"+{pack_bytes(DOCS) - pack_bytes(smaller)} B" in f for f in fails)


def test_counter_a_promised_deletion_not_delivered_fails(tmp_path):
    root, baseline, bdir = allowance_root(tmp_path, {"RB-Q1b": budget("RB-Q1b", -100000)})
    build_image(root, "esp32s3-cyd", 6_000_000 - 60000)
    ok, _, fails = allowance(root, baseline, bdir, "RB-Q1b")
    assert not ok
    assert any("40000 B over" in f for f in fails)


def test_per_env_allowance_overrides_the_lane_default(tmp_path):
    rows = {"RB-X": budget("RB-X", 0, **{"esp32s3-cyd": 2048})}
    root, baseline, bdir = allowance_root(tmp_path, rows)
    build_image(root, "esp32s3-cyd", 6_000_000 + 2048)
    ok, _, fails = allowance(root, baseline, bdir, "RB-X")
    assert ok, fails


@pytest.mark.parametrize(
    "row",
    [
        {"lane": "RB-OTHER", "allowance_bytes": 10, "why": "w"},  # names another lane
        {"lane": "RB-X", "allowance_bytes": "10", "why": "w"},  # not an integer
        {"lane": "RB-X", "allowance_bytes": 10},  # no why
        {"lane": "RB-X", "allowance_bytes": 10, "why": "w", "envs": {"esp32s3-cyd": 1.5}},
    ],
)
def test_counter_malformed_budget_file_fails(tmp_path, row):
    root, baseline, bdir = allowance_root(tmp_path, {"RB-X": row})
    build_image(root, "esp32s3-cyd", 6_000_000)
    ok, _, fails = allowance(root, baseline, bdir, "RB-X")
    assert not ok
    assert any("RB-X.json" in f for f in fails)


def test_counter_image_from_another_build_fails(tmp_path):
    # A stale .pio directory: the bin does not carry this commit's build id.
    root, baseline, bdir = allowance_root(tmp_path)
    ok, _, fails = allowance(root, baseline, bdir, "RB-Y", build_id="v5.1.1-99-gabcdef12")
    assert not ok
    assert any("not this commit's build" in f for f in fails)


def git_out(root, *args):
    return subprocess.run(["git", "-C", root, *args], check=True, capture_output=True, text=True).stdout.strip()


def commit_file(root, rel, text):
    p = os.path.join(root, rel)
    os.makedirs(os.path.dirname(p), exist_ok=True)
    with open(p, "w", encoding="utf-8") as f:
        f.write(text)
    git_out(root, "add", "-A")
    git_out(root, "commit", "-q", "-m", rel)
    return git_out(root, "rev-parse", "HEAD")


def baseline_repo(tmp_path):
    """rb-main with a recorded baseline commit; returns (root, recorded sha)."""
    root = str(tmp_path / "g")
    os.makedirs(root)
    git_out(root, "init", "-q", "-b", "rb-main")
    git_out(root, "config", "user.email", "t@example.invalid")
    git_out(root, "config", "user.name", "t")
    sha = commit_file(root, "src/main.cpp", "int main() {}\n")
    commit_file(root, "tools/release_gate/image_baseline.json", "{}\n")  # the record commit
    return root, sha


def test_fresh_baseline_has_no_problems(tmp_path):
    root, sha = baseline_repo(tmp_path)
    commit_file(root, "docs/a.md", "docs\n")  # docs only: the baseline stays current
    git_out(root, "checkout", "-q", "-b", "lane/RB-X")
    commit_file(root, "src/main.cpp", "int main() { return 1; }\n")  # the lane's own change
    assert gate.git_baseline_problems(root, "rb-main")(sha) == []


def test_counter_baseline_not_re_recorded_after_a_merge_fails(tmp_path):
    # Another lane merged firmware code after the record: its bytes would count for this lane.
    root, sha = baseline_repo(tmp_path)
    commit_file(root, "lib/core/src/orch_x.cpp", "int x;\n")
    git_out(root, "checkout", "-q", "-b", "lane/RB-X")
    problems = gate.git_baseline_problems(root, "rb-main")(sha)
    assert problems and "stale" in problems[0] and "lib/core/src/orch_x.cpp" in problems[0]


def test_counter_baseline_from_another_line_fails(tmp_path):
    root, _ = baseline_repo(tmp_path)
    git_out(root, "checkout", "-q", "--orphan", "other")
    other = commit_file(root, "src/main.cpp", "other\n")
    git_out(root, "checkout", "-q", "rb-main")
    problems = gate.git_baseline_problems(root, "rb-main")(other)
    assert problems and "not an ancestor" in problems[0]


def test_counter_nothing_built_fails(tmp_path):
    root, baseline, bdir = allowance_root(tmp_path)
    shutil.rmtree(os.path.join(root, ".pio", "build", "esp32s3-cyd"))
    ok, _, fails = allowance(root, baseline, bdir, "RB-Y")
    assert not ok
    assert any("no image with a baseline row" in f for f in fails)


def test_counter_bin_without_its_elf_fails(tmp_path):
    root, baseline, bdir = allowance_root(tmp_path)
    os.remove(os.path.join(root, ".pio", "build", "esp32s3-cyd", "firmware.elf"))
    ok, _, fails = allowance(root, baseline, bdir, "RB-Y")
    assert not ok
    assert any("firmware.elf" in f for f in fails)


def test_record_baseline_writes_code_plus_docs(tmp_path):
    root, baseline, _ = allowance_root(tmp_path)
    with open(baseline, encoding="utf-8") as f:
        row = json.load(f)
    env = row["envs"]["esp32s3-cyd"]
    assert row["sha"] == "a" * 40
    assert env["image_bytes"] == 6_000_000
    assert env["docs_pack_bytes"] == pack_bytes(DOCS)
    assert env["code_bytes"] == env["image_bytes"] - env["docs_pack_bytes"]


def test_main_lane_mode_exit_codes(tmp_path, capsys):
    root, baseline, bdir = allowance_root(tmp_path, {"RB-X": budget("RB-X", 100)})
    fw = str(tmp_path / "framework")
    args = ["--root", root, "--framework-partitions", fw, "--baseline", baseline, "--budget-dir", bdir]
    build_image(root, "esp32s3-cyd", 6_000_000 + 100)
    assert gate.main([*args, "--lane", "RB-X", "--no-ancestry"]) == 0
    assert "IMAGE ALLOWANCE GREEN RB-X" in capsys.readouterr().out
    build_image(root, "esp32s3-cyd", 6_000_000 + 101)
    assert gate.main([*args, "--lane", "RB-X", "--no-ancestry"]) == 1
    assert "IMAGE ALLOWANCE RED RB-X" in capsys.readouterr().out


def test_real_tree_baseline_and_budget_files_are_well_formed():
    """The committed baseline is consistent and every budget file in image_budget.d parses."""
    with open(os.path.join(HERE, "image_baseline.json"), encoding="utf-8") as f:
        row = json.load(f)
    assert len(row["sha"]) == 40 and row["envs"]
    for env, e in row["envs"].items():
        assert e["code_bytes"] == e["image_bytes"] - e["docs_pack_bytes"], env
        assert e["docs_pack_bytes"] > 0, env
    bdir = os.path.join(HERE, "image_budget.d")
    for name in sorted(os.listdir(bdir)):
        if name.endswith(".json"):
            lane = name[: -len(".json")]
            _, errs = gate.load_budget(bdir, lane)
            assert errs == [], errs
