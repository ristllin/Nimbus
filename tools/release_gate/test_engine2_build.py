"""Tests for the engine-2 device builds (lane RB-I0), each check with its counter-case, and the tree
pins:
  - tools/engine/list_tool_registrations.py: no old-engine tool registers on an engine-2 build
    (every registration site in src/ is engine-1 guarded, in an engine1_only file, or in an
    engine-2 binding file);
  - check_elf_symbols.py --engine1-absent: an engine-2 ELF defines no symbol of e1_symbols.txt;
  - check_tg_poll_stack.py: a path row measures one engine, an image is checked on its own rows;
  - platformio.ini: the six *-e2 envs build engine 2 with -DNIMBUS_ENGINE=2, every other device
    env builds engine 1 with no NIMBUS_ENGINE define (engine-1 command lines unchanged);
  - tools/engine/engine1_only.txt covers RB-SEAM2's engine1_only sources and the library sources
    behind the old-engine headers;
  - static_dram_ceilings.json has a row for every engine-2 env.
"""

from __future__ import annotations

import json
import os
import shutil
import sys
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, os.path.join(REPO, "tools", "engine"))
sys.path.insert(0, HERE)

import check_elf_symbols  # noqa: E402
import check_static_dram  # noqa: E402
import check_engine_filter  # noqa: E402
import check_tg_poll_stack  # noqa: E402
import list_engine1_includes as lister  # noqa: E402
import list_tool_registrations as regs  # noqa: E402

GUARD = "#if !defined(NIMBUS_ENGINE) || NIMBUS_ENGINE == 1"
E2_ENVS = {
    "esp32s3-e2": "esp32s3",
    "esp32s3-cyd-e2": "esp32s3-cyd",
    "esp32s3-cyd-35-e2": "esp32s3-cyd-35",
    "esp32s3-cyd-40-e2": "esp32s3-cyd-40",
    "test-e2": "test",
    "test-cyd-e2": "test-cyd",
}


def statuses(text):
    return [(what, g) for _line, what, g in regs.sites(text)]


class RegistrationSiteTests(unittest.TestCase):
    def test_an_unguarded_add_on_the_registry_is_a_site(self):
        self.assertEqual(statuses('void f() {\n  g_reg.add("a.b", "d", h);\n}\n'), [("add()", False)])

    def test_an_add_inside_the_guard_is_guarded(self):
        text = "void f() {\n" + GUARD + '\n  reg.add("a.b", "d", h);\n#endif\n}\n'
        self.assertEqual(statuses(text), [("add()", True)])

    def test_the_else_branch_of_the_guard_is_engine_2(self):
        text = GUARD + "\n#else\n  registry().add(name, d, h);\n#endif\n"
        self.assertEqual(statuses(text), [("add()", False)])

    def test_another_conditional_inside_the_guard_stays_guarded(self):
        text = GUARD + "\n#ifdef NIMBUS_TEST\n  reg.add(n, d, h);\n#endif\n#endif\n"
        self.assertEqual(statuses(text), [("add()", True)])

    def test_any_receiver_with_a_dotted_tool_name_literal_is_a_site(self):
        self.assertEqual(statuses('  r->add("media.play", "d", h);\n'), [("add()", False)])

    def test_any_receiver_named_like_a_registry_is_a_site(self):
        text = "  registry.add(n, d, h);\n  toolReg->add(n, d, h);\n  my_reg.add(n, d, h);\n"
        self.assertEqual(statuses(text), [("add()", False)] * 3)

    def test_an_add_on_something_else_is_not_a_site(self):
        self.assertEqual(statuses("  items.add(x);\n  set.add(\"plain\");\n"), [])

    def test_a_commented_registration_is_not_a_site(self):
        self.assertEqual(statuses('// reg.add("a.b", d, h);\n/* registerTools(); */\n'), [])

    def test_a_helper_call_is_a_site_its_declaration_and_definition_are_not(self):
        text = "void registerTools();\nstatic void registerTenantTools() {\n}\nvoid begin() {\n  registerTools();\n}\n"
        self.assertEqual(statuses(text), [("registerTools()", False)])
        self.assertEqual(statuses("  nimbus::orch::registerCalcTool(g_reg);\n"), [("registerCalcTool()", False)])

    def test_the_engine_tool_hooks_are_not_sites(self):
        self.assertEqual(statuses("  tools::registerMemoryFamilies(g_reg, ctx);\n"), [])


