"""Engine 2's /usage and /budget never print engine 1's state (lane E2-FIX5, bench part 2).

The history profile (/data/budget.json, lane RB-BG) is engine 1's: engine 2 never reads it. The
look that /usage and /budget answer with (src/agent/budget_command.cpp, BudgetVerb::Show) ended with
budgetprofile::statusLine() on both engines. Every call of it in the device's budget command must sit
in the engine-1 guard's first branch, so an engine-1 build preprocesses to the same code and an
engine-2 build prints no profile line.
"""

from __future__ import annotations

import os
import re
import sys
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, os.path.join(REPO, "tools", "engine"))

import list_engine1_includes as lister  # noqa: E402
import list_tool_registrations as regs  # noqa: E402

CALL = re.compile(r"\bbudgetprofile::statusLine\s*\(")
GUARD = "#if !defined(NIMBUS_ENGINE) || NIMBUS_ENGINE == 1"


def unguarded_calls(text: str):
    stripped = lister.strip_comments(text)
    flags = regs.guarded_flags(stripped)
    out = []
    for m in CALL.finditer(stripped):
        line = stripped.count("\n", 0, m.start()) + 1
        if not flags[line]:
            out.append(line)
    return out


class Engine2BudgetTextTests(unittest.TestCase):
    def test_the_device_budget_command_prints_the_profile_on_engine_1_only(self):
        with open(os.path.join(REPO, "src", "agent", "budget_command.cpp"), encoding="utf-8") as fh:
            text = fh.read()
        self.assertTrue(CALL.search(text), "engine 1 still shows its profile line")
        self.assertEqual(unguarded_calls(text), [])

    def test_a_call_outside_the_guard_is_caught(self):
        self.assertEqual(unguarded_calls('void f() {\n  x = budgetprofile::statusLine();\n}\n'), [2])

    def test_a_call_in_the_else_branch_is_engine_2_and_caught(self):
        text = GUARD + "\n  a();\n#else\n  x = budgetprofile::statusLine();\n#endif\n"
        self.assertEqual(unguarded_calls(text), [4])

    def test_a_call_in_the_first_branch_is_engine_1(self):
        text = GUARD + "\n  x = budgetprofile::statusLine();\n#else\n  y();\n#endif\n"
        self.assertEqual(unguarded_calls(text), [])


if __name__ == "__main__":
    unittest.main()
