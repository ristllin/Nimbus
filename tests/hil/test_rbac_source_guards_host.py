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
SOURCE_DIRS = ("lib", "src", "include", "nimbusd", "tools", "test", ".github")
CODE_SUFFIXES = {".cpp", ".c", ".cc", ".h", ".hpp"}
BUILD_SUFFIXES = {".ini", ".mk", ".py", ".sh", ".cmake", ".yml", ".yaml", ".json", ".toml"}
BUILD_NAMES = {"Makefile", "CMakeLists.txt", "Dockerfile", "platformio.ini"}
SKIPPED_DIRS = {"node_modules", "build", ".pio", "__pycache__"}


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
    """Who switches the test seam on. In code that takes a #define (an #ifdef is the seam
    itself). In a build file, the root's platformio.ini included, any mention at all: a
    flag can be spelled -D, CPPDEFINES, add_definitions or an environment variable."""
    found = set()
    paths = [p for top in SOURCE_DIRS for p in (root / top).rglob("*")] + list(root.glob("*"))
    for path in paths:
        if not path.is_file() or SKIPPED_DIRS & set(path.relative_to(root).parts):
            continue
        is_build = path.name in BUILD_NAMES or path.suffix in BUILD_SUFFIXES
        if not is_build and path.suffix not in CODE_SUFFIXES:
            continue
        text = path.read_text(errors="replace")
        if SEAM in text and (is_build or re.search(rf"^\s*#\s*define\s+{SEAM}\b", text, re.M)):
            found.add(path.relative_to(root).as_posix())
    return found


def test_the_unscoped_results_api_stays_with_the_job_tests():
    assert seam_definers(ROOT) == SEAM_DEFINERS


def test_the_seam_guard_sees_a_new_definer(tmp_path):
    (tmp_path / "src").mkdir()
    (tmp_path / "src" / "agent.cpp").write_text(f"#define {SEAM}\n")
    assert seam_definers(tmp_path) == {"src/agent.cpp"}


# Every route a build flag can take: the root's platformio.ini, a Makefile, a PlatformIO
# extra script, a workflow.
@pytest.mark.parametrize(
    "path,text",
    [
        ("platformio.ini", f"build_flags = -D{SEAM}\n"),
        ("platformio.ini", f"build_flags =\n  -D {SEAM}=1\n"),
        ("nimbusd/Makefile", f"CXXFLAGS += -D{SEAM}\n"),
        ("tools/harness-lab/Makefile", f"CXXFLAGS += -D{SEAM}\n"),
        ("tools/flags.mk", f"X = -D{SEAM}\n"),
        ("tools/pio_extra.py", f'env.Append(CPPDEFINES=["{SEAM}"])\n'),
        ("nimbusd/CMakeLists.txt", f"add_definitions(-D{SEAM})\n"),
        ("nimbusd/Dockerfile", f"ENV CXXFLAGS=-D{SEAM}\n"),
        (".github/workflows/checks.yml", f"env:\n  PLATFORMIO_BUILD_FLAGS: -D{SEAM}\n"),
    ],
)
def test_the_seam_guard_sees_the_build_flag_route(tmp_path, path, text):
    target = tmp_path / path
    target.parent.mkdir(parents=True, exist_ok=True)
    target.write_text(text)
    assert seam_definers(tmp_path) == {path}


def test_the_seam_guard_leaves_a_plain_mention_in_code_alone(tmp_path):
    (tmp_path / "lib").mkdir()
    (tmp_path / "lib" / "jobs.h").write_text(f"#ifdef {SEAM}\nvoid peek();\n#endif  // {SEAM}\n")
    assert seam_definers(tmp_path) == set()
