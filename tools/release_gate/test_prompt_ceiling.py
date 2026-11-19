"""Tests for check_prompt_ceiling.py (lane RB-Q3b): every red path, and the seeded 300 B addition."""

from __future__ import annotations

import json
import os
import shutil
import sys
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

import check_prompt_ceiling as gate  # noqa: E402

REPO = gate.REPO


def golden(
    prompt: int,
    tools: int,
    extra: str = "",
    demand: int = gate.MEASURED["prompt_demand"],
    text: int = gate.MEASURED["text"],
    wire: int = gate.MEASURED["tools_wire"],
) -> str:
    return (
        "# test golden\n"
        f"measure turn=owner_text prompt={prompt} system=1 input=1 history=1 text={text} tools={tools} "
        f"registry_tools=2 tools_raw=1 tools_wire={wire} body=1 prompt_demand={demand}\n"
        "group name=always tools=2 raw=10\n"
        "tool name=orch_turn group=final raw=5 sent=1\n"
        "tool name=files.read group=always raw=5 sent=1\n"
        "tool name=tenant.list group=people raw=5 sent=0\n" + extra
    )


FIXTURE = {
    "tools": [
        {
            "name": "files.read",
            "description": "Read a file. Returns its text.",
            "schema": '{"type":"object","required":["path"]}',
        },
        {"name": "tenant.list", "description": "List who may use the device.", "schema": '{"type":"object"}'},
    ]
}
SOURCE = (
    "void regTools(ToolRegistry& reg) {\n"
    '  reg.add("files.read", "Read a file. Returns its text.", h, "{\\"type\\":\\"object\\",\\"required\\":[\\"path\\"]}");\n'
    '  reg.add("tenant.list", std::string("List who may ") + "use the device.", h, "{\\"type\\":\\"object\\"}");\n'
    "}\n"
)


class Tree:
    """A minimal tree: the golden, the fixture, one registering source and owners.json."""

    def __init__(
        self,
        prompt: int = gate.MEASURED["prompt"],
        tools: int = gate.MEASURED["tools"],
        prompt_demand: int = gate.MEASURED["prompt_demand"],
        text: int = gate.MEASURED["text"],
        tools_wire: int = gate.MEASURED["tools_wire"],
    ):
        self.root = tempfile.mkdtemp(prefix="prompt-ceiling-")
        self.write(gate.GOLDEN, golden(prompt, tools, demand=prompt_demand, text=text, wire=tools_wire))
        self.write(gate.FIXTURE, json.dumps(FIXTURE))
        self.write("src/agent/tools.cpp", SOURCE)
        self.write(gate.OWNERS, json.dumps({"lanes": {"RB-Q3b": {}, "RB-REC8": {}}}))

    def write(self, rel: str, text: str) -> None:
        path = os.path.join(self.root, rel)
        os.makedirs(os.path.dirname(path), exist_ok=True)
        with open(path, "w", encoding="utf-8") as fh:
            fh.write(text)

    def allowance(self, file_lane: str, **fields) -> None:
        self.write(f"{gate.ALLOWANCES}/{file_lane}.json", json.dumps(fields))

    def errs(self) -> "list[str]":
        return gate.run(self.root)

    def close(self) -> None:
        shutil.rmtree(self.root)


