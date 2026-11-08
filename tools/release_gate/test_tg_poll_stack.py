"""Host tests for the tg_poll stack gate (check_tg_poll_stack.py).

The analysis logic runs on synthetic disassembly and call graphs, so it is tested with
no toolchain. The last test runs the real gate on the built esp32s3-cyd image when the
image and the Xtensa toolchain are there (the battery builds it right before this
suite); elsewhere it is skipped, and the release workflow runs the gate on every image.

Run: python3 -m pytest tools/release_gate
"""

import json
import os
import re

import pytest

import check_tg_poll_stack as st

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))


def fn(addr, size, name, frame=0):
    f = st.Fn(addr, size, name)
    f.frame = frame
    return f


def config(**over):
    cfg = {
        "entry": "root",
        "task_wrapper": "none",
        "stack_bytes": 1000,
        "min_free_bytes": 200,
        "fixed_overhead": {"irq": 0},
        "unresolved_indirect_bytes": 0,
        "indirect": [],
        "paths": [],
    }
    cfg.update(over)
    return cfg


def model(funcs, cfg, root=1):
    m = st.Model(funcs, cfg, {})
    m.analyse(root)
    return m


# ---- disassembly --------------------------------------------------------------------

DIS = """
42001000 <a>:
42001000:	entry	a1, 0x60
42001003:	call8	42002000 <b>
42001006:	l32r	a8, 42000f00 <lit> (42003000 <c>)
42001009:	callx8	a8
4200100c:	l32i	a8, a2, 0
4200100f:	l32i	a8, a8, 12
42001012:	callx8	a8
42001015:	retw.n
42001017:	callx8	a9

42002000 <b>:
42002000:	entry	a1, 32
42002003:	retw.n

42003000 <c>:
42003000:	entry	a1, 0x1350
42003003:	retw.n
"""
A, B, C = 0x42001000, 0x42002000, 0x42003000


def parsed():
    funcs = {A: fn(A, 0x1A, "a"), B: fn(B, 4, "b"), C: fn(C, 4, "c")}
    st.parse_disassembly(DIS, funcs)
    return funcs


def test_frames_are_the_entry_allocation():
    funcs = parsed()
    assert funcs[A].frame == 0x60 and funcs[B].frame == 32 and funcs[C].frame == 4944


def test_direct_and_long_calls_are_edges():
    calls = [t for _s, t in parsed()[A].calls]
    assert B in calls and C in calls  # call8, and l32r+callx8 (-mlongcalls)


def test_a_vtable_load_is_an_indirect_site_with_its_slot():
    assert (0x42001012, 12) in parsed()[A].indirect


def test_dead_code_after_a_return_adds_no_call():
    # 0x42001017 sits after retw.n and no branch lands there: objdump decoding data.
    assert all(site != 0x42001017 for site, _slot in parsed()[A].indirect)


def test_a_jump_into_another_function_is_a_call():
    funcs = {0x42000010: fn(0x42000010, 8, "t"), B: fn(B, 4, "b")}
    st.parse_disassembly("42000010 <t>:\n42000010:\tj\t42002000 <b>\n", funcs, {0x42000010, B})
    assert funcs[0x42000010].calls == [(0x42000010, B)]


# ---- names ----------------------------------------------------------------------------


def test_std_function_signature_keeps_template_commas():
    name = (
        "std::_Function_handler<std::__cxx11::basic_string<char, std::char_traits<char>, "
        "std::allocator<char> > (int), foo()::{lambda(int)#1}>::_M_invoke(std::_Any_data const&, int&&)"
    )
    sig = st.handler_signature(name, "_M_invoke")
    assert sig == ("std::__cxx11::basic_string<char, std::char_traits<char>, std::allocator<char> > (int)")
    assert st.lambda_parent(name) == "foo()"


def test_method_names():
    assert st.method_name("ns::Cls<a, b>::read(unsigned char*, int)") == "read"
    assert st.method_name("non-virtual thunk to X::~X()") == "~"
    assert st.split_name("agent::(anonymous namespace)::CardFs::list(std::string const&)") == (
        "agent::(anonymous namespace)::CardFs",
        "list",
    )


def test_code_only_drops_strings_and_comments():
    assert "delete" not in st.code_only('if (!strcmp(a, "delete")) go(); // delete x')


# ---- the model ------------------------------------------------------------------------


def test_worst_case_is_the_deepest_chain_and_the_entry_depth_adds_up():
    funcs = {1: fn(1, 4, "root", 100), 2: fn(2, 4, "x", 300), 3: fn(3, 4, "y", 50), 4: fn(4, 4, "z", 400)}
    funcs[1].calls = [(10, 2), (11, 3)]
    funcs[3].calls = [(12, 4)]
    m = model(funcs, config())
    assert m.through([1]) == (550, 1)  # root -> y -> z
    assert m.entry[4] == 150 and m.below[4] == 400
    assert [a for a, _s in m.chain_down(1)] == [1, 3, 4]


