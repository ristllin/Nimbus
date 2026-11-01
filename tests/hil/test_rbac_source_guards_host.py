"""Source guards for the role rails (no board).

Two rules a new line of code could break by omission, with nothing failing at the time:

- Every connector attach builder asks attachBuiltinsOnly first, so a member's request can
  never carry the owner's connectors because someone wrote a fourth builder.
- The unscoped results API (every chat's sub-agent results at once) stays declared only
  for jobs.cpp and the job tests, so no product caller can bring back the cross-chat leak.
"""

from __future__ import annotations

import pathlib
import re

import pytest

pytestmark = pytest.mark.host

ROOT = pathlib.Path(__file__).resolve().parents[2]
WIRE = ROOT / "lib/core/src/orch_connectors_wire.cpp"
SEAM = "NIMBUS_JOBS_TEST_SEAM"
SEAM_DEFINERS = {"lib/harness/src/jobs.cpp", "test/test_harness_jobs/main.cpp"}
SOURCE_DIRS = ("lib", "src", "include", "nimbusd", "tools", "test")


def attach_builders(source: str) -> dict:
    """Each attach builder's name and whether it asks the grant before anything else."""
    found = {}
    for m in re.finditer(r"^void (attach\w+Wire)\(.*?\{\n(.*?)\n", source, re.M | re.S):
        found[m.group(1)] = "attachBuiltinsOnly(" in m.group(2)
    return found


def test_every_attach_builder_asks_the_grant_first():
    builders = attach_builders(WIRE.read_text())
    assert set(builders) >= {"attachOpenAIWire", "attachMistralWire", "attachAnthropicWire"}
    assert all(builders.values()), f"an attach builder skips the grant: {builders}"


def test_the_guard_sees_a_builder_that_skips_the_grant():
    source = (
        "void attachGoodWire(JsonDocument& d, bool builtinsOnly) {\n"
        "  builtinsOnly = attachBuiltinsOnly(builtinsOnly);\n"
        "}\n"
        "void attachNewWire(JsonDocument& d,\n"
        "                   bool builtinsOnly) {\n"
        "  for (auto& c : cs) d.add(c);\n"
        "}\n"
    )
    assert attach_builders(source) == {"attachGoodWire": True, "attachNewWire": False}


def seam_definers(root: pathlib.Path) -> set:
    found = set()
    for top in SOURCE_DIRS:
        for path in (root / top).rglob("*"):
            if path.suffix not in {".cpp", ".h", ".hpp", ".ini"} or not path.is_file():
                continue
            if re.search(rf"^\s*#\s*define\s+{SEAM}\b|-D\s*{SEAM}\b", path.read_text(errors="replace"), re.M):
                found.add(path.relative_to(root).as_posix())
    return found


def test_the_unscoped_results_api_stays_with_the_job_tests():
    assert seam_definers(ROOT) == SEAM_DEFINERS


def test_the_seam_guard_sees_a_new_definer(tmp_path):
    (tmp_path / "src").mkdir()
    (tmp_path / "src" / "agent.cpp").write_text(f"#define {SEAM}\n")
    assert seam_definers(tmp_path) == {"src/agent.cpp"}
