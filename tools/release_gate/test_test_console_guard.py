"""Tests for check_test_console_guard.py (the test console never ships in production).

Run: python3 -m pytest tools/release_gate
"""

import check_test_console_guard as g

GUARDED_CPP = """// test_console - implementation
#include "test_console.h"
#include <Arduino.h>

#if defined(NIMBUS_TEST)
namespace nimbus::tc {
void dispatch(String line) {
  if (line.startsWith("PANIC ")) { return; }
#ifdef SOME_BOARD
  int x = 1;
#endif
}
}  // namespace nimbus::tc
#endif  // NIMBUS_TEST
"""

HEADER = """#pragma once
#if defined(NIMBUS_TEST)
void pumpOrch();
bool ttsMuted();
#else
inline void pumpOrch() {}
inline bool ttsMuted() { return false; }
#endif
"""

MAIN = """#include "test_console.h"
void setup() {
#ifdef NIMBUS_TEST
  tc::Hooks h;
  h.audioInject = [](const String& p) { return audioInjectTurn(p); };
#endif
}
#if defined(NIMBUS_TEST)
static size_t clipRead(void* ctx, uint64_t off, uint8_t* dst, size_t n) { return 0; }
static size_t copyClipPcm(const String& path, String& why) { return 0; }
static String audioInjectTurn(const String& path) { return path; }
#endif
"""


def test_the_real_tree_is_guarded():
    assert g.main([]) == 0


def test_guarded_console_passes():
    ok, problems = g.judge(GUARDED_CPP, HEADER, MAIN)
    assert ok, problems


def test_a_verb_outside_the_guard_fails_even_if_unlisted():
    leaked = GUARDED_CPP + '\nvoid brandNewVerb() { Serial.println("ZAP"); }\n'
    ok, problems = g.judge(leaked, HEADER, MAIN)
    assert not ok
    assert any("outside the NIMBUS_TEST guard" in p for p in problems)


def test_comments_and_includes_outside_the_guard_are_allowed():
    ok, _ = g.judge("/* a\n block */\n// line\n#include <x.h>\n" + GUARDED_CPP, HEADER, MAIN)
    assert ok


def test_nested_conditionals_do_not_end_the_region_early():
    # The #ifdef SOME_BOARD / #endif pair inside must not close the NIMBUS_TEST region.
    regions = g.test_regions(GUARDED_CPP)
    assert len(regions) == 1
    assert GUARDED_CPP.splitlines()[regions[0][1] - 1].startswith("#endif  // NIMBUS_TEST")


def test_missing_guard_fails():
    ok, problems = g.judge(GUARDED_CPP.replace("#if defined(NIMBUS_TEST)", "#if 1"), HEADER, MAIN)
    assert not ok and any("no NIMBUS_TEST region" in p for p in problems)


def test_header_without_production_stub_fails():
    ok, problems = g.judge(GUARDED_CPP, HEADER.replace("inline bool ttsMuted() { return false; }\n", ""), MAIN)
    assert not ok and any("ttsMuted" in p for p in problems)


def test_main_helper_outside_a_test_region_fails():
    unguarded = MAIN.replace("#if defined(NIMBUS_TEST)\nstatic size_t clipRead", "static size_t clipRead", 1)
    unguarded = unguarded.replace("{ return path; }\n#endif\n", "{ return path; }\n", 1)
    ok, problems = g.judge(GUARDED_CPP, HEADER, unguarded)
    assert not ok
    assert any("copyClipPcm" in p and "OUTSIDE" in p for p in problems)


def test_main_helper_renamed_is_reported():
    ok, problems = g.judge(GUARDED_CPP, HEADER, MAIN.replace("audioInjectTurn", "voiceInject"))
    assert not ok and any("not found" in p for p in problems)
