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
    # A test env (no release floor): only the 64 KB line applies.
    root, fw = make_root(tmp_path, images={"test": BIG_SLOT - 1000})
    ok, _, fails, warns = run_gate(root, fw)
    assert ok and not fails
    assert any("headroom 1000 B" in w for w in warns)


def test_comfortable_headroom_passes_quietly(tmp_path):
    root, fw = make_root(tmp_path, images={"esp32s3": SLOT - gate.RELEASE_WARN_BYTES})
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


def build_image(root, env, image_bytes, sections=DOCS, stamp=b""):
    """firmware.bin of image_bytes and its ELF; stamp is the build id string both carry."""
    d = os.path.join(root, ".pio", "build", env)
    os.makedirs(d, exist_ok=True)
    with open(os.path.join(d, "firmware.bin"), "wb") as f:
        f.write(b"\xe9" + stamp + b"\x00" * (image_bytes - 1 - len(stamp)))
    with open(os.path.join(d, "firmware.elf"), "wb") as f:
        f.write(fake_elf(sections) + stamp)


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
        {"lane": "RB-X", "allowance_bytes": 10, "why": "w", "envs": [1, 2]},  # not a map
        {"lane": "RB-X", "allowance_bytes": 10, "why": "w", "envs": 5},
    ],
)
def test_counter_malformed_budget_file_fails(tmp_path, row):
    root, baseline, bdir = allowance_root(tmp_path, {"RB-X": row})
    build_image(root, "esp32s3-cyd", 6_000_000)
    ok, _, fails = allowance(root, baseline, bdir, "RB-X")
    assert not ok
    assert any("RB-X.json" in f for f in fails)


ID = "v5.1.1-99-gabcdef12"


def test_image_carrying_this_commits_build_id_passes(tmp_path):
    root, baseline, bdir = allowance_root(tmp_path)
    build_image(root, "esp32s3-cyd", 6_000_000, stamp=ID.encode() + b"\x00")
    ok, _, fails = allowance(root, baseline, bdir, "RB-Y", build_id=ID)
    assert ok, fails


@pytest.mark.parametrize(
    "stamp",
    [
        b"",  # a stale .pio directory: no id at all
        b"v5.1.1-98-g12345678\x00",  # another commit's build
        ID.encode() + b"-dirty\x00",  # this commit plus uncommitted changes
    ],
)
def test_counter_image_from_another_build_fails(tmp_path, stamp):
    root, baseline, bdir = allowance_root(tmp_path)
    build_image(root, "esp32s3-cyd", 6_000_000, stamp=stamp)
    ok, _, fails = allowance(root, baseline, bdir, "RB-Y", build_id=ID)
    assert not ok
    assert any("not this commit's clean build" in f for f in fails)


def test_counter_stale_elf_beside_a_fresh_bin_fails(tmp_path):
    root, baseline, bdir = allowance_root(tmp_path)
    build_image(root, "esp32s3-cyd", 6_000_000, stamp=ID.encode() + b"\x00")
    with open(os.path.join(root, ".pio", "build", "esp32s3-cyd", "firmware.elf"), "wb") as f:
        f.write(fake_elf(DOCS))  # an older link: no id
    ok, _, fails = allowance(root, baseline, bdir, "RB-Y", build_id=ID)
    assert not ok
    assert any("firmware.elf is not this commit's" in f for f in fails)


def test_counter_unterminated_docs_string_is_an_error_not_a_size():
    sections = [{"name": 0, "type": 1, "flags": 2, "addr": 0x1000, "offset": 0, "size": 3, "link": 0}]
    with pytest.raises(ValueError, match="no terminating NUL"):
        gate.string_bytes(b"abc", sections, 0x1000, set())


def test_counter_record_refuses_to_drop_a_baseline_env(tmp_path, capsys):
    root, baseline, _ = allowance_root(tmp_path)
    shutil.rmtree(os.path.join(root, ".pio", "build", "esp32s3-cyd"))
    build_image(root, "esp32s3", 6_000_000)
    git_out(root, "init", "-q")
    rc = gate.main(["--root", root, "--baseline", baseline, "--record-baseline"])
    assert rc == 1
    assert "never drops an env" in capsys.readouterr().out


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
    assert any("esp32s3-cyd has a baseline row but is not built" in f for f in fails)


