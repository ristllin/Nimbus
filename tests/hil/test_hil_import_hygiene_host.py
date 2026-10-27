"""Host tier: every HIL module imports with pytest alone. No board.

CI's ``hil-collect`` and ``gate-logic`` jobs install only pytest, so a module-level
``import requests`` (or any other bench-only package) is a collection error there,
while a bench machine that has the package collects cleanly. Five modules shipped
that way and kept the ``checks`` workflow red from 22 Oct 2026 with every local run
green. This check fails locally on the same class: a module-level import must be the
standard library, pytest, or a module of this suite. A bench-only package is imported
inside the function that uses it, under ``try/except ImportError`` (net.py), or under
``if TYPE_CHECKING:``.
"""

from __future__ import annotations

import ast
import sys
from pathlib import Path

import pytest

pytestmark = pytest.mark.host

HIL_DIR = Path(__file__).resolve().parent
ALLOWED_THIRD_PARTY = frozenset({"pytest", "_pytest"})


def local_modules(root: Path) -> set[str]:
    """Module and package names this suite provides (it runs with rootdir on sys.path)."""
    return {p.stem for p in root.rglob("*.py")} | {p.name for p in root.rglob("*") if p.is_dir()}


def unguarded_imports(source: str, local: set[str]) -> list[tuple[int, str]]:
    """(line, top-level name) of every module-level import that needs a package
    beyond the standard library, pytest and this suite. Imports nested in a try,
    an if or a function body are guarded or lazy and are not reported."""
    found = []
    for node in ast.parse(source).body:
        if isinstance(node, ast.Import):
            names = [a.name for a in node.names]
        elif isinstance(node, ast.ImportFrom) and node.level == 0 and node.module:
            names = [node.module]
        else:
            continue
        for name in names:
            top = name.split(".")[0]
            if top in sys.stdlib_module_names or top in ALLOWED_THIRD_PARTY or top in local or top == "__future__":
                continue
            found.append((node.lineno, top))
    return found


def test_every_hil_module_imports_with_pytest_alone():
    local = local_modules(HIL_DIR)
    offenders = [
        f"{path.relative_to(HIL_DIR)}:{line} imports {name}"
        for path in sorted(HIL_DIR.rglob("*.py"))
        for line, name in unguarded_imports(path.read_text(encoding="utf-8"), local)
    ]
    assert not offenders, "a module-level bench-only import breaks CI collection:\n" + "\n".join(offenders)


def test_the_check_flags_a_bare_import_and_passes_the_guarded_forms():
    local = {"net", "kit"}
    bare = "import os\nimport requests\nfrom serial.tools import list_ports\nimport net\n"
    assert unguarded_imports(bare, local) == [(2, "requests"), (3, "serial")]
    guarded = (
        "from typing import TYPE_CHECKING\n"
        "try:\n    import requests\nexcept ImportError:\n    requests = None\n"
        "if TYPE_CHECKING:\n    import serial\n"
        "def f():\n    import requests\n"
        "from . import kit\nimport pytest\n"
    )
    assert unguarded_imports(guarded, local) == []
