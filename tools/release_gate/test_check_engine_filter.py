"""Tests for check_engine_filter.py, each failure class with its counter-case.

Also collects tools/test_engine_filter.py, so `pytest tools/release_gate` (CI's gate-logic job)
runs the filter's own tests.
"""

from __future__ import annotations

import os
import shutil
import sys
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.dirname(HERE))

import check_engine_filter as gate  # noqa: E402
from test_engine_filter import ExtraScriptTests, MiddlewareTests, ParseTests  # noqa: E402,F401

REPO = os.path.dirname(os.path.dirname(HERE))

INI = """\
[env:native]
platform = native

[s3]
platform = espressif32
extra_scripts =
    pre:tools/git_version.py
    ; a comment line inside the value
    pre:tools/engine_filter.py

[env:esp32s3-cyd]
extends = s3

[env:test-cyd]
extends = s3
{extra}
"""

SOURCES = (
    "lib/core/src/orch_tx_item.cpp",
    "lib/harness/src/plain_engine.cpp",
    "lib/harness/src/engine.cpp",
    "src/main.cpp",
)
LISTED = "lib/core/src/orch_tx_item.cpp\nlib/harness/src/plain_engine.cpp\n"

CLEAN_MAP = """\
 .text          0x00000000        0x0 .pio/build/esp32s3-cyd/lib328/harness/engine.cpp.o
 .eh_frame      0x3c0a0000       0x40 .pio/build/esp32s3-cyd/src/main.cpp.o
 .text          0x00000000       0x10 /x/framework/lib/libcxx.a(cxx_guards.cpp.o)
"""


class Tree:
    def __init__(self, listed=LISTED, extra="", sources=SOURCES):
        self.root = tempfile.mkdtemp(prefix="engine_gate_")
        for rel in sources:
            self.write(rel, "// source\n")
        self.write("tools/engine/engine2_only.txt", listed)
        self.write("platformio.ini", INI.format(extra=extra))

    def write(self, rel, text):
        path = os.path.join(self.root, rel)
        os.makedirs(os.path.dirname(path), exist_ok=True)
        with open(path, "w", encoding="utf-8") as fh:
            fh.write(text)

    def map(self, env, text):
        self.write(os.path.join(".pio", "build", env, "firmware.map"), text)

    def check(self, require=()):
        return gate.check(self.root, os.path.join(self.root, ".pio", "build"), require)

    def close(self):
        shutil.rmtree(self.root, ignore_errors=True)


