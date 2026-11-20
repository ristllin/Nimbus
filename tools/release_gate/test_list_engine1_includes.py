"""Tests for tools/engine/list_engine1_includes.py (lane RB-SEAM2), each failure class with its
counter-case, and the tree pin: every device file that includes an old-engine header is guarded,
engine1_only or neutral (tools/engine/guard_files.txt), so an engine-2 build (RB-I0) can drop the
old engine's sources.
"""

from __future__ import annotations

import os
import shutil
import sys
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, os.path.join(REPO, "tools", "engine"))

import list_engine1_includes as lister  # noqa: E402

GUARD = "#if !defined(NIMBUS_ENGINE) || NIMBUS_ENGINE == 1"
OLD = "lib/harness/include/nimbus/harness/engine.h"


class ScanIncludesTests(unittest.TestCase):
    def names(self, text, guarded):
        return [n for n, g, _ in lister.scan_includes(text) if g == guarded]

    def test_an_include_inside_the_guard_is_guarded(self):
        text = GUARD + '\n#include "a.h"\n#endif\n#include "b.h"\n'
        self.assertEqual(self.names(text, True), ["a.h"])
        self.assertEqual(self.names(text, False), ["b.h"])

    def test_the_guard_with_other_spacing_is_the_guard(self):
        text = "#  if !defined( NIMBUS_ENGINE )||NIMBUS_ENGINE==1\n#include <a.h>\n#endif\n"
        self.assertEqual(self.names(text, True), ["a.h"])

    def test_the_else_branch_of_the_guard_is_not_guarded(self):
        text = GUARD + '\n#include "a.h"\n#else\n#include "b.h"\n#endif\n'
        self.assertEqual(self.names(text, False), ["b.h"])

    def test_an_elif_branch_of_the_guard_is_not_guarded(self):
        text = GUARD + '\n#elif 1\n#include "b.h"\n#endif\n'
        self.assertEqual(self.names(text, False), ["b.h"])

    def test_a_nested_conditional_inside_the_guard_stays_guarded(self):
        text = GUARD + '\n#ifdef NIMBUS_TEST\n#include "a.h"\n#else\n#include "c.h"\n#endif\n#endif\n'
        self.assertEqual(sorted(self.names(text, True)), ["a.h", "c.h"])

    def test_another_condition_is_not_the_guard(self):
        for cond in ("#if NIMBUS_ENGINE == 1", "#ifdef NIMBUS_TEST", "#if 0", "#if !defined(NIMBUS_ENGINE)"):
            text = cond + '\n#include "a.h"\n#endif\n'
            self.assertEqual(self.names(text, False), ["a.h"], cond)

    def test_an_include_after_the_guard_closes_is_not_guarded(self):
        text = GUARD + '\n#endif\n#include "a.h"\n'
        self.assertEqual(self.names(text, False), ["a.h"])

    def test_commented_includes_are_not_includes(self):
        text = '// #include "a.h"\n/* #include "b.h"\n#include "c.h" */\n#include "d.h"\n'
        self.assertEqual(self.names(text, False), ["d.h"])

    def test_a_comment_opener_inside_a_string_does_not_hide_includes(self):
        text = 'const char* k = "image/*";\n#include "a.h"\nconst char* u = "http://x";\n#include "b.h" // */\n'
        self.assertEqual(self.names(text, False), ["a.h", "b.h"])

    def test_an_include_inside_a_raw_string_is_not_an_include(self):
        text = 'const char* p = R"html(\n#include "a.h"\n)html";\n#include "b.h"\n'
        self.assertEqual(self.names(text, False), ["b.h"])

    def test_line_numbers_survive_comments_and_literals(self):
        text = '/*\n\n*/\nconst char* p = R"(\n)";\n#include "a.h"\n'
        self.assertEqual(lister.scan_includes(text)[0][2], 6)


