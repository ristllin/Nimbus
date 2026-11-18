"""Tests for tools/engine_filter.py (the device build's engine source filter).

Run: python3 -m pytest tools/test_engine_filter.py
(tools/release_gate/test_check_engine_filter.py imports these, so `pytest tools/release_gate` runs them too.)
"""

from __future__ import annotations

import os
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import engine_filter as ef  # noqa: E402

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SCRIPT = os.path.join(REPO, "tools", "engine_filter.py")


class FakeNode:
    def __init__(self, path):
        self.path = path

    def srcnode(self):
        return self

    def get_abspath(self):
        return self.path


class FakeEnv(dict):
    """The parts of a PlatformIO construction environment the script uses."""

    def __init__(self, root, engine=""):
        super().__init__(PROJECT_DIR=root, PIOENV="esp32s3-cyd")
        self.engine = engine
        self.middlewares = []
        self.exit_code = None

    def GetProjectOption(self, name, default=None):  # noqa: N802 - PlatformIO's name
        assert name == ef.ENGINE_OPTION
        return self.engine if self.engine is not None else default

    def AddBuildMiddleware(self, callback, pattern=None):  # noqa: N802
        self.middlewares.append((callback, pattern))

    def Exit(self, code):  # noqa: N802
        self.exit_code = code


def make_tree(lists):
    root = tempfile.mkdtemp(prefix="engine_filter_")
    os.makedirs(os.path.join(root, "tools", "engine"))
    for name, text in lists.items():
        with open(os.path.join(root, "tools", "engine", name), "w", encoding="utf-8") as fh:
            fh.write(text)
    return root


def run_as_extra_script(env):
    """Execute the file the way PlatformIO does: SConscript globals with Import."""
    with open(SCRIPT, "r", encoding="utf-8") as fh:
        code = compile(fh.read(), SCRIPT, "exec")
    glb = {"__name__": "SCons.Script", "__file__": SCRIPT, "Import": lambda name: None, "env": env}
    exec(code, glb)  # noqa: S102 - our own build script


class ParseTests(unittest.TestCase):
    def test_comments_and_blanks_are_skipped(self):
        text = "# head\n\nlib/a.cpp\n  lib/b.cpp  # trailing\n#lib/c.cpp\n"
        self.assertEqual(ef.parse_list(text), ["lib/a.cpp", "lib/b.cpp"])

    def test_engine_number(self):
        self.assertEqual(ef.engine_number(None), 1)
        self.assertEqual(ef.engine_number(""), 1)
        self.assertEqual(ef.engine_number(" 2 "), 2)
        for bad in ("0", "3", "e2", "1.0", "two"):
            with self.assertRaises(ef.EngineFilterError, msg=bad):
                ef.engine_number(bad)


class MiddlewareTests(unittest.TestCase):
    def test_engine1_drops_exactly_the_engine2_list(self):
        root = make_tree({"engine2_only.txt": "lib/harness/src/plain_x.cpp\n"})
        drop = ef.dropped_paths(root, 1)
        mw = ef.make_middleware(drop)
        listed = FakeNode(os.path.join(root, "lib", "harness", "src", "plain_x.cpp"))
        kept = FakeNode(os.path.join(root, "lib", "harness", "src", "engine.cpp"))
        self.assertIsNone(mw(listed))
        self.assertIs(mw(kept), kept)

    def test_path_spelling_does_not_matter(self):
        root = make_tree({"engine2_only.txt": "lib/core/src/./orch_tx_a.cpp\n"})
        mw = ef.make_middleware(ef.dropped_paths(root, 1))
        self.assertIsNone(mw(FakeNode(os.path.join(root, "lib", "core", "..", "core", "src", "orch_tx_a.cpp"))))

    def test_engine2_drops_the_engine1_list(self):
        root = make_tree({"engine2_only.txt": "lib/a2.cpp\n", "engine1_only.txt": "src/agent/e1.cpp\n"})
        mw = ef.make_middleware(ef.dropped_paths(root, 2))
        self.assertIsNone(mw(FakeNode(os.path.join(root, "src", "agent", "e1.cpp"))))
        a2 = FakeNode(os.path.join(root, "lib", "a2.cpp"))
        self.assertIs(mw(a2), a2)

    def test_missing_list_fails_closed(self):
        root = make_tree({"engine2_only.txt": "lib/a2.cpp\n"})
        with self.assertRaises(ef.EngineFilterError):
            ef.dropped_paths(root, 2)  # engine1_only.txt does not exist yet (RB-I0 adds it)


class ExtraScriptTests(unittest.TestCase):
    def test_script_registers_the_middleware_for_engine1(self):
        root = make_tree({"engine2_only.txt": "lib/x/plain_y.cpp\n"})
        env = FakeEnv(root, engine="")
        run_as_extra_script(env)
        self.assertIsNone(env.exit_code)
        self.assertEqual(len(env.middlewares), 1)
        callback, pattern = env.middlewares[0]
        self.assertIsNone(pattern)
        self.assertIsNone(callback(FakeNode(os.path.join(root, "lib", "x", "plain_y.cpp"))))

    def test_script_stops_the_build_on_a_bad_engine(self):
        root = make_tree({"engine2_only.txt": ""})
        env = FakeEnv(root, engine="7")
        run_as_extra_script(env)
        self.assertEqual(env.exit_code, 1)
        self.assertEqual(env.middlewares, [])

    def test_script_stops_the_build_on_a_missing_list(self):
        env = FakeEnv(make_tree({}), engine="1")
        run_as_extra_script(env)
        self.assertEqual(env.exit_code, 1)

    def test_import_as_a_module_runs_nothing(self):
        # Importing for tests and the gate must not need a PlatformIO env.
        self.assertFalse(hasattr(ef, "env"))

    def test_the_real_list_loads(self):
        drop = ef.dropped_paths(REPO, 1)
        self.assertIn(ef.norm(os.path.join(REPO, "lib", "harness", "src", "plain_engine.cpp")), drop)


if __name__ == "__main__":
    unittest.main()
