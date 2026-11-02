"""Host tier: the Freenove OTA twin runs every test of test_ota.py - no test can be added there and silently missed.

test_ota_freenove.py imports the Solide suite's tests instead of copying them (a schema 2 manifest cannot cross
board families, so the Freenove needs its own run of the same legs). This file parses both sources and fails if a
test exists in test_ota.py but is not in the twin's import list, or if the twin imports a name test_ota.py does
not define, so the twin's coverage cannot drift while its docstring claims a guard.
"""

from __future__ import annotations

import ast
from pathlib import Path

import pytest

pytestmark = pytest.mark.host

HIL = Path(__file__).resolve().parent


def _top_level_defs(path: Path):
    tree = ast.parse(path.read_text(encoding="utf-8"))
    return {node.name for node in tree.body if isinstance(node, (ast.FunctionDef, ast.AsyncFunctionDef))}


def _imported_from_test_ota(path: Path):
    tree = ast.parse(path.read_text(encoding="utf-8"))
    names = set()
    for node in ast.walk(tree):
        if isinstance(node, ast.ImportFrom) and node.module == "test_ota":
            names.update(alias.name for alias in node.names)
    return names


def test_the_freenove_twin_imports_every_test_of_test_ota():
    tests = {name for name in _top_level_defs(HIL / "test_ota.py") if name.startswith("test_")}
    imported = _imported_from_test_ota(HIL / "test_ota_freenove.py")
    missing = tests - imported
    assert not missing, f"test_ota.py tests missing from test_ota_freenove.py's import list: {sorted(missing)}"


def test_the_freenove_twin_imports_nothing_test_ota_does_not_define():
    defined = _top_level_defs(HIL / "test_ota.py")
    imported = _imported_from_test_ota(HIL / "test_ota_freenove.py")
    assert imported <= defined, f"test_ota_freenove.py imports names test_ota.py does not define: {sorted(imported - defined)}"


def test_the_freenove_twin_overrides_the_board_fixture():
    """The twin's only difference from test_ota.py is its own ota_board fixture (the Freenove image and variant)."""
    assert "ota_board" in _top_level_defs(HIL / "test_ota_freenove.py")