def test_counter_a_partial_build_fails(tmp_path):
    # Two envs in the baseline, the lane built one: the other's growth would be invisible.
    root, baseline, bdir = allowance_root(tmp_path)
    build_image(root, "test-cyd", 6_100_000)
    gate.record_baseline(os.path.join(root, ".pio", "build"), "a" * 40, baseline, TABLES)
    shutil.rmtree(os.path.join(root, ".pio", "build", "test-cyd"))
    ok, _, fails = allowance(root, baseline, bdir, "RB-Y")
    assert not ok
    assert any("test-cyd has a baseline row but is not built" in f for f in fails)


def test_an_env_without_a_baseline_row_is_named_not_gated(tmp_path):
    root, baseline, bdir = allowance_root(tmp_path)
    build_image(root, "esp32s3-cyd-e2", 5_000_000)
    ok, lines, fails = allowance(root, baseline, bdir, "RB-Y")
    assert ok, fails
    assert any("esp32s3-cyd-e2" in ln and "NOT GATED" in ln for ln in lines)


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


# --- the release floor and the engine-2 allowance (lane E2-GATE) -----------------------
#
# A product image keeps RELEASE_FLOOR_BYTES free in its slot (fail) and warns under
# RELEASE_WARN_BYTES; a lane that grows an engine-2 image by more than E2_GRACE_BYTES must
# carry an allowance file. Each rule has its counter-test one byte past the line.

PRODUCT_INI = INI + "".join(f"\n[env:{e}]\nextends = s3\n" for e in gate.PRODUCT_ENVS if e != "esp32s3")


@pytest.mark.parametrize("env", gate.PRODUCT_ENVS)
def test_counter_a_product_image_under_the_release_floor_fails(tmp_path, env):
    root, fw = make_root(tmp_path, ini=PRODUCT_INI, images={env: SLOT - gate.release_floor(env) + 1})
    ok, _, fails, _ = run_gate(root, fw)
    assert not ok
    assert any(env in f and "release floor" in f for f in fails)


def test_a_product_image_at_the_floor_passes_with_a_warning_that_says_the_margin(tmp_path):
    root, fw = make_root(tmp_path, ini=PRODUCT_INI, images={"esp32s3-cyd": SLOT - 213_520})
    ok, lines, fails, warns = run_gate(root, fw)
    assert ok and not fails
    assert any("213520 B free" in w and "63520 B above the release floor of 150000 B" in w for w in warns)
    assert any(ln.startswith("esp32s3-cyd ") and ln.endswith("WARN") for ln in lines)


def test_ruling_126_the_old_engine_floor_is_150000_and_the_new_one_200000(tmp_path):
    # 190,000 B free: inside the engine-1 floor, under the engine-2 one.
    root, fw = make_root(tmp_path, ini=PRODUCT_INI, images={"esp32s3-cyd": SLOT - 190_000})
    ok, _, fails, warns = run_gate(root, fw)
    assert ok and not fails and any("40000 B above the release floor of 150000 B" in w for w in warns)
    root, fw = make_root(tmp_path / "b", ini=PRODUCT_INI, images={"esp32s3-cyd-e2": SLOT - 190_000})
    ok, _, fails, _ = run_gate(root, fw)
    assert not ok and any("under the release floor of 200000 B" in f for f in fails)


def test_the_warn_line_ends_at_300000_free(tmp_path):
    root, fw = make_root(tmp_path, ini=PRODUCT_INI, images={"esp32s3-cyd-e2": SLOT - gate.RELEASE_WARN_BYTES + 1})
    assert run_gate(root, fw)[3]
    root, fw = make_root(tmp_path / "b", ini=PRODUCT_INI, images={"esp32s3-cyd-e2": SLOT - gate.RELEASE_WARN_BYTES})
    assert not run_gate(root, fw)[3]


def test_counter_a_test_image_has_no_release_floor(tmp_path):
    # The test console images ride a larger table and never ship: only the slot and 64 KB rules.
    root, fw = make_root(tmp_path, images={"test": BIG_SLOT - 100_000})
    ok, _, fails, warns = run_gate(root, fw)
    assert ok and not fails and not warns


def test_real_tree_product_envs_and_engine2_names_match_platformio_ini():
    import check_engine_filter

    ini = check_engine_filter.PioIni(os.path.join(REPO, "platformio.ini"))
    devices = set(ini.device_envs())
    assert set(gate.PRODUCT_ENVS) <= devices
    for env in devices:
        assert gate.is_engine2(env) == (ini.engine(env) == 2), env


E2 = "esp32s3-cyd-e2"


