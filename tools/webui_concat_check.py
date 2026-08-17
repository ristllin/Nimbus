#!/usr/bin/env python3
"""webui_concat_check - assert the web UI fragment set is well-formed.

The config page is served as the ordered concatenation of the PROGMEM
fragments in include/web/ui_*.h (see include/web_pages.h). This check:

  1. extracts each fragment's raw-string payload,
  2. re-concatenates them in the manifest's order,
  3. asserts basic page invariants that a bad split/edit would break
     (single <script> pair, every pane div present, tab bar present,
     balanced R"=====( ... )====="),
  4. optionally byte-compares against a blessed snapshot
     (tools/webui_page.snapshot - refresh with --bless after an
     INTENTIONAL page change).

Run from the repo root:  python3 tools/webui_concat_check.py [--bless]
Exit 0 = OK.
"""

from __future__ import annotations

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
MANIFEST = ROOT / "include" / "web_pages.h"
SNAPSHOT = ROOT / "tools" / "webui_page.snapshot"

RAWSTR = re.compile(r'R"=====\((.*)\)====="', re.S)


def fragment_payload(path: Path) -> str:
    m = RAWSTR.search(path.read_text())
    if not m:
        sys.exit(f"FAIL: no raw-string payload in {path}")
    return m.group(1)


def main() -> None:
    manifest = MANIFEST.read_text()
    order = re.findall(r'#include "web/(ui_\w+)\.h"', manifest)
    if not order:
        sys.exit("FAIL: no fragment includes in web_pages.h")
    listed = re.search(r"CONFIG_HTML_PARTS\[\]\s*=\s*\{([^}]*)\}", manifest)
    if not listed:
        sys.exit("FAIL: CONFIG_HTML_PARTS list missing from web_pages.h")
    idents = [t.strip() for t in listed.group(1).split(",") if t.strip()]
    if [i.lower() for i in idents] != order:
        sys.exit(f"FAIL: include order {order} != parts order {idents}")

    page = "".join(fragment_payload(ROOT / "include" / "web" / f"{n}.h") for n in order)

    # Structural invariants a bad split or edit would break. (Phase 3 C1: the shell
    # became a <nav class=tabs> sidebar and Connectivity folded into pane-set, so the
    # old '<div class=tabs>' + pane-wifi checks were retired for the 8-area IA.)
    panes = ["dash", "fleet", "chat", "mem", "harness", "usage", "gov", "set"]
    checks = {
        "<!doctype html>": page.lower().startswith("<!doctype html>"),
        "one <script>": page.count("<script>") == 1,
        "one </script>": page.count("</script>") == 1,
        "nav bar": 'class=tabs>' in page,
        "8 panes": all(("id=pane-" + p) in page for p in panes),
        "ring sim": "id=ringsim" in page,
    }
    bad = [k for k, ok in checks.items() if not ok]
    if bad:
        sys.exit(f"FAIL: page invariants broken: {bad}")

    if "--bless" in sys.argv:
        SNAPSHOT.write_text(page)
        print(f"blessed snapshot ({len(page)} bytes)")
        return
    if SNAPSHOT.exists():
        if SNAPSHOT.read_text() != page:
            sys.exit("FAIL: page differs from blessed snapshot (intentional? re-bless with --bless)")
        print(f"OK: {len(order)} fragments, {len(page)} bytes, snapshot match")
    else:
        print(f"OK: {len(order)} fragments, {len(page)} bytes (no snapshot)")


if __name__ == "__main__":
    main()