class TreeTestCase(unittest.TestCase):
    def setUp(self):
        self.root = tempfile.mkdtemp(prefix="e1inc-")
        self.addCleanup(shutil.rmtree, self.root)
        self.write(OLD, "#pragma once\n")
        self.write("lib/harness/include/nimbus/harness/jobs.h", "#pragma once\n")

    def write(self, rel, text):
        path = os.path.join(self.root, rel)
        os.makedirs(os.path.dirname(path), exist_ok=True)
        with open(path, "w", encoding="utf-8") as fh:
            fh.write(text)

    def listing(self, decisions=None):
        decisions = decisions or {}
        scan = lister.Scan(self.root, {OLD} | lister.device_engine1_headers(decisions))
        return scan

    def problems(self, guard_files=""):
        self.write(lister.GUARD_LIST, guard_files)
        decisions, problems = lister.read_decisions(self.root)
        scan = lister.Scan(self.root, {OLD} | lister.device_engine1_headers(decisions))
        return lister.check(self.root, scan, decisions, problems)


class ScanTests(TreeTestCase):
    def test_a_direct_include_is_listed(self):
        self.write("src/a.cpp", '#include "nimbus/harness/engine.h"\n')
        self.assertEqual(self.listing().listing(), {"src/a.cpp": {OLD: ""}})

    def test_a_shared_header_is_not_listed(self):
        self.write("src/a.cpp", '#include "nimbus/harness/jobs.h"\n')
        self.assertEqual(self.listing().listing(), {})

    def test_an_include_through_a_device_header_is_listed_with_its_path(self):
        self.write("src/agent/x.h", '#include "nimbus/harness/engine.h"\n')
        self.write("src/net/web.cpp", '#include "../agent/x.h"\n')
        rows = self.listing().listing()
        self.assertEqual(rows["src/net/web.cpp"], {OLD: "src/agent/x.h"})
        self.assertEqual(rows["src/agent/x.h"], {OLD: ""})

    def test_a_device_header_that_guards_its_include_stops_the_chain(self):
        self.write("src/agent/x.h", GUARD + '\n#include "nimbus/harness/engine.h"\n#endif\n')
        self.write("src/net/web.cpp", '#include "../agent/x.h"\n')
        self.assertEqual(self.listing().listing(), {})
        self.assertEqual(self.listing().listing(guarded_too=True), {"src/agent/x.h": {OLD: "guarded"}})

    def test_an_include_cycle_terminates(self):
        self.write("src/a.h", '#include "b.h"\n#include "nimbus/harness/engine.h"\n')
        self.write("src/b.h", '#include "a.h"\n')
        rows = self.listing().listing()
        self.assertEqual(rows["src/b.h"], {OLD: "src/a.h"})

    def test_a_chain_through_a_shared_library_header_is_followed(self):
        self.write("lib/harness/include/nimbus/harness/shared.h", '#include "engine.h"\n')
        self.write("src/a.cpp", '#include "nimbus/harness/shared.h"\n')
        rows = self.listing().listing()
        self.assertEqual(rows, {"src/a.cpp": {OLD: "lib/harness/include/nimbus/harness/shared.h"}})

    def test_lib_sources_are_not_device_files(self):
        self.write("lib/harness/src/engine.cpp", '#include "nimbus/harness/engine.h"\n')
        self.assertEqual(self.listing().listing(), {})


