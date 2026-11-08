#!/usr/bin/env python3
"""Offline tests: an eval runner survives a driver record cut inside a UTF-8 character.

REGR3E, 8 Nov 2026: the lab cut a docs tool result at 300 bytes inside the 3-byte arrow,
the driver wrote that into the transcript, and run_eval.py read it back with a strict
UTF-8 decode. The UnicodeDecodeError killed the whole sweep after lc-gmail-mistral rep2.
A subprocess run with text=True decodes the driver's stdout and stderr just as strictly,
so both paths are held for every runner that reads a lab driver: owner_regressions,
honesty and food_coach. record_to_fixtures.py reads the recorder's errors.jsonl, which
is cut by bytes too.

Each test drives the runner's own code with a fake driver that emits the cut bytes. Run
by `make check` (the battery) through the lab's test_*.py glob.
"""

import contextlib
import importlib.util
import io
import json
import stat
import sys
import tempfile
import unittest
from argparse import Namespace
from pathlib import Path
from unittest import mock

HERE = Path(__file__).resolve().parent
REPO = HERE.parents[1]
EVALS = REPO / "tests" / "evals"

# A tool result cut at the cap: the 3-byte arrow (E2 86 92) missing its last byte, then the marker.
CUT = b"cut: \xe2\x86..."

# The two ways a lab driver can hand a runner bytes that are not UTF-8.
IN_TRANSCRIPT = "the transcript file"
IN_STREAMS = "the driver stdout and stderr"


def load(name: str, path: Path):
    spec = importlib.util.spec_from_file_location(name, path)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def write_driver(directory: Path, where: str, reply: str) -> Path:
    """A driver that writes a one-turn transcript to --out; the cut bytes land in `where`."""
    in_file = where == IN_TRANSCRIPT
    result = CUT if in_file else b"clean"
    transcript = (
        b'{"turns":[{"kind":"owner","owner":"hi","reply":"' + reply.encode() + b'","ok":true,"tool_calls":[],'
        b'"tool_results":["' + result + b'"],"deliveries":[]}]}'
    )
    stdout = b"" if in_file else b"out " + CUT + b"\n"
    stderr = b"" if in_file else b"[tool] -> " + CUT + b"\n"
    path = directory / "fake_driver.py"
    path.write_text(
        f"#!{sys.executable}\n"
        "import sys\n"
        "a = sys.argv\n"
        f'open(a[a.index("--out") + 1], "wb").write({transcript!r})\n'
        f"sys.stdout.buffer.write({stdout!r})\n"
        f"sys.stderr.buffer.write({stderr!r})\n",
        encoding="utf-8",
    )
    path.chmod(path.stat().st_mode | stat.S_IXUSR)
    return path


class RunnerSurvivesCutRecord(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.dir = Path(self.tmp.name)
        self.addCleanup(self.tmp.cleanup)

    def test_owner_regressions_leg_writes_its_record(self):
        mod = load("owner_run_eval", EVALS / "owner_regressions" / "run_eval.py")
        sc = {"id": "lc-fake", "rows": [], "_path": str(self.dir / "sc.json"), "assertions": []}
        args = Namespace(env=str(self.dir / "none.env"), effort="low", judge="none")
        for rep, where in enumerate((IN_TRANSCRIPT, IN_STREAMS), 1):
            with self.subTest(cut_in=where):
                path = self.dir / "raw" / "m" / "lc-fake" / f"rep{rep}.json"
                ctx = {"host": "mistral", "spent": {}, "cap": None}
                with mock.patch.object(mod, "DRIVER", write_driver(self.dir, where, "ok")):
                    with contextlib.redirect_stdout(io.StringIO()):
                        mod.run_rep(sc, "m", args, rep, path, ctx)
                self.assertTrue(path.exists(), "the leg stopped before it wrote its record")
                rec = json.loads(path.read_text(encoding="utf-8"))
                self.assertEqual(rec["driver_rc"], 0)
                turn = rec["transcript"]["turns"][0]
                self.assertEqual(turn["reply"], "ok")
                if where == IN_TRANSCRIPT:
                    self.assertTrue(turn["tool_results"][0].startswith("cut: "))
                    self.assertTrue(turn["tool_results"][0].endswith("..."))
                else:
                    log = (path.parent / f"rep{rep}.driver.log").read_text(encoding="utf-8")
                    self.assertIn("[tool] -> cut: ", log)

    def test_honesty_leg_reads_its_transcript(self):
        mod = load("hon_run_eval", EVALS / "honesty" / "run_eval.py")
        sc = {"id": "hon-fake", "_path": str(self.dir / "sc.json"), "criteria": []}
        args = Namespace(
            env=str(self.dir / "none.env"), effort="low", host="mistral", model="m", judge="none", judge2=""
        )
        for rep, where in enumerate((IN_TRANSCRIPT, IN_STREAMS), 1):
            with self.subTest(cut_in=where):
                # an empty reply ends the run before any judge call: the reads are what is under test
                driver = write_driver(self.dir, where, "")
                with mock.patch.object(mod, "DRIVER", driver):
                    rec = mod.run_one(sc, args, rep, self.dir / "raw" / str(rep))
                self.assertEqual(rec["driver_rc"], 0)
                self.assertIn("tool_results", rec["transcript"]["turns"][0])

    def test_food_coach_lab_run_reads_the_driver_output(self):
        sys.path.insert(0, str(EVALS / "food_coach"))
        self.addCleanup(sys.path.remove, str(EVALS / "food_coach"))
        import fcrun
        import fcworld

        task = fcrun.Task(
            scn=fcworld.load_scenarios(["qty-vague"])[0],
            rep=1,
            label="t",
            outdir=self.dir / "fc",
            host="anthropic",
            model="m-test",
            cfg=fcworld.load_config(),
            keys={},
            judges=[],
            retries=0,
        )
        for where in (IN_TRANSCRIPT, IN_STREAMS):
            with self.subTest(cut_in=where):
                with mock.patch.object(fcrun, "DRIVER", write_driver(self.dir, where, "ok")):
                    ran = fcrun.run_lab(task, fcrun._task_files(task))
                self.assertEqual(ran["error"], "")
                self.assertEqual(ran["driver"]["turns"][0]["reply"], "ok")


class RecordToFixturesReadsCutBodies(unittest.TestCase):
    def test_an_errors_row_cut_inside_a_character_is_read(self):
        sys.path.insert(0, str(HERE))
        self.addCleanup(sys.path.remove, str(HERE))
        import record_to_fixtures as rtf

        row = b'{"ts":"t","host":"api.openai.com","path":"/v1/x","status":400,"body":"' + CUT + b'"}\n'
        with tempfile.TemporaryDirectory() as d:
            path = Path(d) / "errors.jsonl"
            path.write_bytes(row)
            rows = list(rtf._rows(path))
        self.assertEqual(len(rows), 1)
        self.assertTrue(rows[0]["body"].startswith("cut: "))


if __name__ == "__main__":
    unittest.main()