def e2_root(tmp_path, budgets=None, required=None):
    """A baseline with an engine-1 and an engine-2 env."""
    root, baseline, bdir = allowance_root(tmp_path, budgets)
    build_image(root, E2, 5_000_000)
    gate.record_baseline(os.path.join(root, ".pio", "build"), "a" * 40, baseline, TABLES, "", required)
    return root, baseline, bdir


def test_an_engine2_lane_without_a_file_may_grow_8_kb(tmp_path):
    root, baseline, bdir = e2_root(tmp_path)
    build_image(root, E2, 5_000_000 + gate.E2_GRACE_BYTES)
    ok, lines, fails = allowance(root, baseline, bdir, "E2-X")
    assert ok, fails
    assert any("no budget file" in ln and "8192 B on an engine-2 env" in ln for ln in lines)


def test_counter_an_engine2_lane_without_a_file_growing_past_8_kb_fails(tmp_path):
    root, baseline, bdir = e2_root(tmp_path)
    build_image(root, E2, 5_000_000 + gate.E2_GRACE_BYTES + 1)
    ok, _, fails = allowance(root, baseline, bdir, "E2-X")
    assert not ok
    assert any(E2 in f and "1 B over" in f and "no image_budget.d/E2-X.json" in f for f in fails)


def test_counter_the_grace_never_reaches_an_engine1_image(tmp_path):
    root, baseline, bdir = e2_root(tmp_path)
    build_image(root, "esp32s3-cyd", 6_000_000 + 1)
    ok, _, fails = allowance(root, baseline, bdir, "E2-X")
    assert not ok and any(f.startswith("esp32s3-cyd:") for f in fails)


def test_counter_a_file_replaces_the_grace(tmp_path):
    # A lane that declares 4 KB is held to 4 KB, even under the no-file grace.
    root, baseline, bdir = e2_root(tmp_path, {"E2-X": budget("E2-X", 4096)})
    build_image(root, E2, 5_000_000 + 6000)
    ok, _, fails = allowance(root, baseline, bdir, "E2-X")
    assert not ok and any(E2 in f and "allowance 4096 B" in f for f in fails)


def test_a_file_names_the_lane_and_the_bytes_for_a_bigger_engine2_change(tmp_path):
    root, baseline, bdir = e2_root(tmp_path, {"E2-X": budget("E2-X", 0, **{E2: 40960})})
    build_image(root, E2, 5_000_000 + 40960)
    ok, _, fails = allowance(root, baseline, bdir, "E2-X")
    assert ok, fails


def test_required_envs_must_be_built_and_the_others_are_judged_when_built(tmp_path):
    root, baseline, bdir = e2_root(tmp_path, required=["esp32s3-cyd"])
    shutil.rmtree(os.path.join(root, ".pio", "build", E2))
    ok, lines, fails = gate.check_allowance(os.path.join(root, ".pio", "build"), "E2-X", baseline, bdir)
    assert ok, fails
    assert any(E2 in ln and "not judged" in ln for ln in lines)
    build_image(root, E2, 5_000_000 + gate.E2_GRACE_BYTES + 1)  # built: judged
    ok, _, fails = gate.check_allowance(os.path.join(root, ".pio", "build"), "E2-X", baseline, bdir)
    assert not ok and any(E2 in f for f in fails)


def test_counter_a_required_env_not_built_fails(tmp_path):
    root, baseline, bdir = e2_root(tmp_path, required=["esp32s3-cyd", E2])
    shutil.rmtree(os.path.join(root, ".pio", "build", E2))
    ok, _, fails = gate.check_allowance(os.path.join(root, ".pio", "build"), "E2-X", baseline, bdir)
    assert not ok and any(f"{E2} has a baseline row but is not built" in f for f in fails)


def test_record_keeps_the_required_envs(tmp_path):
    root, baseline, _ = e2_root(tmp_path, required=["esp32s3-cyd"])
    with open(baseline, encoding="utf-8") as f:
        assert json.load(f)["required_envs"] == ["esp32s3-cyd"]


def test_real_tree_required_envs_are_baseline_envs():
    with open(os.path.join(HERE, "image_baseline.json"), encoding="utf-8") as f:
        row = json.load(f)
    assert set(row["required_envs"]) <= set(row["envs"])
    assert {"esp32s3-cyd", "esp32s3-cyd-e2", "test-cyd-e2"} <= set(row["required_envs"])
    assert set(gate.PRODUCT_ENVS) <= set(row["envs"])


# --- the table and the lane read from the branch ----------------------------------------