class GateTests(unittest.TestCase):
    def tree(self, **kw):
        t = Tree(**kw)
        self.addCleanup(t.close)
        return t

    def assertFails(self, t, needle, require=()):
        ok, _, fails = t.check(require)
        self.assertFalse(ok)
        self.assertTrue(any(needle in f for f in fails), fails)

    def test_clean_tree_and_clean_map_pass(self):
        t = self.tree()
        t.map("esp32s3-cyd", CLEAN_MAP)
        ok, lines, fails = t.check(require=["esp32s3-cyd"])
        self.assertTrue(ok, fails)
        self.assertIn("env:esp32s3-cyd: firmware.map names no engine-2 object", lines)

    # 1. the list
    def test_unlisted_engine2_source_fails(self):
        for rel in (
            "lib/core/src/orch_tx_new.cpp",
            "lib/harness/src/plain_new.cpp",
            "lib/harness/src/providers/plain_wire_new.cpp",
        ):
            t = self.tree(sources=SOURCES + (rel,))
            self.assertFails(t, f"{rel} matches")

    def test_engine2_header_or_unrelated_source_is_not_required(self):
        # Counter-case: only sources of the families count, not a header or an engine-1 file.
        t = self.tree(sources=SOURCES + ("lib/harness/src/plain_notes.h", "lib/harness/src/planner.cpp"))
        ok, _, fails = t.check()
        self.assertTrue(ok, fails)

    def test_stale_entry_fails(self):
        t = self.tree(listed=LISTED + "lib/harness/src/plain_gone.cpp\n")
        self.assertFails(t, "plain_gone.cpp does not exist")

    def test_duplicate_and_unclean_entries_fail(self):
        self.assertFails(self.tree(listed=LISTED + "lib/core/src/orch_tx_item.cpp\n"), "listed twice")
        self.assertFails(self.tree(listed=LISTED + "lib/core/../core/src/orch_tx_item.cpp\n"), "not a clean repo path")
        self.assertFails(self.tree(listed=LISTED + "/abs/x.cpp\n"), "not a clean repo path")
        t = self.tree(listed=LISTED + "lib/harness/src/plain_x.h\n", sources=SOURCES + ("lib/harness/src/plain_x.h",))
        self.assertFails(t, "not a .c/.cc/.cpp source")

    def test_missing_list_fails(self):
        t = self.tree()
        os.remove(os.path.join(t.root, "tools", "engine", "engine2_only.txt"))
        self.assertFails(t, "is missing")

    # 2. the hook
    def test_env_overriding_extra_scripts_without_the_hook_fails(self):
        t = self.tree(extra="extra_scripts = pre:tools/git_version.py")
        self.assertFails(t, "env:test-cyd: extra_scripts lacks pre:tools/engine_filter.py")

    def test_env_overriding_extra_scripts_with_the_hook_passes(self):
        t = self.tree(extra="extra_scripts = pre:tools/engine_filter.py\n    post:tools/x.py")
        ok, _, fails = t.check()
        self.assertTrue(ok, fails)

    def test_bad_engine_option_fails(self):
        t = self.tree(extra="custom_nimbus_engine = 3")
        t.map("test-cyd", CLEAN_MAP)  # the map pass must not crash on the bad option
        self.assertFails(t, "must be 1 or 2")

    # 3. the maps
    def test_loaded_engine2_object_fails(self):
        t = self.tree()
        t.map(
            "esp32s3-cyd",
            CLEAN_MAP + " .eh_frame 0x3c0a0040 0xe0 .pio/build/esp32s3-cyd/lib328/harness/plain_engine.cpp.o\n",
        )
        self.assertFails(t, "harness/plain_engine.cpp.o")

    def test_archive_member_fails(self):
        t = self.tree()
        t.map("test-cyd", CLEAN_MAP + " .eh_frame 0x0 0x38 .pio/build/test-cyd/lib9b1/libcore.a(orch_tx_item.cpp.o)\n")
        self.assertFails(t, "env:test-cyd: firmware.map names 1 engine-2 object(s): orch_tx_item.cpp.o")

    def test_unlisted_glob_object_in_a_map_fails(self):
        # The source is gone from the tree, but a map still links it: the map check uses the families too.
        t = self.tree()
        t.map("esp32s3-cyd", CLEAN_MAP + ".pio/build/esp32s3-cyd/lib328/harness/providers/plain_wire_x.cpp.o\n")
        self.assertFails(t, "harness/providers/plain_wire_x.cpp.o")

    def test_similar_names_are_not_engine2(self):
        # Counter-case: engine-1 objects that only look alike.
        t = self.tree()
        t.map(
            "esp32s3-cyd",
            CLEAN_MAP
            + ".pio/build/esp32s3-cyd/lib328/harness/explain_engine.cpp.o\n"
            + ".pio/build/esp32s3-cyd/lib9b1/core/orch_txn.cpp.o\n"
            + ".pio/build/esp32s3-cyd/src/agent/plain_engine.cpp.o\n",
        )
        ok, _, fails = t.check()
        self.assertTrue(ok, fails)

    def test_engine2_env_map_is_skipped(self):
        t = self.tree(extra="custom_nimbus_engine = 2")
        t.map("test-cyd", ".pio/build/test-cyd/lib328/harness/plain_engine.cpp.o\n")
        ok, lines, fails = t.check()
        self.assertTrue(ok, fails)
        self.assertIn("env:test-cyd: engine 2, not an engine-1 image (skipped)", lines)

    def test_required_map_missing_fails(self):
        self.assertFails(self.tree(), "is not built (required)", require=["esp32s3-cyd"])
        self.assertFails(self.tree(), "not a device env", require=["native"])

    def test_cli_exit_codes(self):
        t = self.tree()
        self.assertEqual(gate.main(["--root", t.root]), 0)
        t.map("esp32s3-cyd", ".pio/build/esp32s3-cyd/lib9b1/core/orch_tx_item.cpp.o\n")
        self.assertEqual(gate.main(["--root", t.root]), 1)


class RepoTests(unittest.TestCase):
    def test_the_repo_list_and_hooks_are_clean(self):
        # The real tree; maps are checked only where built, so this needs no build.
        _, fails = gate.check_list(REPO)
        self.assertEqual(fails, [])
        self.assertEqual(gate.check_hooks(gate.parse_ini(os.path.join(REPO, "platformio.ini"))), [])

    def test_every_full_device_env_is_covered(self):
        envs = gate.device_envs(gate.parse_ini(os.path.join(REPO, "platformio.ini")))
        for env in ("esp32s3", "esp32s3-cyd", "esp32s3-cyd-35", "esp32s3-cyd-40", "test", "test-cyd"):
            self.assertIn(env, envs)
        self.assertNotIn("native", envs)


if __name__ == "__main__":
    unittest.main()
