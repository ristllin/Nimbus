"""Secret values that never print (the HIL harness).

A traceback, an assertion message or ``--showlocals`` prints the ``repr`` of every
argument it shows, so a fixture that returns a raw token puts that token in the
test log (the bench run on 19 Oct 2026 printed the test bot token that way).

``Secret`` is a ``str`` whose ``repr`` shows at most the last 4 characters, and
only for values of 12 characters or more. Everything else behaves exactly like the
plain string: ``str()``, f-strings, ``.encode()``, comparisons, form and header
values, so HTTP, serial and URL code needs no change.

``str()`` and f-strings still give the real value (they must: a URL or a form needs
it), so a string built that way, a library error that quotes such a URL, or pytest's
character diff for a failed ``assert secret == other`` would carry the value past
``repr``. The second layer covers those: every ``Secret`` remembers its value
(``remember``), and the HIL conftest runs each test report, and live output when
pytest does not capture it (``-s``), through ``scrub`` before it is printed.
"""

from __future__ import annotations

from functools import lru_cache
from typing import FrozenSet, Iterable, Set

SHOW_TAIL = 4  # the most characters a redacted repr ever shows
SHOW_TAIL_MIN_LEN = 12  # shorter values show no characters at all
SCRUB_MIN_LEN = 6  # shorter values are not scrubbed from text (too likely to hit other words)
# A run of FRAGMENT characters copied from a secret of FRAGMENT_MIN_LEN or more is
# masked even when the value is cut: pytest shortens a long string to
# "prefix...suffix" in an assertion summary. Shorter values (a chat id) only match
# whole, so a stray number in a log is never masked by accident.
FRAGMENT = 6
FRAGMENT_MIN_LEN = 12

_known: Set[str] = set()  # every secret value long enough to scrub, for scrub()


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


def remember(value: object) -> None:
    """Add a value to the set scrub() replaces (values under SCRUB_MIN_LEN are
    ignored)."""
    text = str(value)
    if len(text) >= SCRUB_MIN_LEN:
        _known.add(text)


@lru_cache(maxsize=8)
def _fragments(values: FrozenSet[str]) -> FrozenSet[str]:
    """Every FRAGMENT-long run of the values long enough to be cut mid-string. A
    URL is left out: its runs ("https:", "/api/v") would mask every URL in a
    report."""
    out: Set[str] = set()
    for v in values:
        if len(v) >= FRAGMENT_MIN_LEN and "://" not in v:
            out.update(v[i : i + FRAGMENT] for i in range(len(v) - FRAGMENT + 1))
    return frozenset(out)


def _mask_fragments(text: str, fragments: FrozenSet[str]) -> str:
    """Replace each stretch of ``text`` made of overlapping secret runs with
    ``<secret>``."""
    if not fragments:
        return text
    out, i = [], 0
    while i < len(text):
        if text[i : i + FRAGMENT] not in fragments:
            out.append(text[i])
            i += 1
            continue
        j = i + FRAGMENT
        while j < len(text) and text[j - FRAGMENT + 1 : j + 1] in fragments:
            j += 1
        out.append("<secret>")
        i = j
    return "".join(out)


def scrub(text: str, extra: Iterable[str] = (), fragments: bool = False) -> str:
    """``text`` with every remembered secret value, and every value in ``extra``,
    replaced by its redacted form (longest first, so a value containing another is
    replaced whole). With ``fragments``, also mask any leftover cut piece of a long
    value (FRAGMENT characters or more): pytest's own reprs cut strings, so a
    traceback needs it; printed output carries values whole."""
    values = set(_known)
    values.update(v for v in map(str, extra) if len(v) >= SCRUB_MIN_LEN)
    for value in sorted(values, key=len, reverse=True):
        if value in text:
            text = text.replace(value, redact(value))
    return _mask_fragments(text, _fragments(frozenset(values))) if fragments else text


class Secret(str):
    """A string whose ``repr`` is redacted. Concatenation keeps the wrapper, so a
    value built from a secret (``password + "_WRONG"``) stays redacted too. Every
    value is remembered for scrub()."""

    __slots__ = ()

    def __new__(cls, value: object = "") -> "Secret":
        obj = super().__new__(cls, value)
        remember(obj)
        return obj

    def __repr__(self) -> str:
        return redact(self)

    def __add__(self, other: str) -> "Secret":
        return Secret(str.__add__(self, other))

    def __radd__(self, other: str) -> "Secret":
        return Secret(str.__add__(str(other), self))
