"""Secret values that never print (the HIL harness).

A traceback, an assertion message or ``--showlocals`` prints the ``repr`` of every
argument it shows, so a fixture that returns a raw token puts that token in the
test log (the bench run on 19 Oct 2026 printed the test bot token that way).

``Secret`` is a ``str`` whose ``repr`` shows at most the last 4 characters, and
only for values of 12 characters or more. Everything else behaves exactly like the
plain string: ``str()``, f-strings, ``.encode()``, comparisons, form and header
values, so HTTP, serial and URL code needs no change.

Known limits, both by design (the value must stay usable): ``str()`` and
f-strings give the real value, so never put a secret in an assertion message or a
skip reason; and pytest's text diff for a failed ``assert secret == other`` compares
the raw characters, so assert on a secret's presence or length, never its value.
"""

from __future__ import annotations

SHOW_TAIL = 4  # the most characters a redacted repr ever shows
SHOW_TAIL_MIN_LEN = 12  # shorter values show no characters at all


def redact(value: object) -> str:
    """The printable stand-in for a secret value: ``<secret empty>``,
    ``<secret>`` (short values) or ``<secret ...wxyz>`` (the last 4 only)."""
    if value is None:
        return "<secret none>"
    text = str(value)
    if not text:
        return "<secret empty>"
    if len(text) < SHOW_TAIL_MIN_LEN:
        return "<secret>"
    return f"<secret ...{text[-SHOW_TAIL:]}>"


class Secret(str):
    """A string whose ``repr`` is redacted. Concatenation keeps the wrapper, so a
    value built from a secret (``password + "_WRONG"``) stays redacted too."""

    __slots__ = ()

    def __repr__(self) -> str:
        return redact(self)

    def __add__(self, other: str) -> "Secret":
        return Secret(str.__add__(self, other))

    def __radd__(self, other: str) -> "Secret":
        return Secret(str.__add__(str(other), self))
