"""Host tests for the static DRAM ceiling gate (check_static_dram.py).

Run: python3 -m pytest tools/release_gate
"""

import json
import os
import struct

import check_static_dram as dram
import merge_static_dram as merge

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
    """The class rule: every variant release.yml ships (the stable rows of
    tools/release/variants.json, which its gate loop reads) has a recorded baseline and a
    ceiling no more than 2 KB above it; a new shipped variant with none fails here."""
    with open(os.path.join(REPO, "tools", "release", "variants.json"), encoding="utf-8") as f:
        rows = json.load(f)["variants"]
    shipped = {r["type"]: r["env"] for r in rows}
    assert shipped, "no stable row in tools/release/variants.json"
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


# ---- the merge-time check (merge_static_dram.py) ---------------------------------------

MERGE_ID = "v5.0.0-3-g1a2b3c4d"
MERGE_CEIL = {
    "nimbus-tft": {"env": "esp32s3", "baseline": 1000, "ceiling": 1100},
    "freenove-28": {"env": "esp32s3-cyd", "baseline": 1000, "ceiling": 1100},
    "freenove-35": {"env": "esp32s3-cyd-35", "baseline": 1000, "ceiling": 1100},
}
MERGE_REQ = ("nimbus-tft", "freenove-28")


def built(tree, env, bss=600, build_id=MERGE_ID):
    d = tree / ".pio" / "build" / env
    d.mkdir(parents=True, exist_ok=True)
    (d / "firmware.elf").write_bytes(make_elf({".dram0.bss": bss, ".dram0.data": 400}) + build_id.encode())


def test_merge_passes_when_both_images_are_this_commits_and_under(tmp_path):
    built(tmp_path, "esp32s3")
    built(tmp_path, "esp32s3-cyd")
    assert merge.check_tree(str(tmp_path), MERGE_REQ, MERGE_CEIL, MERGE_ID)


def test_merge_fails_over_a_ceiling_on_either_image(tmp_path):
    built(tmp_path, "esp32s3")
    built(tmp_path, "esp32s3-cyd", bss=701)  # 1101 B: one over
    assert not merge.check_tree(str(tmp_path), MERGE_REQ, MERGE_CEIL, MERGE_ID)


def test_merge_fails_on_a_missing_required_image(tmp_path):
    built(tmp_path, "esp32s3")
    assert not merge.check_tree(str(tmp_path), MERGE_REQ, MERGE_CEIL, MERGE_ID)


def test_merge_fails_on_a_stale_required_image(tmp_path):
    built(tmp_path, "esp32s3", build_id="v5.0.0-2-g99999999")  # under its ceiling, last commit's
    built(tmp_path, "esp32s3-cyd")
    assert not merge.check_tree(str(tmp_path), MERGE_REQ, MERGE_CEIL, MERGE_ID)


def test_merge_checks_an_optional_image_only_when_this_commit_built_it(tmp_path):
    built(tmp_path, "esp32s3")
    built(tmp_path, "esp32s3-cyd")
    built(tmp_path, "esp32s3-cyd-35", bss=9000, build_id="v5.0.0-2-g99999999")  # stale: not judged
    assert merge.check_tree(str(tmp_path), MERGE_REQ, MERGE_CEIL, MERGE_ID)
    built(tmp_path, "esp32s3-cyd-35", bss=9000)  # this commit's: judged
    assert not merge.check_tree(str(tmp_path), MERGE_REQ, MERGE_CEIL, MERGE_ID)


def test_merge_fails_without_a_build_id_or_on_an_unknown_required_variant(tmp_path):
    built(tmp_path, "esp32s3")
    built(tmp_path, "esp32s3-cyd")
    assert not merge.check_tree(str(tmp_path), MERGE_REQ, MERGE_CEIL, None)
    assert not merge.check_tree(str(tmp_path), MERGE_REQ + ("new-board",), MERGE_CEIL, MERGE_ID)


def test_merge_cli_says_its_marker_and_exit_code(tmp_path, capsys):
    ceil = tmp_path / "ceil.json"
    ceil.write_text('{"variants": {"nimbus-tft": {"env": "esp32s3", "baseline": 1000, "ceiling": 1100}}}')
    # The temporary directory is no git checkout: no build id, so red.
    assert merge.main(["--tree", str(tmp_path), "--require", "nimbus-tft", "--ceilings", str(ceil)]) == 1
    assert capsys.readouterr().out.strip().endswith("STATIC DRAM RED")


def test_merge_required_default_is_what_the_battery_builds():
    ceilings = dram.load_ceilings()
    assert {ceilings[v]["env"] for v in merge.DEFAULT_REQUIRED} == {"esp32s3", "esp32s3-cyd"}


# The baseline is the last measured tree, so a report's "+N B" is growth since then and not
# since v4.5.15 (the release line and main merged, lane MB: 91,195 B and 91,275 B). The
# ceilings did not move: moving one needs the bench heap A/B.
MEASURED = {"nimbus-tft": 91195, "freenove-28": 91275, "freenove-35": 91275, "freenove-40": 91275}
UNCHANGED_CEILINGS = {"nimbus-tft": 91635, "freenove-28": 91723, "freenove-35": 91723, "freenove-40": 91723}


