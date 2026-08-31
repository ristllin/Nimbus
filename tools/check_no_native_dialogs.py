#!/usr/bin/env python3
"""check_no_native_dialogs - the web UI must never use native browser popups.

Native confirm() / alert() / prompt() block the page and look nothing like the
app; the whole web UI uses the styled in-app modal (uiConfirm / uiPrompt /
uiAlert in include/web/ui_js.h) instead. This gate assembles the web UI fragments
(include/web/ui_*.h) exactly as the device serves them and fails on ANY native
dialog call, so a regression can never slip back in (CUM-266).

The allowlist is intentionally EMPTY: there is no legitimate native dialog on
this surface. If you are adding a confirm/prompt/alert, use the shared modal.

Run from the repo root:  python3 tools/check_no_native_dialogs.py
Exit 0 = clean.
"""

from __future__ import annotations

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
WEB = ROOT / "include" / "web"

RAWSTR = re.compile(r'R"=====\((.*)\)====="', re.S)

# A native dialog call: confirm/alert/prompt as a bare call, not a method
# (.confirm) and not a longer identifier (uiConfirm). Matches "confirm(" and
# "confirm (" so a comment restating the banned pattern is caught too - keep the
# word away from an open paren in prose.
NATIVE_DIALOG = re.compile(r"(?<![A-Za-z0-9_.$])(confirm|alert|prompt)\s*\(")

# Empty by design - there is no allowed native dialog on the web surface.
ALLOWLIST: set[str] = set()


def fragment_payload(path: Path) -> str:
    m = RAWSTR.search(path.read_text())
    return m.group(1) if m else ""


def find_native_dialogs(text: str) -> list[tuple[int, str]]:
    """Return (1-based line number, matched call) for every native dialog call."""
    hits: list[tuple[int, str]] = []
    for i, line in enumerate(text.splitlines(), start=1):
        for m in NATIVE_DIALOG.finditer(line):
            hits.append((i, m.group(0)))
    return hits


def main() -> None:
    fragments = sorted(WEB.glob("ui_*.h"))
    if not fragments:
        sys.exit("FAIL: no include/web/ui_*.h fragments found")

    total = 0
    for frag in fragments:
        payload = fragment_payload(frag)
        for lineno, call in find_native_dialogs(payload):
            key = f"{frag.name}:{lineno}"
            if key in ALLOWLIST:
                continue
            total += 1
            print(
                f"FAIL: native dialog {call!r} at {frag.name}:{lineno} "
                f"(use the in-app modal: uiConfirm / uiPrompt / uiAlert)"
            )
    if total:
        sys.exit(f"FAIL: {total} native browser dialog call(s) in the web UI (CUM-266). Replace with the shared modal.")
    print(f"OK: {len(fragments)} web fragments, no native dialog calls")


if __name__ == "__main__":
    main()
