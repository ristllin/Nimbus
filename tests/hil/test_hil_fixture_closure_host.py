"""Host tier: every HIL test's fixtures can be set up. No board.

``pytest --collect-only tests/hil`` proves the files import; it never resolves a
fixture. A board leg whose fixture closure is broken (a module-scoped fixture that
asks for a function-scoped one, or a fixture name that does not exist) collects
cleanly and then errors at setup in seconds, after the image was built and
flashed: the turn-contract bench lost a whole bench run to a ScopeMismatch on
19 Oct 2026, with collection green.

pytest's ``--setup-plan`` resolves the full fixture closure of every selected test
with pytest's own machinery and runs nothing: no fixture body, no test body, no
teardown. So the check below selects every test (``-m ""``, hardware legs
included, with ``--allow-hardware`` so the gate does not skip them before their
fixtures resolve) and fails on any setup error. As a second guard the serial port
points at a path that does not exist and the LAN-only variables are removed, so a
fixture body could not reach a board even if one ran.
"""

from __future__ import annotations

import os
import shutil
import subprocess
import sys
from pathlib import Path

import pytest

pytestmark = pytest.mark.host

HIL_DIR = Path(__file__).resolve().parent


def fixture_plan(hil_dir: Path) -> tuple[int, list[str], str]:
    """Run ``pytest --setup-plan`` over every test in ``hil_dir``.

    Returns (exit code, the setup-error lines, the whole output)."""
    env = {k: v for k, v in os.environ.items() if k not in ("NIMBUS_TEST_IP", "NIMBUS_TEST_TOKEN")}
    env["NIMBUS_PORT"] = str(hil_dir / "no-such-serial-port")
    env["COLUMNS"] = "400"  # keep each summary line whole
    cmd = [
        sys.executable,
        "-m",
        "pytest",
        ".",
        "--setup-plan",
        "--allow-hardware",
        "-m",
        "",
        "-p",
        "no:cacheprovider",
        "-q",
        "--no-header",
        "-rE",
    ]
    r = subprocess.run(cmd, cwd=hil_dir, env=env, capture_output=True, text=True, timeout=300)
    out = r.stdout + r.stderr
    errors = [ln for ln in out.splitlines() if ln.startswith("ERROR ")]
    return r.returncode, errors, out


def _causes(out: str) -> list[str]:
    keep = ("ScopeMismatch", "fixture '", "recursive dependency")
    return sorted({ln.strip() for ln in out.splitlines() if any(k in ln for k in keep)})


def test_every_hil_test_fixture_closure_resolves():
    rc, errors, out = fixture_plan(HIL_DIR)
    assert "ScopeMismatch" not in out, "\n".join(errors + _causes(out))
    assert rc == 0 and not errors, f"pytest --setup-plan rc={rc}:\n" + "\n".join(errors + _causes(out) or [out[-2000:]])


# ---- counter-tests: the check goes red on the class, and only on the class ----

PROBE_HEAD = "import pytest\n\n"
BAD_SCOPE = '''
@pytest.fixture(scope="module")
def board(device, net, secrets, require_secret):
    return "ip"


@pytest.mark.hil
def test_leg(board):
    pass
'''
MISSING_FIXTURE = '''
@pytest.mark.hil
@pytest.mark.manual
def test_leg(device, manual):
    pass
'''
CACHED_FUNCTION_SCOPE = '''
@pytest.fixture(scope="module")
def _cache():
    return {}


@pytest.fixture
def board(device, net, secrets, require_secret, _cache):
    return _cache.setdefault("ip", "ip")


@pytest.mark.hil
@pytest.mark.net
def test_leg(board):
    pass
'''


def _probe_dir(tmp_path: Path, body: str) -> Path:
    """The HIL harness (conftest, helpers, pytest.ini) without its tests, plus one probe."""
    d = tmp_path / "hil"
    shutil.copytree(HIL_DIR, d, ignore=shutil.ignore_patterns("test_*.py", "__pycache__", "*.md", ".pytest_cache"))
    (d / "test_probe.py").write_text(PROBE_HEAD + body)
    return d


def test_the_check_catches_a_module_fixture_asking_for_a_function_fixture(tmp_path):
    rc, errors, out = fixture_plan(_probe_dir(tmp_path, BAD_SCOPE))
    assert rc != 0 and "ScopeMismatch" in out, out[-2000:]
    assert any("test_probe.py::test_leg" in e for e in errors), errors


def test_the_check_catches_a_fixture_that_does_not_exist(tmp_path):
    rc, errors, out = fixture_plan(_probe_dir(tmp_path, MISSING_FIXTURE))
    assert rc != 0 and "fixture 'manual' not found" in out, out[-2000:]
    assert any("test_probe.py::test_leg" in e for e in errors), errors


def test_a_function_scoped_board_with_a_module_cache_passes(tmp_path):
    rc, errors, out = fixture_plan(_probe_dir(tmp_path, CACHED_FUNCTION_SCOPE))
    assert rc == 0 and not errors, out[-2000:]
