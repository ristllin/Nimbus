"""Static internal RAM host guard: no firmware source links the C++ stream library.

``<sstream>``, ``<iostream>`` and friends pull in the libstdc++ locale: about 3.5 KB of
static internal RAM (ctype, numpunct, timepunct and moneypunct facets and their
caches) on a board whose idle internal heap is a few tens of KB. One
std::istringstream in the PANIC arm parser (lib/core) put all of it into every test
image in v4.5.14, so every memory-pressure leg on the bench ran about 3.5 KB short
of what the shipped image has. No board needed.
"""

from __future__ import annotations

import re
from pathlib import Path

import pytest

pytestmark = pytest.mark.host

ROOT = Path(__file__).resolve().parents[2]
STREAM_HEADERS = re.compile(r"#\s*include\s*<(sstream|iostream|istream|ostream|fstream|iomanip|locale|regex)>")


def _offenders():
    for top in ("src", "lib"):
        for path in sorted((ROOT / top).rglob("*")):
            if path.suffix in (".cpp", ".h", ".hpp", ".c") and STREAM_HEADERS.search(path.read_text(errors="replace")):
                yield str(path.relative_to(ROOT))


def test_no_firmware_source_includes_a_stream_header():
    bad = list(_offenders())
    assert not bad, f"these firmware sources link the C++ stream library and its locale (~3.5 KB internal RAM): {bad}"


def test_the_guard_sees_the_shapes_it_bans():
    for line in ("#include <sstream>", "#  include <iostream>", "#include<regex>"):
        assert STREAM_HEADERS.search(line), line
    assert not STREAM_HEADERS.search("#include <string>")