class PromptCeilingTest(unittest.TestCase):
    def tree(self, **kw) -> Tree:
        t = Tree(**kw)
        self.addCleanup(t.close)
        return t

    def test_the_repo_is_green(self):
        self.assertEqual(gate.run(REPO), [])

    def test_the_measured_figures_are_green(self):
        self.assertEqual(self.tree().errs(), [])

    def test_a_seeded_300_byte_addition_is_red_on_either_figure(self):
        for fig in gate.MEASURED:
            figures = dict(gate.MEASURED)
            figures[fig] += 300
            errs = self.tree(**figures).errs()
            self.assertTrue(any(e.startswith(f"{fig}: ") and "over its ceiling" in e for e in errs), errs)

    def test_the_margin_is_stated_and_smaller_than_the_seeded_addition(self):
        self.assertLess(gate.MARGIN_BYTES, 300)
        for fig, cap in gate.CAPS.items():
            self.assertLessEqual(gate.MEASURED[fig], cap)

    def test_an_allowance_of_the_growing_lane_raises_the_ceiling_by_its_bytes(self):
        t = self.tree(tools=gate.MEASURED["tools"] + 300)
        t.allowance("RB-REC8", lane="RB-REC8", tools_bytes=100, why="calc, always on (ruling 45)")
        self.assertTrue(t.errs(), "100 B and the margin do not cover 300 B")
        t.allowance("RB-REC8", lane="RB-REC8", tools_bytes=300 - gate.MARGIN_BYTES, why="calc")
        self.assertEqual(t.errs(), [])

    def test_no_allowance_passes_the_cap(self):
        t = self.tree(prompt=gate.CAPS["prompt"] + 1)
        t.allowance("RB-REC8", lane="RB-REC8", prompt_bytes=10000, why="too much")
        self.assertTrue(any("cap" in e for e in t.errs()))

    def test_a_malformed_or_foreign_allowance_is_red(self):
        cases = {
            "RB-REC8": {"lane": "RB-REC1", "tools_bytes": 10, "why": "x"},  # another lane's name
            "RB-NOPE": {"lane": "RB-NOPE", "tools_bytes": 10, "why": "x"},  # not a lane
        }
        for lane, body in cases.items():
            t = self.tree()
            t.allowance(lane, **body)
            self.assertTrue(t.errs(), lane)
        for body in ({"lane": "RB-REC8", "tools_bytes": 10}, {"lane": "RB-REC8", "tools_bytes": -5, "why": "x"}):
            t = self.tree()
            t.allowance("RB-REC8", **body)
            self.assertTrue(t.errs(), body)

    def test_a_tool_that_grows_in_the_source_is_red_until_the_fixture_carries_it(self):
        t = self.tree()
        t.write("src/agent/tools.cpp", SOURCE.replace("Returns its text.", "Returns its text and more words."))
        self.assertTrue(any("files.read's description" in e for e in t.errs()))

    def test_a_schema_that_changes_in_the_source_is_red(self):
        t = self.tree()
        t.write("src/agent/tools.cpp", SOURCE.replace('[\\"path\\"]', '[]'))
        self.assertTrue(any("files.read's schema" in e for e in t.errs()))

    def test_a_registered_tool_missing_from_the_fixture_is_red(self):
        t = self.tree()
        t.write("src/agent/more.cpp", 'void m(ToolRegistry& reg) { reg.add("device.new", "New.", h, "{}"); }\n')
        self.assertTrue(any("registers device.new" in e for e in t.errs()))

    def test_a_tool_with_a_dotless_name_is_held_too(self):
        t = self.tree()
        t.write("src/agent/calc.cpp", 'void c(ToolRegistry& reg) { reg.add("calc", "Exact arithmetic.", h, "{}"); }\n')
        self.assertTrue(any("registers calc" in e for e in t.errs()))

    def test_an_engine2_only_source_is_not_an_engine1_tool(self):
        t = self.tree()
        t.write(
            "lib/core/src/orch_file_tools.cpp",
            'void f(ToolRegistry& reg) { reg.add("read_file", "Read.", h, "{}"); }\n',
        )
        self.assertTrue(any("registers read_file" in e for e in t.errs()))
        t.write(gate.ENGINE2_ONLY, "lib/core/src/orch_file_tools.cpp\n")
        self.assertFalse(any("read_file" in e for e in t.errs()))

    def test_a_golden_that_names_a_tool_the_fixture_lacks_is_red(self):
        t = self.tree()
        t.write(gate.GOLDEN, golden(1, 1, "tool name=ghost.tool group=always raw=5 sent=1\n"))
        self.assertTrue(any("ghost.tool" in e for e in t.errs()))

    def test_a_missing_or_malformed_golden_is_red(self):
        t = self.tree()
        os.remove(os.path.join(t.root, gate.GOLDEN))
        self.assertTrue(t.errs())
        t.write(gate.GOLDEN, "measure turn=owner_text prompt=abc\n")
        self.assertTrue(t.errs())
        t.write(gate.GOLDEN, "measure turn=owner_text system=5\n")
        self.assertTrue(any("no prompt figure" in e for e in t.errs()))

    def test_a_literal_appended_to_a_registration_is_red(self):
        t = self.tree()
        t.write(
            "src/agent/tools.cpp",
            SOURCE.replace(
                '"Read a file. Returns its text.",', '"Read a file. Returns its text." + std::string(" EXTRA"),'
            ),
        )
        self.assertTrue(any("files.read's description" in e for e in t.errs()))

    def test_a_run_time_piece_matches_any_text_but_the_literals_around_it_are_fixed(self):
        consts = {"kTail": "use the device.", "": set()}
        pat, fixed = gate.pattern_of('std::string("List who may ") + keysDoc() + kTail', consts)
        self.assertTrue(fixed)
        self.assertTrue(pat.match("List who may (anything) use the device."))
        self.assertFalse(pat.match("List who may (anything) use the device. More."))
        self.assertFalse(pat.match("List people who may use the device."))

    def test_an_argument_with_no_fixed_text_must_be_a_literal_of_the_sources(self):
        self.assertTrue(gate._matches("t.schemaJson", '{"a":1}', {"": {'{"a":1}'}}))
        self.assertFalse(gate._matches("t.schemaJson", '{"a":2}', {"": {'{"a":1}'}}))

    def test_a_tool_registered_under_a_run_time_name_is_red_unless_listed(self):
        t = self.tree()
        t.write("src/agent/more.cpp", 'void m(ToolRegistry& reg) { reg.add(kName2, "New.", h, "{}"); }\n')
        self.assertTrue(any("not a literal (kName2)" in e for e in t.errs()))
        t.write(
            "src/agent/more.cpp",
            'const char kName2[] = "calc.eval";\nvoid m(ToolRegistry& reg) { reg.add(kName2, "New.", h, "{}"); }\n',
        )
        self.assertTrue(any("registers calc.eval" in e for e in t.errs()))

    def test_a_source_that_cannot_be_read_is_red(self):
        t = self.tree()
        t.write("src/agent/bad.cpp", 'void m(ToolRegistry& reg) { reg.add("x.y", "unterminated, h); }\n')
        self.assertTrue(any("cannot be read" in e for e in t.errs()))

    def test_a_large_allowance_names_a_ruling(self):
        t = self.tree(tools=gate.MEASURED["tools"] + 300)
        t.allowance("RB-REC8", lane="RB-REC8", tools_bytes=2000, why="big")
        self.assertTrue(any("name the ruling" in e for e in t.errs()))
        t.allowance("RB-REC8", lane="RB-REC8", tools_bytes=2000, why="big", ruling="Ruling 45")
        self.assertFalse(any("name the ruling" in e for e in t.errs()))


if __name__ == "__main__":
    unittest.main()