def lane_repo(tmp_path, branch="lane/E2-X"):
    """A git repo on rb-main with a recorded baseline at its first commit, then `branch`."""
    root, fw = make_root(tmp_path, ini=PRODUCT_INI, images={})
    git_out(root, "init", "-q", "-b", "rb-main")
    git_out(root, "config", "user.email", "t@example.invalid")
    git_out(root, "config", "user.name", "t")
    with open(os.path.join(root, ".gitignore"), "w", encoding="utf-8") as f:
        f.write(".pio/\n")
    sha = commit_file(root, "src/main.cpp", "int main() {}\n")
    build_image(root, "esp32s3-cyd", 6_000_000)
    build_image(root, E2, 5_000_000)
    baseline = os.path.join(root, "tools", "release_gate", "image_baseline.json")
    os.makedirs(os.path.dirname(baseline))
    gate.record_baseline(os.path.join(root, ".pio", "build"), sha, baseline, TABLES)
    bdir = os.path.join(root, "tools", "release_gate", "image_budget.d")
    os.makedirs(bdir)
    commit_file(root, "tools/release_gate/image_baseline.json", open(baseline, encoding="utf-8").read())
    if branch != "rb-main":
        git_out(root, "checkout", "-q", "-b", branch)
    args = ["--root", root, "--framework-partitions", fw[0], "--baseline", baseline, "--budget-dir", bdir]
    return root, args, bdir


def test_the_table_shows_the_change_and_the_allowance(tmp_path, capsys):
    root, args, _ = lane_repo(tmp_path)
    build_image(root, E2, 5_000_000 + 1234)
    assert gate.main(args) == 0
    out = capsys.readouterr().out
    row = next(ln for ln in out.splitlines() if f"] {E2} " in ln)
    assert "+1234" in row and "E2-X 8192 (no file)" in row and str(SLOT - 5_001_234) in row
    assert "code vs base" in out and "lane E2-X" in out


def test_counter_on_a_lane_branch_growth_past_the_grace_fails_without_a_flag(tmp_path, capsys):
    root, args, _ = lane_repo(tmp_path)
    build_image(root, E2, 5_000_000 + gate.E2_GRACE_BYTES + 1)
    assert gate.main(args) == 1
    assert "no image_budget.d/E2-X.json" in capsys.readouterr().out


def test_on_a_lane_branch_a_stale_baseline_only_warns(tmp_path, capsys):
    root, args, _ = lane_repo(tmp_path, branch="rb-main")
    commit_file(root, "src/other.cpp", "int x;\n")  # firmware merged after the record
    git_out(root, "checkout", "-q", "-b", "lane/E2-X")
    build_image(root, E2, 5_000_000 + gate.E2_GRACE_BYTES + 1)
    assert gate.main(args) == 0
    assert "allowance not judged, the baseline is stale" in capsys.readouterr().out


def test_off_a_lane_branch_the_change_is_reported_with_the_files_since_the_baseline(tmp_path, capsys):
    root, args, bdir = lane_repo(tmp_path, branch="rb-main")
    commit_file(root, "tools/release_gate/image_budget.d/E2-Y.json", json.dumps(budget("E2-Y", 20000)))
    build_image(root, E2, 5_000_000 + 20000)
    build_image(root, "esp32s3-cyd", 6_000_000 + 30000)
    assert gate.main(args) == 0
    out = capsys.readouterr().out
    assert "allowance committed since the baseline: E2-Y (20000 B)" in out
    assert "since base 20000" in out
    assert "esp32s3-cyd: code +30000 B against the baseline, 20000 B explained" in out


def test_counter_on_a_lane_branch_an_unreadable_baseline_fails(tmp_path, capsys):
    root, args, _ = lane_repo(tmp_path)
    with open(args[args.index("--baseline") + 1], "w", encoding="utf-8") as f:
        f.write("{not json")
    assert gate.main(args) == 1
    assert "cannot read the baseline" in capsys.readouterr().out


def test_counter_on_a_lane_branch_a_bin_without_its_elf_fails(tmp_path, capsys):
    # Without the ELF the whole image would be compared, and a docs cut could hide code growth.
    root, args, _ = lane_repo(tmp_path)
    os.remove(os.path.join(root, ".pio", "build", E2, "firmware.elf"))
    assert gate.main(args) == 1
    assert f"{E2}: firmware.elf cannot be read" in capsys.readouterr().out


def test_no_git_is_no_answer_not_a_crash(monkeypatch):
    def boom(*_a, **_k):
        raise FileNotFoundError("git")

    monkeypatch.setattr(gate.subprocess, "run", boom)
    assert gate.git("/nowhere", "status")[0] == 127
    assert gate.infer_lane("/nowhere") is None
