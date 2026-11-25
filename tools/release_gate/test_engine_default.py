"""The default engine is one switch in platformio.ini (lane E2-P-REL, D2).

[engine_default] extends engine1 or engine2. The plain product env names (the stable rows of
tools/release/variants.json, and the test console builds) follow it; the *-e1 and *-e2 envs pin
their engine and never follow it. Each position is checked on a copy of the real file, through
the gate's resolver (check_engine_filter.PioIni) and, where PlatformIO is importable, through
PlatformIO's own ProjectConfig. The committed position is engine 1: the flip is RB-FLIP's.
"""

from __future__ import annotations

import os
import re
import shutil
import sys
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(REPO, "tools", "release"))

import check_engine_filter  # noqa: E402
import publish_candidate  # noqa: E402

PLAIN = ("esp32s3", "esp32s3-cyd", "esp32s3-cyd-35", "esp32s3-cyd-40", "test", "test-cyd")
DEFINE = "-DNIMBUS_ENGINE=2"
SWITCH = re.compile(r"^\[engine_default\]\nextends = (engine[12])\n", re.M)


def flipped(text: str, engine: str) -> str:
    out, n = SWITCH.subn(f"[engine_default]\nextends = {engine}\n", text)
    assert n == 1, "platformio.ini has no single [engine_default] switch"
    return out


class EngineDefaultTests(unittest.TestCase):
    def setUp(self):
        with open(os.path.join(REPO, "platformio.ini"), encoding="utf-8") as fh:
            self.text = fh.read()
        self.tmp = tempfile.mkdtemp()

    def tearDown(self):
        shutil.rmtree(self.tmp)

    def ini(self, engine: str) -> str:
        path = os.path.join(self.tmp, engine + ".ini")
        with open(path, "w", encoding="utf-8") as fh:
            fh.write(flipped(self.text, engine))
        return path

    def resolved(self, path: str, env: str):
        """(engine, the engine defines in build_flags) by the gate's resolver."""
        ini = check_engine_filter.PioIni(path)
        flags = (ini.get("env:" + env, "build_flags") or "").split()
        return ini.engine(env), [f for f in flags if f.startswith("-DNIMBUS_ENGINE")]

    def test_the_committed_switch_is_engine_1(self):
        self.assertEqual(SWITCH.search(self.text).group(1), "engine1")
        for env in PLAIN:
            self.assertEqual(self.resolved(os.path.join(REPO, "platformio.ini"), env), (1, []), env)

    def test_each_position_moves_the_plain_names_and_never_the_pinned_ones(self):
        for want, engine in ((1, "engine1"), (2, "engine2")):
            path = self.ini(engine)
            for env in PLAIN:
                self.assertEqual(self.resolved(path, env), (want, [DEFINE] if want == 2 else []), (engine, env))
                self.assertEqual(self.resolved(path, env + "-e1"), (1, []), (engine, env))
                self.assertEqual(self.resolved(path, env + "-e2"), (2, [DEFINE]), (engine, env))

    def test_a_plain_name_and_its_pinned_twins_differ_only_in_the_engine(self):
        # publish_candidate.build_options: every resolved option but the engine; so a stable row
        # built from the plain name is the same board and build in both positions.
        for engine in ("engine1", "engine2"):
            ini = check_engine_filter.PioIni(self.ini(engine))
            for env in PLAIN:
                base = publish_candidate.build_options(ini, env)
                for twin in (env + "-e1", env + "-e2"):
                    self.assertEqual(publish_candidate.build_options(ini, twin), base, (engine, twin))

    def test_platformio_resolves_both_positions_the_same_way(self):
        try:
            from platformio.project.config import ProjectConfig
        except ImportError:
            self.skipTest("PlatformIO is not importable here (CI's pytest-only venv)")
        for want, engine in ((1, "engine1"), (2, "engine2")):
            pio = ProjectConfig(self.ini(engine))
            for env in PLAIN:
                self.assertEqual(int(pio.get("env:" + env, "custom_nimbus_engine")), want, (engine, env))
                flags = " ".join(pio.get("env:" + env, "build_flags")).split()
                self.assertEqual(flags.count(DEFINE), 1 if want == 2 else 0, (engine, env))
                e1 = " ".join(pio.get("env:" + env + "-e1", "build_flags")).split()
                self.assertEqual((int(pio.get("env:" + env + "-e1", "custom_nimbus_engine")), DEFINE in e1), (1, False))


if __name__ == "__main__":
    unittest.main()