class RegistrationTreeTests(unittest.TestCase):
    def tree(self, files):
        root = tempfile.mkdtemp()
        self.addCleanup(shutil.rmtree, root)
        for rel, text in files.items():
            os.makedirs(os.path.dirname(os.path.join(root, rel)), exist_ok=True)
            with open(os.path.join(root, rel), "w", encoding="utf-8") as fh:
                fh.write(text)
        return root

    def run_check(self, root):
        return regs.main(["--root", root, "--check"])

    def test_an_unguarded_registration_fails_the_check(self):
        root = self.tree(
            {
                "tools/engine/engine1_only.txt": "src/agent/old.cpp\n",
                "src/agent/old.cpp": 'void f() { reg.add("a.b", d, h); }\n',
                "src/agent/neutral.cpp": 'void g() { reg.add("c.d", d, h); }\n',
            }
        )
        self.assertEqual(self.run_check(root), 1)

    def test_engine1_only_guarded_and_engine2_files_pass(self):
        root = self.tree(
            {
                "tools/engine/engine1_only.txt": "src/agent/old.cpp\n",
                "src/agent/old.cpp": 'void f() { reg.add("a.b", d, h); }\n',
                "src/agent/neutral.cpp": GUARD + '\nvoid g() { reg.add("c.d", d, h); }\n#endif\n',
                regs.ENGINE2_REGISTRATION_FILES[0]: 'void k() { reg.add("e.f", d, h); }\n',
            }
        )
        self.assertEqual(self.run_check(root), 0)

    def test_a_missing_engine1_list_fails_the_check(self):
        root = self.tree({"src/agent/neutral.cpp": "int x;\n"})
        self.assertEqual(self.run_check(root), 1)

    def test_the_tree_registers_no_old_tool_on_engine_2(self):
        rows = regs.classify(REPO, lister.read_engine1_only(REPO))
        bad = [r for r in rows if r[3] == "UNGUARDED"]
        self.assertEqual(bad, [])
        # The counter-check: the scanner does see the old registrations (memory, files, device, media).
        files = {r[0] for r in rows if r[3] == "guarded"}
        for rel in (
            "src/agent/memory_subsystem.cpp",
            "src/agent/files_subsystem.cpp",
            "src/main.cpp",
            "src/sfx/music.cpp",
        ):
            self.assertIn(rel, files)


NM = """\
00000000 T _ZN5agent10TurnEngine7runTurnEv
00000010 t _ZN12_GLOBAL__N_14lockEv
00000020 W _ZN5agent4Shared5shareEv
00000030 T app_main
"""


class EngineOneAbsentTests(unittest.TestCase):
    def test_a_listed_symbol_in_the_image_fails(self):
        ok, msgs = check_elf_symbols.check_engine1_absent(NM, ["_ZN5agent10TurnEngine7runTurnEv"])
        self.assertFalse(ok)
        self.assertIn("1 engine-1 symbol", msgs[0])

    def test_an_image_without_the_listed_symbols_passes(self):
        ok, _ = check_elf_symbols.check_engine1_absent(NM, ["_ZN5agent7compose7composeEv"])
        self.assertTrue(ok)

    def test_an_empty_list_fails(self):
        ok, _ = check_elf_symbols.check_engine1_absent(NM, [])
        self.assertFalse(ok)

    def test_an_image_with_no_symbols_fails(self):
        ok, _ = check_elf_symbols.check_engine1_absent("", ["_ZN5agent7compose7composeEv"])
        self.assertFalse(ok)

    def test_the_committed_list_is_the_engine1_objects_symbols(self):
        listed = check_elf_symbols.read_e1_symbols()
        self.assertGreater(len(listed), 300)
        self.assertEqual(listed, sorted(listed))
        self.assertEqual(len(listed), len(set(listed)))
        sources = set(lister.read_engine1_only(REPO))
        with open(check_elf_symbols.E1_SYMBOLS, encoding="utf-8") as fh:
            for line in fh:
                body, _, src = line.partition("#")
                if body.strip():
                    self.assertIn(src.strip(), sources, line)
        # The old engine's own entry points are on it.
        self.assertTrue(any("TurnEngine" in s for s in listed))


class StackEngineTests(unittest.TestCase):
    CFG = {"paths": [{"name": "a", "through": ["x"]}, {"name": "b", "engine": 2, "through": ["y"]}]}

    def test_an_engine1_image_keeps_the_rows_with_no_engine(self):
        self.assertEqual([r["name"] for r in check_tg_poll_stack.for_engine(self.CFG, 1)["paths"]], ["a"])

    def test_an_engine2_image_gets_only_engine2_rows(self):
        self.assertEqual([r["name"] for r in check_tg_poll_stack.for_engine(self.CFG, 2)["paths"]], ["b"])

    def test_the_engine_of_an_image_is_its_envs(self):
        self.assertEqual(check_tg_poll_stack.engine_of(os.path.join(REPO, ".pio/build/test-cyd-e2/firmware.elf")), 2)
        self.assertEqual(check_tg_poll_stack.engine_of(os.path.join(REPO, ".pio/build/esp32s3-cyd/firmware.elf")), 1)
        self.assertEqual(check_tg_poll_stack.engine_of("/tmp/elsewhere/firmware.elf"), 1)

    def test_the_engine2_fragment_adds_only_engine2_rows_and_envs(self):
        with open(os.path.join(HERE, "tg_poll_stack.json"), encoding="utf-8") as fh:
            base = json.load(fh)
        cfg = check_tg_poll_stack.load_config()
        e1 = check_tg_poll_stack.for_engine(cfg, 1)["paths"]
        self.assertEqual(
            [r["name"] for r in e1],
            [r["name"] for r in base["paths"]] + self._fragment_e1_rows(),
        )
        with open(os.path.join(HERE, "tg_poll_stack.d", "RB-E2.json"), encoding="utf-8") as fh:
            frag = json.load(fh)
        self.assertTrue(all(r.get("engine") == 2 for r in frag["paths"]))
        self.assertEqual(set(frag["envs"]), set(E2_ENVS))
        self.assertTrue(set(E2_ENVS) <= set(cfg["envs"]))

    def _fragment_e1_rows(self):
        names = []
        frag_dir = os.path.join(HERE, "tg_poll_stack.d")
        for name in sorted(os.listdir(frag_dir)):
            if name.endswith(".json"):
                with open(os.path.join(frag_dir, name), encoding="utf-8") as fh:
                    names += [r["name"] for r in json.load(fh).get("paths", []) if r.get("engine", 1) == 1]
        return names


