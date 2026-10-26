"""QW3 - the food log's write paths on the device itself, no model in the loop.

The merge review of QW3 asked for proofs on hardware of the rules the host suites
pin (lib/core file_append.h): each case calls the device's own tools through the
test build's tool route (``/api/test/astool``, as the owner) and reads the file
back (``/api/files/dl``):

1. a CSV saved WITHOUT a final line end (artifact.save writes text verbatim, as a
   model often sends it) keeps its well-formed last row: a keyed files.append
   counts it and gives it its line end, never a #cut: row;
2. a CSV whose last row is provably cut (fewer fields than the header): a plain
   files.append sets it aside as "#cut: ..." before the new rows, and the reply
   says so;
3. a plain files.append onto a well-formed row without a line end just starts on
   a fresh line;
4. a date column named "when": two days' "lunch" rows stay two rows;
5. no date column: the keyed write is refused, nothing written;
6. an old log with a meal logged twice: refused, then resolved in one call with
   replace: "all" (the extra row kept as #superseded:), the day counted once.

Markers: net (HTTP only; nothing paid). Needs --allow-hardware, NIMBUS_TEST_IP,
NIMBUS_TEST_TOKEN, a test build (the astool route) and an SD card.
"""

from __future__ import annotations

import json

import pytest

from webrig import ip_tok_or_skip, make_session, require_orchestrator

pytestmark = [pytest.mark.net]

PROJECT = "qw3-paths"
_S = None
_IP = ""


@pytest.fixture(scope="module", autouse=True)
def rig():
    global _S, _IP
    _IP, tok = ip_tok_or_skip("QW3 paths")
    _S = make_session(tok)
    require_orchestrator(_S, _IP, "QW3 paths")
    yield
    for name in ("saved.csv", "cut.csv", "plain.csv", "when.csv", "nodate.csv", "dups.csv"):
        try:
            _S.post(f"http://{_IP}/api/files/rm", data={"project": PROJECT, "name": name}, timeout=10)
        except Exception:  # noqa: BLE001 - best effort cleanup
            pass


def _tool(name: str, args: dict):
    """(reply text, isError) of one tool call as the owner."""
    body = json.dumps({"jsonrpc": "2.0", "id": 1, "method": "tools/call", "params": {"name": name, "arguments": args}})
    r = _S.post(f"http://{_IP}/api/test/astool", data={"chat": "web", "body": body}, timeout=30)
    assert r.status_code == 200, r.text[:200]
    j = r.json()
    res = j.get("result") or {}
    text = " ".join(c.get("text", "") for c in res.get("content", [])) or json.dumps(j)
    return text, bool(res.get("isError")) or "error" in j


def _save(name: str, text: str) -> None:
    _S.post(f"http://{_IP}/api/files/rm", data={"project": PROJECT, "name": name}, timeout=10)
    reply, err = _tool("artifact.save", {"project": PROJECT, "name": name, "text": text})
    assert not err and "saved" in reply, reply


def _dump(name: str) -> str:
    r = _S.get(f"http://{_IP}/api/files/dl", params={"project": PROJECT, "name": name}, timeout=15)
    assert r.status_code == 200, r.text[:120]
    return r.text


def _append(name: str, text: str, **kw):
    return _tool("files.append", dict({"project": PROJECT, "name": name, "text": text}, **kw))


def test_well_formed_last_row_without_line_end_is_kept_and_counted():
    _save("saved.csv", "date,meal,kcal\n2026-10-25,lunch,450\n2026-10-25,dinner,800")
    reply, err = _append("saved.csv", "date,meal,kcal\n2026-10-25,tea,52", key="meal", sum="kcal")
    print(f"QW3 paths 1: {reply!r}")
    assert not err, reply
    assert _dump("saved.csv") == "date,meal,kcal\n2026-10-25,lunch,450\n2026-10-25,dinner,800\n2026-10-25,tea,52\n"
    assert "kcal on 2026-10-25: 1302 from 3 rows" in reply
    assert "#cut" not in reply


def test_plain_append_sets_a_provably_cut_row_aside():
    _save("cut.csv", "date,meal,kcal\n2026-10-25,lunch,450\n2026-10-25,dinner")
    reply, err = _append("cut.csv", "2026-10-25,tea,52")
    print(f"QW3 paths 2: {reply!r}")
    assert not err, reply
    assert "kept as a #cut: row and not counted" in reply
    assert _dump("cut.csv") == "date,meal,kcal\n2026-10-25,lunch,450\n#cut: 2026-10-25,dinner\n2026-10-25,tea,52\n"


def test_plain_append_on_a_whole_row_without_line_end_starts_a_fresh_line():
    _save("plain.csv", "date,meal,kcal\n2026-10-25,lunch,450")
    reply, err = _append("plain.csv", "2026-10-25,tea,52")
    print(f"QW3 paths 3: {reply!r}")
    assert not err, reply
    assert "no line end" in reply and "#cut" not in reply
    assert _dump("plain.csv") == "date,meal,kcal\n2026-10-25,lunch,450\n2026-10-25,tea,52\n"


def test_a_date_column_named_when_keeps_days_apart():
    _S.post(f"http://{_IP}/api/files/rm", data={"project": PROJECT, "name": "when.csv"}, timeout=10)
    for day, kcal in (("2026-10-24", 500), ("2026-10-25", 450)):
        reply, err = _append("when.csv", f"when,meal,kcal\n{day},lunch,{kcal}", key="meal", sum="kcal")
        print(f"QW3 paths 4: {reply!r}")
        assert not err, reply
    assert _dump("when.csv") == "when,meal,kcal\n2026-10-24,lunch,500\n2026-10-25,lunch,450\n"


def test_no_date_column_is_refused():
    _S.post(f"http://{_IP}/api/files/rm", data={"project": PROJECT, "name": "nodate.csv"}, timeout=10)
    reply, err = _append("nodate.csv", "meal,kcal\nlunch,450", key="meal", sum="kcal")
    print(f"QW3 paths 5: {reply!r}")
    assert err and "no date column" in reply
    r = _S.get(f"http://{_IP}/api/files/dl", params={"project": PROJECT, "name": "nodate.csv"}, timeout=15)
    assert r.status_code == 404, "a refused keyed write created the file"


def test_replace_all_resolves_a_meal_logged_twice():
    _save("dups.csv", "date,meal,kcal\n2026-10-24,dinner,1200\n2026-10-24,lunch,500\n2026-10-24,dinner,1100\n")
    reply, err = _append("dups.csv", "date,meal,kcal\n2026-10-24,dinner,990", key="meal", sum="kcal")
    print(f"QW3 paths 6a: {reply!r}")
    assert err and 'replace: "all"' in reply
    reply, err = _append("dups.csv", "date,meal,kcal\n2026-10-24,dinner,990", key="meal", sum="kcal", replace="all")
    print(f"QW3 paths 6b: {reply!r}")
    assert not err, reply
    assert "kcal on 2026-10-24: 1490 from 2 rows" in reply
    assert _dump("dups.csv") == (
        "date,meal,kcal\n2026-10-24,dinner,990\n2026-10-24,lunch,500\n#superseded: 2026-10-24,dinner,1100\n"
    )
