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
 .literal.esp_tls_plain_tcp_connect
"""


class Tree:
    def __init__(self, listed=LISTED, extra="", sources=SOURCES, ini=None):
        self.root = tempfile.mkdtemp(prefix="engine_gate_")
        for rel in sources:
            self.write(rel, "// source\n")
        if listed is not None:
            self.write("tools/engine/engine2_only.txt", listed)
        self.write("platformio.ini", ini if ini is not None else INI.format(extra=extra))

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


class Base(unittest.TestCase):
    def tree(self, **kw):
        t = Tree(**kw)
        self.addCleanup(t.close)
        return t

    def assertFails(self, t, needle, require=()):
        ok, _, fails = t.check(require)
        self.assertFalse(ok)
        self.assertTrue(any(needle in f for f in fails), fails)

    def assertPasses(self, t, require=()):
        ok, lines, fails = t.check(require)
        self.assertTrue(ok, fails)
        return lines


class ListTests(Base):
    def test_clean_tree_and_clean_map_pass(self):
        t = self.tree()
        t.map("esp32s3-cyd", CLEAN_MAP)
        lines = self.assertPasses(t, require=["esp32s3-cyd"])
        self.assertIn("env:esp32s3-cyd: engine 1 firmware.map names no engine-2 object", lines)

    def test_unlisted_engine2_source_fails(self):
        for rel in (
            "lib/core/src/orch_tx_new.cpp",
            "lib/core/src/orch_tx/deep.cpp",
            "lib/harness/src/plain_new.cxx",
            "lib/harness/src/plain/inner.cpp",
            "lib/harness/src/plain_mod/inner.c",
            "lib/harness/src/providers/plain_wire_new.cpp",
            "lib/harness/src/plain.cpp",
        ):
            t = self.tree(sources=SOURCES + (rel,))
            self.assertFails(t, f"{rel} is an engine-2 source not in")

    def test_engine1_and_non_sources_are_not_required(self):
        # Counter-case: only family sources count, not a header, a lookalike or an engine-1 file.
        extra = (
            "lib/harness/src/plain_notes.h",
            "lib/harness/src/planner.cpp",
            "lib/harness/src/explain_plain.cpp",
            "lib/core/src/orch_txn.cpp",
            "lib/core/src/plain_x.cpp",
            "src/agent/plain_bridge.cpp",
        )
        self.assertPasses(self.tree(sources=SOURCES + extra))

    def test_stale_entry_fails(self):
        self.assertFails(self.tree(listed=LISTED + "lib/harness/src/plain_gone.cpp\n"), "plain_gone.cpp does not exist")

    def test_duplicate_and_unclean_entries_fail(self):
        self.assertFails(self.tree(listed=LISTED + "lib/core/src/orch_tx_item.cpp\n"), "listed twice")
        self.assertFails(self.tree(listed=LISTED + "lib/core/../core/src/orch_tx_item.cpp\n"), "not a clean repo path")
        self.assertFails(self.tree(listed=LISTED + "/abs/x.cpp\n"), "not a clean repo path")
        self.assertFails(self.tree(listed=LISTED + "tools/x.cpp\n", sources=SOURCES + ("tools/x.cpp",)), "under lib/")
        t = self.tree(listed=LISTED + "lib/harness/src/plain_x.h\n", sources=SOURCES + ("lib/harness/src/plain_x.h",))
        self.assertFails(t, "not a source PlatformIO compiles")

    def test_missing_list_fails(self):
        self.assertFails(self.tree(listed=None), "tools/engine/engine2_only.txt is missing")

    def test_engine1_list_required_once_an_env_builds_engine2(self):
        t = self.tree(extra="custom_nimbus_engine = 2")
        self.assertFails(t, "tools/engine/engine1_only.txt is missing")
        t.write("tools/engine/engine1_only.txt", "src/main.cpp\n")
        self.assertPasses(t)

    def test_engine1_list_is_checked_and_disjoint(self):
        t = self.tree()
        t.write("tools/engine/engine1_only.txt", "src/gone.cpp\nlib/core/src/orch_tx_item.cpp\n")
        self.assertFails(t, "src/gone.cpp does not exist")
        self.assertFails(t, "lib/core/src/orch_tx_item.cpp is on both engine lists")


class HookTests(Base):
    def test_env_overriding_extra_scripts_without_the_hook_fails(self):
        t = self.tree(extra="extra_scripts = pre:tools/git_version.py")
        self.assertFails(t, "env:test-cyd: extra_scripts lacks pre:tools/engine_filter.py")

    def test_env_overriding_extra_scripts_with_the_hook_passes(self):
        self.assertPasses(self.tree(extra="extra_scripts = pre:tools/engine_filter.py\n    post:tools/x.py"))

    def test_interpolated_scripts_resolve(self):
        self.assertPasses(self.tree(extra="extra_scripts =\n    ${s3.extra_scripts}\n    post:tools/x.py"))

    def test_hook_hidden_in_an_inline_comment_fails(self):
        # PlatformIO drops `; ...` after whitespace, so this env never runs the hook.
        t = self.tree(extra="extra_scripts = pre:tools/git_version.py ; pre:tools/engine_filter.py")
        self.assertFails(t, "env:test-cyd: extra_scripts lacks")

    def test_multi_extends_and_env_defaults_are_followed(self):
        ini = (
            "[env]\ncustom_nimbus_engine = 3\n\n[common]\nbuild_flags = -DX\n\n"
            "[s3]\nplatform = espressif32\nextra_scripts = pre:tools/git_version.py\n\n"
            "[env:board]\nextends = common, s3\n"
        )
        t = self.tree(ini=ini)
        self.assertFails(t, "env:board: extra_scripts lacks")
        self.assertFails(t, "env:board: custom_nimbus_engine = '3'")

    def test_device_env_without_s3_still_counts(self):
        # Device means "platform is not native", not "extends [s3]".
        t = self.tree(extra="\n[env:raw]\nplatform = espressif32\n")
        self.assertFails(t, "env:raw: extra_scripts lacks")

    def test_bad_engine_option_fails_without_crashing_the_map_pass(self):
        t = self.tree(extra="custom_nimbus_engine = 3")
        t.map("test-cyd", CLEAN_MAP)
        self.assertFails(t, "must be 1 or 2")

    def test_inline_comment_after_the_engine_is_dropped(self):
        # As PlatformIO reads it: engine 2, not a bad value.
        t = self.tree(extra="custom_nimbus_engine = 2 ; the e2 env")
        t.write("tools/engine/engine1_only.txt", "src/main.cpp\n")
        self.assertEqual(gate.PioIni(os.path.join(t.root, "platformio.ini")).engine("test-cyd"), 2)
        self.assertPasses(t)

    def test_unresolvable_reference_fails_loudly(self):
        self.assertFails(self.tree(extra="custom_nimbus_engine = ${sysenv.NIMBUS_ENGINE}"), "cannot be resolved")


class MapTests(Base):
    def test_loaded_engine2_object_fails(self):
        t = self.tree()
        t.map(
            "esp32s3-cyd",
            CLEAN_MAP + " .eh_frame 0x3c0a0040 0xe0 .pio/build/esp32s3-cyd/lib328/harness/plain_engine.cpp.o\n",
        )
        self.assertFails(t, "engine 1 firmware.map names 1 engine-2 object(s): lib/harness/src/plain_engine.cpp")

    def test_wrapped_path_line_fails(self):
        t = self.tree()
        t.map(
            "esp32s3-cyd",
            CLEAN_MAP
            + " .text._ZN5plain3runEv\n                .pio/build/esp32s3-cyd/lib9b1/core/orch_tx_item.cpp.o\n",
        )
        self.assertFails(t, "lib/core/src/orch_tx_item.cpp")

    def test_archive_member_fails(self):
        t = self.tree()
        t.map("test-cyd", CLEAN_MAP + " .eh_frame 0x0 0x38 .pio/build/test-cyd/lib9b1/libcore.a(orch_tx_item.cpp.o)\n")
        self.assertFails(t, "env:test-cyd: engine 1 firmware.map names 1 engine-2 object(s): orch_tx_item.cpp.o")

    def test_unlisted_family_object_in_a_map_fails(self):
        # The source is gone from the tree, but a stale or odd build still links it.
        t = self.tree()
        t.map("esp32s3-cyd", CLEAN_MAP + ".pio/build/esp32s3-cyd/lib328/harness/providers/plain_wire_x.cpp.o\n")
        self.assertFails(t, "lib/harness/src/providers/plain_wire_x.cpp")

    def test_similar_names_are_not_engine2(self):
        # Counter-case: engine-1 objects and framework symbols that only look alike.
        t = self.tree()
        t.map(
            "esp32s3-cyd",
            CLEAN_MAP
            + ".pio/build/esp32s3-cyd/lib328/harness/explain_engine.cpp.o\n"
            + ".pio/build/esp32s3-cyd/lib9b1/core/orch_txn.cpp.o\n"
            + ".pio/build/esp32s3-cyd/src/agent/plain_engine.cpp.o\n"
            + "/x/lib/libesp-tls.a(esp_tls.c.obj)\n",
        )
        self.assertPasses(t)

    def test_engine2_env_is_checked_against_the_engine1_list(self):
        t = self.tree(extra="custom_nimbus_engine = 2")
        t.write("tools/engine/engine1_only.txt", "src/main.cpp\n")
        t.map("test-cyd", ".pio/build/test-cyd/lib328/harness/plain_engine.cpp.o\n")
        lines = self.assertPasses(t)
        self.assertIn("env:test-cyd: engine 2 firmware.map names no engine-1 object", lines)
        t.map("test-cyd", CLEAN_MAP)  # names src/main.cpp.o
        self.assertFails(t, "env:test-cyd: engine 2 firmware.map names 1 engine-1 object(s): src/main.cpp")

    def test_required_map_missing_fails(self):
        self.assertFails(self.tree(), "is not built (required)", require=["esp32s3-cyd"])
        self.assertFails(self.tree(), "not a device env", require=["native"])

    def test_no_map_passes_with_a_note(self):
        lines = self.assertPasses(self.tree())
        self.assertTrue(any("ran on nothing" in line for line in lines), lines)

    def test_cli_exit_codes(self):
        t = self.tree()
        self.assertEqual(gate.main(["--root", t.root]), 0)
        t.map("esp32s3-cyd", ".pio/build/esp32s3-cyd/lib9b1/core/orch_tx_item.cpp.o\n")
        self.assertEqual(gate.main(["--root", t.root]), 1)


class RepoTests(unittest.TestCase):
    def setUp(self):
        self.ini = gate.PioIni(os.path.join(REPO, "platformio.ini"))

    def test_the_repo_lists_and_hooks_are_clean(self):
        # The real tree; maps are checked only where built, so this needs no build.
        _, fails = gate.check_lists(REPO, engine1_list_needed=False)
        self.assertEqual(fails, [])
        self.assertEqual(gate.check_hooks(self.ini), [])

    def test_every_full_device_env_is_covered(self):
        envs = self.ini.device_envs()
        for env in ("esp32s3", "esp32s3-cyd", "esp32s3-cyd-35", "esp32s3-cyd-40", "test", "test-cyd"):
            self.assertIn(env, envs)
        self.assertNotIn("native", envs)

    def test_resolution_matches_platformio(self):
        try:
            from platformio.project.config import ProjectConfig
        except ImportError:
            self.skipTest("PlatformIO is not importable here (CI's pytest-only venv)")
        pio = ProjectConfig(os.path.join(REPO, "platformio.ini"))
        for env in pio.envs():
            self.assertEqual(self.ini.scripts(env), pio.get("env:" + env, "extra_scripts"), env)
            self.assertEqual(
                (self.ini.get("env:" + env, "platform") or "").strip(), pio.get("env:" + env, "platform"), env
            )


if __name__ == "__main__":
    unittest.main()