def test_an_undeclared_cycle_fails_and_a_declared_one_is_paid_per_level():
    funcs = {1: fn(1, 4, "root", 10), 2: fn(2, 4, "walk", 100)}
    funcs[1].calls = [(10, 2)]
    funcs[2].calls = [(11, 2)]
    assert any("undeclared cycle" in e for e in model(funcs, config()).errors)
    m = model(funcs, config(recursion=[{"function": "^walk$", "depth": 4, "why": "test"}]))
    assert not m.errors and m.through([1])[0] == 10 + 400


def test_an_unresolved_indirect_call_is_charged():
    funcs = {1: fn(1, 4, "root", 10)}
    funcs[1].indirect = [(10, None)]
    assert model(funcs, config(unresolved_indirect_bytes=256)).through([1])[0] == 266


def test_a_rule_resolves_an_indirect_call_and_a_terminal_stops_the_walk():
    funcs = {1: fn(1, 4, "root", 10), 2: fn(2, 4, "hook", 20), 3: fn(3, 4, "abort", 30), 4: fn(4, 4, "deep", 999)}
    funcs[1].indirect = [(10, None)]
    funcs[2].calls = [(11, 3)]
    funcs[3].calls = [(12, 4)]
    cfg = config(
        indirect=[{"in": "^root$", "targets": ["^hook$"], "why": "t"}], terminal=[{"function": "^abort$", "why": "t"}]
    )
    assert model(funcs, cfg).through([1])[0] == 60


def test_a_dynamic_frame_needs_a_bound():
    funcs = {1: fn(1, 4, "root", 10), 2: fn(2, 4, "vla", 20)}
    funcs[1].calls = [(10, 2)]
    funcs[2].dynamic = True
    assert any("dynamic stack frame" in e for e in model(funcs, config()).errors)
    m = model(funcs, config(dynamic_frames=[{"function": "^vla$", "extra_bytes": 500, "why": "t"}]))
    assert m.through([1])[0] == 530


def test_the_verdict_holds_the_floor():
    funcs = {1: fn(1, 4, "root", 801)}
    m = model(funcs, config())
    m.elf, m.tools = "x", {"addr2line": "true"}
    v = st.verdict(m)
    assert not v["ok"] and v["rows"][0]["free"] == 199
    funcs[1].frame = 800
    m = model(funcs, config())
    m.elf, m.tools = "x", {"addr2line": "true"}
    assert st.verdict(m)["ok"]


def test_a_stale_config_row_is_reported():
    funcs = {1: fn(1, 4, "root", 10)}
    m = model(funcs, config(indirect=[{"in": "^nothing$", "targets": [], "why": "t"}]))
    m.elf, m.tools = "x", {"addr2line": "true"}
    v = st.verdict(m)
    assert any("stale indirect rule" in e for e in v["stale"]) and v["ok"]


# ---- the shipped config ---------------------------------------------------------------


def test_every_config_row_says_why_and_compiles():
    cfg = st.load_config()
    assert cfg["stack_bytes"] == 24576 and cfg["min_free_bytes"] == 4096
    for section in (
        "indirect",
        "not_on_tg_poll",
        "recursion",
        "impossible_cycles",
        "impossible_calls",
        "dynamic_frames",
        "terminal",
        "hierarchies",
    ):
        for row in cfg.get(section, []):
            assert row.get("why"), f"{section} row without why: {row}"
            for key in ("in", "at", "function", "caller", "callee"):
                if key in row:
                    re.compile(row[key])
    assert len(cfg["paths"]) >= 12


def test_the_stack_size_in_the_config_is_the_tasks():
    src = open(os.path.join(REPO, "src", "agent", "telegram.cpp"), encoding="utf-8").read()
    m = re.search(r"POLL_STACK_BYTES\s*=\s*(\d+)", src)
    assert m and int(m.group(1)) == st.load_config()["stack_bytes"]


# ---- the built image ------------------------------------------------------------------

ELF = os.path.join(REPO, ".pio", "build", "esp32s3-cyd", "firmware.elf")


def _toolchain():
    try:
        st.find_tool("objdump")
        return os.path.exists(st.find_rom_elf(st.load_config()))
    except FileNotFoundError:
        return False


@pytest.mark.skipif(
    not (os.path.exists(ELF) and _toolchain()),
    reason="no esp32s3-cyd image or Xtensa toolchain here (the battery and release.yml run it)",
)
def test_the_built_image_keeps_the_floor_on_every_tg_poll_path():
    assert st.check(ELF, st.load_config(), quiet=True), "tg_poll leaves under 4096 B on some path (report above)"


def test_report_shape_is_json_safe():
    json.dumps(config())