def test_the_baseline_is_the_measured_merge_and_the_ceilings_did_not_move():
    variants = json.load(open(dram.CEILINGS))["variants"]
    for name, row in variants.items():
        assert row["baseline"] == MEASURED[name], name
        assert row["ceiling"] == UNCHANGED_CEILINGS[name], f"{name}: a raised ceiling needs the bench heap A/B"
        assert row["baseline"] <= row["ceiling"], name


# ---- declared allowances for the engine-2 rows (lane E2-GATE, ruling 121) ----------------

E2_CEIL = {
    "variants": {"nimbus-tft": {"env": "esp32s3", "baseline": 1000, "ceiling": 1100}},
    "engine2_baseline_sha": "a" * 40,
    "engine2": {"freenove-28-e2": {"env": "esp32s3-cyd-e2", "baseline": 1000, "ceiling": 3048}},
}


def e2_case(tmp_path, bss, budget=None):
    elf = tmp_path / "firmware.elf"
    elf.write_bytes(make_elf({".dram0.bss": bss, ".dram0.data": 400}))
    ceil = tmp_path / "ceil.json"
    ceil.write_text(json.dumps(E2_CEIL))
    bdir = tmp_path / "budget.d"
    bdir.mkdir(exist_ok=True)
    if budget is not None:
        (bdir / "E2-X.json").write_text(json.dumps(budget))
    return ["--elf", str(elf), "--ceilings", str(ceil), "--budget-dir", str(bdir), "--lane", "E2-X"]


def row(n):
    return {"lane": "E2-X", "allowance_bytes": 0, "static_dram_bytes": n, "why": "a 512 B static buffer"}


def test_declared_growth_under_the_ceiling_passes(tmp_path, capsys):
    args = e2_case(tmp_path, 600 + 512, row(512))  # 1,512 B: +512 over the baseline, declared
    assert dram.main([*args, "--variant", "freenove-28-e2"]) == 0
    assert "inside E2-X's declared 512 B" in capsys.readouterr().out


def test_counter_undeclared_growth_fails_under_the_ceiling(tmp_path, capsys):
    args = e2_case(tmp_path, 600 + 513, row(512))  # 1 B more than declared, far under 3,048 B
    assert dram.main([*args, "--variant", "freenove-28-e2"]) == 1
    out = capsys.readouterr().out
    assert "1 B more than E2-X declared (512 B)" in out and "static_dram_bytes" in out


def test_counter_no_file_declares_nothing(tmp_path):
    args = e2_case(tmp_path, 601)
    assert dram.main([*args, "--variant", "freenove-28-e2"]) == 1


def test_counter_a_declaration_never_lifts_the_ceiling(tmp_path):
    args = e2_case(tmp_path, 600 + 2049, row(4096))
    assert dram.main([*args, "--variant", "freenove-28-e2"]) == 1


def test_counter_a_malformed_declaration_fails(tmp_path, capsys):
    args = e2_case(tmp_path, 600, row(-1))
    assert dram.main([*args, "--variant", "freenove-28-e2"]) == 1
    assert "non-negative integer" in capsys.readouterr().out


def test_a_shipped_engine1_row_keeps_its_ceiling_only_check(tmp_path, capsys):
    args = e2_case(tmp_path, 690)  # nimbus-tft: +90 B over its baseline, under its ceiling, no file
    assert dram.main([*args, "--variant", "nimbus-tft"]) == 0
    assert "declared" not in capsys.readouterr().out


def test_real_ceilings_say_how_to_declare_and_give_engine2_2048_b_of_room():
    with open(dram.CEILINGS, encoding="utf-8") as f:
        data = json.load(f)
    how = data["allowance_how"]
    assert "static_dram_bytes" in how and "image_budget.d" in how and "ruling 121" in how
    assert len(data["engine2_baseline_sha"]) == 40
    for name, r in data["engine2"].items():
        assert r["ceiling"] == r["baseline"] + 2048, name


def test_a_bad_declaration_fails_the_engine2_images_only(tmp_path, capsys):
    args = e2_case(tmp_path, 690, row("many"))
    assert dram.main([*args, "--variant", "nimbus-tft"]) == 0  # an engine-1 image: its ceiling alone
    assert dram.main([*args, "--variant", "freenove-28-e2"]) == 1


class _Args:
    lane = None
    no_infer_lane = False
    base_ref = "rb-main"

    def __init__(self, ceilings, budget_dir):
        self.ceilings, self.budget_dir = ceilings, budget_dir


def test_a_lane_read_from_the_branch_is_judged_only_on_a_fresh_baseline(tmp_path, monkeypatch):
    e2_case(tmp_path, 600, row(0))
    args = _Args(str(tmp_path / "ceil.json"), str(tmp_path / "budget.d"))
    monkeypatch.setattr(dram.image, "infer_lane", lambda _root: "E2-X")
    monkeypatch.setattr(dram.image, "git_baseline_problems", lambda _r, _b: lambda _sha: [])
    assert dram.lane_view(args) == ("E2-X", 0, [], [])
    monkeypatch.setattr(dram.image, "git_baseline_problems", lambda _r, _b: lambda _sha: ["stale: x.cpp changed"])
    lane, _n, fails, warns = dram.lane_view(args)
    assert lane is None and not fails and "not judged" in warns[0]
    args.no_infer_lane = True
    assert dram.lane_view(args) == (None, 0, [], [])