class IniTests(unittest.TestCase):
    def setUp(self):
        self.ini = check_engine_filter.PioIni(os.path.join(REPO, "platformio.ini"))

    def test_the_six_e2_envs_build_engine_2_on_their_twins(self):
        for env, twin in E2_ENVS.items():
            self.assertEqual(self.ini.engine(env), 2, env)
            self.assertIn("-DNIMBUS_ENGINE=2", self.ini.get("env:" + env, "build_flags").split(), env)
            self.assertEqual(self.ini.get("env:" + env, "extends").strip(), "env:" + twin)
            twin_flags = self.ini.get("env:" + twin, "build_flags").split()
            self.assertEqual(self.ini.get("env:" + env, "build_flags").split(), twin_flags + ["-DNIMBUS_ENGINE=2"])

    def test_every_other_device_env_is_engine_1_with_no_engine_define(self):
        for env in self.ini.device_envs():
            flags = (self.ini.get("env:" + env, "build_flags") or "").split()
            defines = [f for f in flags if f.startswith("-DNIMBUS_ENGINE")]
            if env in E2_ENVS:
                self.assertEqual(defines, ["-DNIMBUS_ENGINE=2"], env)
            else:
                self.assertEqual(self.ini.engine(env), 1, env)
                self.assertEqual(defines, [], env)

    def test_an_engine_and_its_define_always_agree(self):
        # Counter-case of the class rule above: an e2 env without the define compiles the engine-1
        # guards' first branch and links nothing it needs; the filter number alone is not enough.
        for env in self.ini.device_envs():
            flags = (self.ini.get("env:" + env, "build_flags") or "").split()
            self.assertEqual(self.ini.engine(env) == 2, "-DNIMBUS_ENGINE=2" in flags, env)


class EngineOneListTests(unittest.TestCase):
    def test_seam2_engine1_only_sources_are_listed(self):
        listed = lister.read_engine1_only(REPO)
        decisions, problems = lister.read_decisions(REPO)
        self.assertEqual(problems, [])
        for rel, word in decisions.items():
            if word == "engine1_only" and rel.endswith(".cpp"):
                self.assertIn(rel, listed)

    def test_the_sources_behind_the_old_engine_headers_are_listed(self):
        listed = lister.read_engine1_only(REPO)
        for header in lister.engine1_headers():
            lib = header.split("/include/")[0]
            src = os.path.join(lib, "src", os.path.basename(header)[:-2] + ".cpp")
            stem = os.path.basename(header)[:-2]
            lib_src = os.path.join(lib, "src", ("orch_" + stem) if lib.endswith("core") else stem) + ".cpp"
            for rel in {src, lib_src}:
                if os.path.isfile(os.path.join(REPO, rel)):
                    self.assertIn(rel, listed, header)


class StaticDramTests(unittest.TestCase):
    def test_every_engine2_env_has_a_bounded_ceiling_row(self):
        with open(os.path.join(HERE, "static_dram_ceilings.json"), encoding="utf-8") as fh:
            data = json.load(fh)
        rows = data["engine2"]
        envs = {r["env"]: r for r in rows.values()}
        self.assertEqual(set(envs), set(E2_ENVS))
        for row in rows.values():
            self.assertLessEqual(row["baseline"], row["ceiling"])
            self.assertLessEqual(row["ceiling"], row["baseline"] + 2048)  # the shipped rows' bound
        # Engine-2 rows never pose as shipped variants (release.yml's loop reads "variants").
        self.assertFalse(set(rows) & set(data["variants"]))
        self.assertEqual(set(check_static_dram.load_all_ceilings()), set(rows) | set(data["variants"]))

    def test_a_name_in_both_tables_fails(self):
        d = tempfile.mkdtemp()
        self.addCleanup(shutil.rmtree, d)
        path = os.path.join(d, "c.json")
        row = {"env": "x", "baseline": 1, "ceiling": 2}
        with open(path, "w", encoding="utf-8") as fh:
            json.dump({"variants": {"a": row}, "engine2": {"a": row}}, fh)
        with self.assertRaises(ValueError):
            check_static_dram.load_all_ceilings(path)
        self.assertEqual(check_static_dram.main(["--ceilings", path]), 1)


if __name__ == "__main__":
    unittest.main()
