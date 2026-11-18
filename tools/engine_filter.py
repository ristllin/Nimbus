"""platformio extra_scripts - keep the other engine's sources out of a device build.

A device env builds one engine. Its number is the env option `custom_nimbus_engine` (1 when
absent). The sources only the other engine uses are listed in tools/engine/:

    engine 1 (every device env today)  drops the files in engine2_only.txt
    engine 2 (the *-e2 envs, RB-I0)    drops the files in engine1_only.txt

The drop is a PlatformIO build middleware, so it applies to every source collection of the
env: src/ and each library (lib/core, lib/harness), archived or not. A dropped file is never
compiled, so its object, and the unwind tables --gc-sections keeps for a loaded object
(.eh_frame), never reach the link. The host builds (pio test -e native, nimbusd, harness-lab)
do not run this script and compile both engines.

Fails closed: an unknown engine number or a missing list stops the build.
tools/release_gate/check_engine_filter.py checks the lists against the tree and the built maps.
"""

from __future__ import annotations

import atexit
import os
from typing import Iterable, List, Set

ENGINE_OPTION = "custom_nimbus_engine"
DEFAULT_ENGINE = 1
LIST_DIR = os.path.join("tools", "engine")
# The list of sources an env of engine N must NOT compile.
DROP_LIST = {1: "engine2_only.txt", 2: "engine1_only.txt"}


class EngineFilterError(Exception):
    pass


def parse_list(text: str) -> List[str]:
    """Repo-relative paths, one per line; '#' starts a comment, blank lines are skipped."""
    out = []
    for raw in text.splitlines():
        line = raw.split("#", 1)[0].strip()
        if line:
            out.append(line)
    return out


def load_list(path: str) -> List[str]:
    with open(path, "r", encoding="utf-8") as fh:
        return parse_list(fh.read())


def engine_number(value: "str | None") -> int:
    if value is None or str(value).strip() == "":
        return DEFAULT_ENGINE
    text = str(value).strip()
    if text not in ("1", "2"):
        raise EngineFilterError(f"{ENGINE_OPTION} = {text!r}: must be 1 or 2")
    return int(text)


def drop_list_path(root: str, engine: int) -> str:
    return os.path.join(root, LIST_DIR, DROP_LIST[engine])


def dropped_paths(root: str, engine: int) -> Set[str]:
    """Absolute, normalized paths of the sources an engine-`engine` device build drops."""
    path = drop_list_path(root, engine)
    if not os.path.isfile(path):
        raise EngineFilterError(f"engine {engine}: {os.path.relpath(path, root)} is missing")
    return {norm(os.path.join(root, rel)) for rel in load_list(path)}


def norm(path: str) -> str:
    return os.path.normcase(os.path.realpath(path))


def make_middleware(drop: Iterable[str]):
    """A build middleware: returns None (drop) for a listed source, the node otherwise.

    The sources it actually dropped collect in `engine_middleware.dropped`."""
    drop = frozenset(drop)

    def engine_middleware(node):
        path = norm(node.srcnode().get_abspath())
        if path in drop:
            engine_middleware.dropped.add(path)
            return None
        return node

    engine_middleware.dropped = set()
    return engine_middleware


def install(env) -> int:
    """Register the drop for this env; returns the number of listed sources."""
    root = env["PROJECT_DIR"]
    engine = engine_number(env.GetProjectOption(ENGINE_OPTION, ""))
    drop = dropped_paths(root, engine)
    middleware = make_middleware(drop)
    env.AddBuildMiddleware(middleware)
    name = env["PIOENV"]
    print(f"engine_filter: {name} builds engine {engine}, drops the {len(drop)} sources of the other engine")
    # The count of files the build actually collected and dropped (a library the env never
    # builds drops none), printed once the SConscripts have run.
    atexit.register(
        lambda: print(f"engine_filter: {name} dropped {len(middleware.dropped)} of {len(drop)} listed sources")
    )
    return len(drop)


if "Import" in globals():  # run by PlatformIO as an extra script
    Import("env")  # noqa: F821  (platformio construction environment)
    try:
        install(env)  # noqa: F821
    except EngineFilterError as exc:
        print(f"engine_filter: FAIL: {exc}")
        env.Exit(1)  # noqa: F821
