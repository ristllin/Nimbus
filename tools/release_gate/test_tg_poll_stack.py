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


# ---- decode resync and landing pads ------------------------------------------------

# A landing pad right after an unconditional jump, behind 1 byte of alignment padding:
# objdump's linear decode reads the padding as the start of a 3-byte instruction and stays
# out of step until 0x42001010, inventing a call8 to b on the way (7631b6a2 had one such
# call to runTurn, a cycle that is not in the code).
PAD_DIS = """
42001000 <f>:
42001000:	entry	a1, 32
42001003:	j	42001010 <f+0x10>
42001006:	call8	42002000 <b>
42001009:	addi	a8, a8, 1
42001010:	retw.n

42002000 <b>:
42002000:	entry	a1, 2048
42002003:	retw.n

42003000 <c>:
42003000:	entry	a1, 64
42003003:	retw.n
"""
F, PAD = 0x42001000, 0x42001007
REAL = [
    (0x42001007, "l32r", "a8, 42000f00 <lit> (42003000 <x>)"),
    (0x4200100A, "callx8", "a8"),
    (0x4200100D, "j", "42001010 <x>"),
]


class FakeDecoder(st.Decoder):
    """The pad's bytes (l32r, callx8, j: three 3-byte instructions) and its real decode."""

    def __init__(self):
        mem = {0x42001007: 0x81, 0x4200100A: 0xE0, 0x4200100D: 0x06}
        super().__init__(lambda a, n: bytes(mem.get(a + i, 0) for i in range(n)), "objdump")
        self.asked = []

    def decode(self, stretches):
        self.asked += stretches
        return {e: [i for i in REAL if e <= i[0] < stop] for e, stop in stretches}


def pad_funcs():
    return {F: fn(F, 0x12, "f"), B: fn(B, 4, "b"), C: fn(C, 4, "c")}


def test_instruction_length_follows_op0():
    assert [st.insn_length(b) for b in (0x36, 0x81, 0x07, 0x1D, 0x88, 0xCD, 0x0E, 0x0F)] == [3, 3, 3, 2, 2, 2, 4, 4]


def test_a_misaligned_landing_pad_is_decoded_again_and_the_invented_call_goes():
    funcs = pad_funcs()
    st.parse_disassembly(PAD_DIS, funcs)
    assert B in [t for _s, t in funcs[F].calls]  # the linear decode alone: a call to b that is not there
    funcs = pad_funcs()
    dec = FakeDecoder()
    st.parse_disassembly(PAD_DIS, funcs, None, dec, {F: {PAD}})
    assert dec.asked == [(PAD, 0x42001010)]  # from the pad to the first shared boundary
    assert [t for _s, t in funcs[F].calls] == [C]  # the pad's real long call, nothing else


def test_objdump_decodes_a_stretch_at_its_real_address():
    try:
        objdump = st.find_tool("objdump")
    except FileNotFoundError:
        pytest.skip("no Xtensa objdump here")
    # 0x42001007: l32r a8, [0x42001000] (= 0x42003000); callx8 a8; then call8 0x42002000.
    mem = {0x42001000: bytes((0x00, 0x30, 0x00, 0x42)), 0x42001007: bytes((0x81, 0xFE, 0xFF, 0xE0, 0x08, 0x00))}
    mem[0x4200100D] = bytes((0xE5, 0x0F, 0x00))  # call8, offset field 0x3f

    def read(a, n):
        for base, data in mem.items():
            if base <= a and a + n <= base + len(data):
                return data[a - base : a - base + n]
        return None

    out = st.Decoder(read, objdump).decode([(0x42001007, 0x4200100D), (0x4200100D, 0x42001010)])
    assert out[0x42001007] == [
        (0x42001007, "l32r", "a8, 42001000 <lit> (42003000 <x>)"),
        (0x4200100A, "callx8", "a8"),
    ]
    ((addr, op, args),) = out[0x4200100D]
    assert (addr, op) == (0x4200100D, "call8")
    assert int(args.split()[0], 16) == (0x4200100D & ~3) + 4 + (0x3F << 2)  # pc-relative, at the real address


def test_live_targets_ignore_branches_decoded_from_dead_bytes():
    insns = [(1, "j", "00000009 <x>"), (4, "beqz", "a2, 00000007 <x>"), (9, "retw.n", "")]
    assert st.live_targets(insns, {1}) == {9}


def _uleb_bytes(v):
    out = bytearray()
    while True:
        b, v = v & 0x7F, v >> 7
        out.append(b | (0x80 if v else 0))
        if not v:
            return bytes(out)


def test_eh_frame_and_lsda_give_the_landing_pads():
    import struct

    eh, lsda, func = 0x3C000000, 0x3C001000, 0x42001000
    cie = struct.pack("<IB", 0, 1) + b"zLR\0" + b"\x01\x7c\x00" + _uleb_bytes(2) + b"\x00\x1b"
    cie += b"\0" * (-(len(cie) + 4) % 4)
    fde_at = eh + 4 + len(cie)
    fde = struct.pack("<I", fde_at + 4 - eh)
    fde += struct.pack("<i", func - (fde_at + 8)) + struct.pack("<i", 0x12) + _uleb_bytes(4) + struct.pack("<I", lsda)
    frame = struct.pack("<I", len(cie)) + cie + struct.pack("<I", len(fde)) + fde + struct.pack("<I", 0)
    table = bytes((0xFF, 0xFF, 0x01)) + _uleb_bytes(8) + bytes((0, 3, 7, 0, 3, 3, 0, 0))
    mem = {eh + i: b for i, b in enumerate(frame)} | {lsda + i: b for i, b in enumerate(table)}

    def read(a, n):
        return bytes(mem[a + i] for i in range(n))

    assert list(st.eh_frames(read, eh)) == [(func, lsda)]
    assert st.landing_pads(read, func, lsda) == {func + 7}  # the second call site has no pad
    nm = f"{eh:08x} A __eh_frame\n"
    assert st.image_landing_pads(read, nm) == {func: {func + 7}}


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


def _elf(env):
    return os.path.join(REPO, ".pio", "build", env, "firmware.elf")


def _toolchain():
    try:
        st.find_tool("objdump")
        return os.path.exists(st.find_rom_elf(st.load_config()))
    except FileNotFoundError:
        return False


@pytest.mark.parametrize("env", ["esp32s3-cyd", "esp32s3"])
def test_the_built_image_keeps_the_floor_on_every_tg_poll_path(env):
    if not (os.path.exists(_elf(env)) and _toolchain()):
        pytest.skip(f"no {env} image or Xtensa toolchain here (the battery and release.yml run it)")
    assert st.check(_elf(env), st.load_config(), quiet=True), f"tg_poll leaves under 4096 B on some {env} path"


def test_report_shape_is_json_safe():
    json.dumps(config())
