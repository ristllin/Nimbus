"""The bench's cut and allocation hooks never reach a production image (lane RB-REC7, ruling 87).

CUTARM (a panic at the cut point of a files.append, a files.edit part or a swap) and ALLOCREFUSE (a refusing
working allocator) exist for tests/hil/test_files_bench.py. Their console verbs sit in src/test_console.cpp, which
check_test_console_guard.py keeps NIMBUS_TEST-only; their hooks sit in src/agent/files_subsystem.{h,cpp}. This test
reads those sources: every line that names a hook is inside a NIMBUS_TEST region, except the one macro whose
production expansion is a no-op; the production envs never define NIMBUS_TEST; and, when a production ELF is built,
it holds none of the hooks' strings. Counter-tests show each check catches the leak it exists for.
"""

import os
import re

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))
SOURCES = ("src/agent/files_subsystem.h", "src/agent/files_subsystem.cpp")
HOOK = re.compile(
    r"testArmCut|testCutArmed|testCutNow|testCutSwapStep|TestCut::|g_cut(Where|At|Ops)|CUT_SWAP_STEP"
    r"|testArmHeapRefuse|testHeapRefused|HeapRefuseWindow|HEAP_REFUSE_WINDOW|g_heap(Armed|Window|Min|Refused|Task)|operator new"
)
# Allowed outside a NIMBUS_TEST region: the swap step macro's call sites and its production no-op definition.
CALL_SITE = re.compile(
    r"^\s*(CUT_SWAP_STEP\(\);|HEAP_REFUSE_WINDOW\(\);.*|#define (CUT_SWAP_STEP|HEAP_REFUSE_WINDOW)\(\) \(\(void\)0\))\s*$"
)
PRODUCTION_ENVS = ("esp32s3", "esp32s3-cyd")
MARKERS = (b"CUTARM", b"ALLOCREFUSE", b"CUT %s at")


def leaks(text: str) -> list:
    """(line number, line) of every hook name outside a NIMBUS_TEST region, the allowed call sites left out."""
    out, stack = [], []  # per #if level: "test" (NIMBUS_TEST holds), "not" (it does not), "other"
    for n, line in enumerate(text.split("\n"), 1):
        s = line.strip()
        if s.startswith("#if"):
            if re.match(r"#(ifdef\s+NIMBUS_TEST|if\s+defined\(NIMBUS_TEST\))\b", s):
                stack.append("test")
            elif re.match(r"#(ifndef\s+NIMBUS_TEST|if\s+!defined\(NIMBUS_TEST\))\b", s):
                stack.append("not")
            else:
                stack.append("other")
            continue
        if (s.startswith("#else") or s.startswith("#elif")) and stack:
            stack[-1] = {"test": "not", "not": "test"}.get(stack[-1], "other")
            continue
        if s.startswith("#endif") and stack:
            stack.pop()
            continue
        if "test" in stack or not HOOK.search(line) or CALL_SITE.match(line):
            continue
        out.append((n, s))
    return out


def no_op_in_production(text: str) -> bool:
    """CUT_SWAP_STEP() has an #else branch that defines it as ((void)0)."""
    return no_op(text, "CUT_SWAP_STEP")


def no_op(text: str, macro: str) -> bool:
    """`macro()` is defined as ((void)0) in an #else branch (beside other no-op macros)."""
    pat = rf"#else\s*\n(?:\s*#define \w+\(\) \(\(void\)0\)\s*\n)*?\s*#define {macro}\(\) \(\(void\)0\)"
    return re.search(pat, text) is not None


def env_flags(env: str) -> str:
    text = open(os.path.join(REPO, "platformio.ini"), encoding="utf-8").read()
    m = re.search(rf"^\[env:{re.escape(env)}\]\n(.*?)(?=^\[)", text, re.M | re.S)
    assert m, env
    return m.group(1)


def test_every_hook_line_is_test_only():
    for path in SOURCES:
        text = open(os.path.join(REPO, path), encoding="utf-8").read()
        assert HOOK.search(text), f"{path} names no hook: this test is reading the wrong files"
        assert leaks(text) == [], f"{path}: hook outside NIMBUS_TEST: {leaks(text)}"
    text = open(os.path.join(REPO, SOURCES[1]), encoding="utf-8").read()
    assert no_op_in_production(text)
    assert no_op(text, "HEAP_REFUSE_WINDOW")


def test_the_production_envs_never_define_nimbus_test():
    for env in PRODUCTION_ENVS:
        assert "NIMBUS_TEST" not in env_flags(env), env
    assert "-DNIMBUS_TEST" in env_flags("test-cyd")


def test_a_built_production_elf_holds_no_hook():
    for env in PRODUCTION_ENVS:
        elf = os.path.join(REPO, ".pio", "build", env, "firmware.elf")
        if not os.path.exists(elf):
            continue  # not built in this tree; the battery builds both and runs this again
        data = open(elf, "rb").read()
        assert not [m for m in MARKERS if m in data], f"{env}: a bench hook string is in the production image"


def test_counter_a_hook_outside_the_guard_is_caught():
    leak = "static void f() {\n  testArmCut(TestCut::Append, 3);\n}\n"
    assert leaks(leak) == [(2, "testArmCut(TestCut::Append, 3);")]
    guarded = "#ifdef NIMBUS_TEST\nstatic void f() { testArmCut(TestCut::Append, 3); }\n#endif\n"
    assert leaks(guarded) == []
    in_else = "#ifdef NIMBUS_TEST\n#else\nstatic void f() { testCutNow(\"x\", 1); }\n#endif\n"
    assert leaks(in_else) != []
    assert leaks("  CUT_SWAP_STEP();\n") == []
    assert not no_op_in_production("#define CUT_SWAP_STEP() testCutSwapStep()\n")


def test_counter_a_marker_in_an_elf_is_caught(tmp_path):
    fake = tmp_path / "firmware.elf"
    fake.write_bytes(b"\x7fELF....CUTARM armed append 3....")
    assert [m for m in MARKERS if m in fake.read_bytes()] == [b"CUTARM"]
