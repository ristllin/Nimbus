"""LOUD manual-step protocol (the HIL test spec).

The one non-negotiable rule of this harness:

    A manual step that is not affirmatively confirmed by a human is a test
    FAILURE (red). It is NEVER a silent skip and NEVER a pass-by-default.

Contract:
  * ``confirm(prompt)``  - prompt the operator, read y/n with a timeout. Anything
    but an explicit ``y`` (including a timeout or a non-TTY stdin) -> pytest.fail.
  * ``observe(prompt, parse)`` - the human reports a value (e.g. a frequency they
    heard); a blank/unparseable answer -> pytest.fail. Returns the parsed value so
    the TEST'S OWN assertion runs on it.

``--manual-yes`` only short-circuits the *keystroke* (for CI dry-runs of the plumbing).
It NEVER short-circuits the test's downstream assertion, and it is REFUSED when the
active marker selection is exactly ``-m manual`` - a genuine manual run cannot be
rubber-stamped.
"""

from __future__ import annotations

import select
import sys
from typing import Callable, Optional, TypeVar

import pytest

T = TypeVar("T")

_BOX = "=" * 72


def _emit(text: str) -> None:
    """Write straight to the real stderr so the prompt is visible even when
    pytest is capturing stdout. Flush so the operator sees it immediately."""
    sys.stderr.write(text + "\n")
    sys.stderr.flush()


def _read_line_with_timeout(timeout: float) -> Optional[str]:
    """Read one line from stdin within ``timeout`` seconds. Returns None on
    timeout. Returns None immediately if stdin is not an interactive TTY (so a
    CI/non-interactive run can never accidentally 'confirm')."""
    try:
        if not sys.stdin or not sys.stdin.isatty():
            return None
    except (ValueError, OSError):
        return None
    # select() on the stdin fd; POSIX only, which is all this harness targets.
    try:
        rlist, _, _ = select.select([sys.stdin], [], [], timeout)
    except (OSError, ValueError):
        return None
    if not rlist:
        return None
    return sys.stdin.readline()


class ManualStep:
    """Runner injected via the ``require_manual`` fixture. Holds the pytest
    config so it can honor (and police) ``--manual-yes``."""

    def __init__(self, config: "pytest.Config"):
        self._config = config

    # -- policy: is the prompt-keystroke bypass allowed right now? -----------
    def _bypass_allowed(self) -> bool:
        if not self._config.getoption("--manual-yes", default=False):
            return False
        # Refuse the bypass when the operator is explicitly doing a manual run
        # (-m manual). A real manual pass must come from a real keystroke.
        markexpr = str(self._config.getoption("-m", default="") or "")
        if "manual" in markexpr:
            _emit(
                f"{_BOX}\n[manual] --manual-yes is REFUSED under `-m manual`: "
                "a genuine manual run cannot be auto-confirmed.\n{}".format(_BOX)
            )
            return False
        return True

    def _prompt_box(self, prompt: str) -> None:
        _emit("")
        _emit(_BOX)
        _emit(">>> MANUAL STEP")
        _emit(f">>> {prompt}")
        _emit(_BOX)

    # -- confirm y/n ---------------------------------------------------------
    def confirm(self, prompt: str, timeout: float = 60.0) -> None:
        """Block until the operator presses ``y``. Fail LOUD on anything else,
        on timeout, or on a non-interactive stdin. Never returns without a 'y'."""
        self._prompt_box(prompt + "   [y = done / anything else = fail]")

        if self._bypass_allowed():
            _emit("[manual] --manual-yes: auto-confirming the PROMPT only (the test's own assertion still runs).")
            return

        answer = _read_line_with_timeout(timeout)
        if answer is None:
            if not (sys.stdin and getattr(sys.stdin, "isatty", lambda: False)()):
                pytest.fail(
                    "manual step requires an interactive operator; run with "
                    "`-m manual` on a real console (stdin is not a TTY). "
                    f"Step: {prompt}"
                )
            pytest.fail(f"manual step timed out after {timeout:.0f}s (no confirmation): {prompt}")
        if answer.strip().lower() not in ("y", "yes"):
            pytest.fail(f"manual step NOT confirmed (got {answer.strip()!r}): {prompt}")
        # 'y' -> fall through; the caller's own assertion is what actually
        # decides pass/fail.

    # -- observe a reported value -------------------------------------------
    def observe(self, prompt: str, parse: Callable[[str], T], timeout: float = 90.0) -> T:
        """Ask the human to REPORT a value and return the parsed result for the
        test to assert on. Blank/unparseable/timeout/non-TTY -> LOUD fail."""
        self._prompt_box(prompt + "   [type the value, then Enter]")

        if self._bypass_allowed():
            pytest.fail(
                "--manual-yes cannot fabricate an observed value; run this "
                f"step with a real operator (-m manual). Step: {prompt}"
            )

        answer = _read_line_with_timeout(timeout)
        if answer is None:
            pytest.fail(
                "manual observation requires an interactive operator on a real "
                f"console (stdin not a TTY or timed out): {prompt}"
            )
        text = answer.strip()
        if not text:
            pytest.fail(f"manual observation was blank: {prompt}")
        try:
            return parse(text)
        except Exception as exc:  # noqa: BLE001 - surface any parse failure loud
            pytest.fail(f"could not parse observed value {text!r}: {exc} ({prompt})")
