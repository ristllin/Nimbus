"""The HIL suite's door to tools/bench_guard.py, the one forbidden-device guard.

tools/ is not on the suite's import path (and this directory's secrets.py shadows the standard library, so the two
must not be mixed), so the guard is loaded by file on first use and shared through sys.modules with any tool loaded
the same way. Importing this module touches nothing; a guard that cannot be found raises at the check, so a port is
never opened without one. Standard library only: CI collects this suite with pytest alone.
"""

from __future__ import annotations

import importlib.util
import sys
from pathlib import Path

_PATH = Path(__file__).resolve().parents[2] / "tools" / "bench_guard.py"


def load():
    """The guard module, loaded once."""
    if "bench_guard" in sys.modules:
        return sys.modules["bench_guard"]
    spec = importlib.util.spec_from_file_location("bench_guard", _PATH)
    module = importlib.util.module_from_spec(spec)
    sys.modules["bench_guard"] = module
    try:
        spec.loader.exec_module(module)
    except BaseException:
        del sys.modules["bench_guard"]
        raise
    return module


def __getattr__(name):
    """port_guard.ForbiddenDevice, port_guard.AnonymousPort and the rest come straight from the guard."""
    return getattr(load(), name)


def check_port(port, tool="bench tool"):
    return load().check_port(port, tool)


def check_mac(mac, tool="bench tool"):
    return load().check_mac(mac, tool)


def port_for_mac(mac, tool="port-for-mac"):
    return load().port_for_mac(mac, tool)


def guard_norm(text):
    return load().norm(text)