class CheckTests(TreeTestCase):
    def test_an_unguarded_includer_is_a_problem(self):
        self.write("src/a.cpp", '#include "nimbus/harness/engine.h"\n')
        self.assertEqual(len(self.problems()), 1)

    def test_an_engine1_only_includer_is_not(self):
        self.write("src/a.cpp", '#include "nimbus/harness/engine.h"\n')
        self.assertEqual(self.problems("src/a.cpp  # engine1_only: the old seam\n"), [])

    def test_engine1_only_txt_counts_too(self):
        self.write("src/a.cpp", '#include "nimbus/harness/engine.h"\n')
        self.write(lister.ENGINE1_ONLY_LIST, "src/a.cpp\n")
        self.assertEqual(self.problems(), [])

    def test_a_guarded_includer_listed_as_guard_is_clean(self):
        self.write("src/a.cpp", GUARD + '\n#include "nimbus/harness/engine.h"\n#endif\n')
        self.assertEqual(self.problems("src/a.cpp  # guard: why\n"), [])

    def test_a_guard_entry_with_no_old_include_is_a_problem(self):
        self.write("src/a.cpp", '#include "nimbus/harness/jobs.h"\n')
        self.assertEqual(len(self.problems("src/a.cpp  # guard: why\n")), 1)

    def test_a_neutral_entry_that_still_includes_is_a_problem(self):
        self.write("src/a.cpp", GUARD + '\n#include "nimbus/harness/engine.h"\n#endif\n')
        self.assertEqual(len(self.problems("src/a.cpp  # neutral: why\n")), 1)
        self.write("src/a.cpp", '#include "nimbus/harness/jobs.h"\n')
        self.assertEqual(self.problems("src/a.cpp  # neutral: why\n"), [])

    def test_an_unknown_decision_a_missing_file_and_a_duplicate_are_problems(self):
        self.write("src/a.cpp", "\n")
        self.assertEqual(len(self.problems("src/a.cpp  # maybe: why\n")), 1)
        self.assertEqual(len(self.problems("src/gone.cpp  # guard: why\n")), 1)
        self.assertEqual(len(self.problems("src/a.cpp  # neutral: x\nsrc/a.cpp  # neutral: y\n")), 1)

    def test_an_engine1_only_header_is_old_for_its_includers(self):
        self.write("src/agent/skills.h", "#pragma once\n")
        self.write("src/agent/skills.cpp", '#include "skills.h"\n')
        self.write("src/net/web.cpp", '#include "../agent/skills.h"\n')
        lines = "src/agent/skills.h  # engine1_only: x\nsrc/agent/skills.cpp  # engine1_only: x\n"
        problems = self.problems(lines)
        self.assertEqual(len(problems), 1)
        self.assertIn("src/net/web.cpp", problems[0])
        self.write("src/net/web.cpp", GUARD + '\n#include "../agent/skills.h"\n#endif\n')
        self.assertEqual(self.problems(lines + "src/net/web.cpp  # guard: x\n"), [])

    def test_main_counts_an_engine1_only_header_as_old(self):
        for hdr in lister.engine1_headers():
            self.write(hdr, "#pragma once\n")
        self.write("src/agent/skills.h", "#pragma once\n")
        self.write("src/agent/skills.cpp", '#include "skills.h"\n')
        self.write("src/net/web.cpp", '#include "../agent/skills.h"\n')
        self.write(
            lister.GUARD_LIST, "src/agent/skills.h  # engine1_only: x\nsrc/agent/skills.cpp  # engine1_only: x\n"
        )
        self.assertEqual(lister.main(["--root", self.root, "--check"]), 1)
        self.write("src/net/web.cpp", "\n")
        self.assertEqual(lister.main(["--root", self.root, "--check"]), 0)

    def test_an_engine1_only_source_needs_its_header_engine1_only(self):
        self.write("src/agent/skills.h", "#pragma once\n")
        self.write("src/agent/skills.cpp", '#include "skills.h"\n')
        problems = self.problems("src/agent/skills.cpp  # engine1_only: x\n")
        self.assertEqual(len(problems), 1)
        self.assertIn("skills.h", problems[0])


class UnfollowedTests(TreeTestCase):
    def test_a_macro_include_is_a_problem(self):
        self.write("src/a.cpp", "#define H <x.h>\n#include H\n")
        problems = self.problems()
        self.assertEqual(len(problems), 1)
        self.assertIn("cannot follow line 2", problems[0])

    def test_an_unresolved_nimbus_header_is_a_problem(self):
        self.write("src/a.cpp", '#include "nimbus/harness/gone.h"\n#include <Arduino.h>\n')
        problems = self.problems()
        self.assertEqual(len(problems), 1)
        self.assertIn("nimbus/harness/gone.h", problems[0])

    def test_an_unfollowable_include_inside_the_guard_is_not(self):
        self.write("src/a.cpp", GUARD + '\n#include "nimbus/harness/gone.h"\n#endif\n')
        self.assertEqual(self.problems(), [])


class TreePinTests(unittest.TestCase):
    def test_every_old_engine_header_exists(self):
        for hdr in lister.engine1_headers():
            self.assertTrue(os.path.isfile(os.path.join(REPO, hdr)), hdr)

    def test_the_tree_has_no_unguarded_includer(self):
        self.assertEqual(lister.main(["--root", REPO, "--check"]), 0)


if __name__ == "__main__":
    unittest.main()
