"""files.append's paths on the device itself, no model in the loop (RB-REC1, owner ruling 41).

files.append is a pure append on every path: the rows go on the end as sent, nothing stored is
read back, replaced, set aside or refused. Each case calls the device's own tools through the
test build's tool route (``/api/test/astool``, as the owner) and reads the file back
(``/api/files/dl``):

1. a CSV saved WITHOUT a final line end (artifact.save writes text verbatim) keeps its last row
   as it was; the new rows start on a fresh line and the reply says so;
2. a CSV whose last row is cut (fewer fields than the header) is never set aside or rewritten:
   the row stays as it was, the new rows start on a fresh line;
3. a food log, an expenses log and a training log take the same older keyed call alike: one
   error naming its arguments, nothing written; a plain append of the same row lands, and the
   day total of files.read {tail, sum} counts every row;
4. a date column named "when" keeps two days' rows apart in the day total;
5. no date column: the rows are appended, and the day total says there is no date column.

Markers: net (HTTP only; nothing paid). Needs --allow-hardware, NIMBUS_TEST_IP,
NIMBUS_TEST_TOKEN, a test build (the astool route) and an SD card.
"""

from __future__ import annotations

import json

import pytest

from webrig import ip_tok_or_skip, make_session, require_orchestrator

pytestmark = [pytest.mark.net]

PROJECT = "append-paths"
NAMES = ("saved.csv", "cut.csv", "food.csv", "expenses.csv", "training.csv", "when.csv", "nodate.csv")
_S = None
_IP = ""


@pytest.fixture(scope="module", autouse=True)
def rig():
    global _S, _IP
    _IP, tok = ip_tok_or_skip("append paths")
    _S = make_session(tok)
    require_orchestrator(_S, _IP, "append paths")
    yield
    for name in NAMES:
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


def _tail(name: str, col: str, date: str = "") -> str:
    args = {"project": PROJECT, "name": name, "tail": 5, "sum": col}
    if date:
        args["date"] = date
    reply, err = _tool("files.read", args)
    assert not err, reply
    return reply


def test_a_last_row_without_line_end_is_kept_and_the_rows_start_fresh():
    _save("saved.csv", "date,item,amount\n2026-10-25,lunch,450\n2026-10-25,dinner,800")
    reply, err = _append("saved.csv", "2026-10-25,tea,52")
    print(f"append paths 1: {reply!r}")
    assert not err, reply
    assert "no line end" in reply and "#cut" not in reply
    assert _dump("saved.csv") == "date,item,amount\n2026-10-25,lunch,450\n2026-10-25,dinner,800\n2026-10-25,tea,52\n"
    assert "amount on 2026-10-25: 1302 from 3 rows" in _tail("saved.csv", "amount", "2026-10-25")


def test_a_cut_last_row_is_never_set_aside():
    _save("cut.csv", "date,item,amount\n2026-10-25,lunch,450\n2026-10-25,dinner")
    reply, err = _append("cut.csv", "2026-10-25,tea,52")
    print(f"append paths 2: {reply!r}")
    assert not err, reply
    assert "#cut" not in reply
    assert _dump("cut.csv") == "date,item,amount\n2026-10-25,lunch,450\n2026-10-25,dinner\n2026-10-25,tea,52\n"


@pytest.mark.parametrize(
    "name, header, row, col",
    [
        ("food.csv", "date,time,item,qty,kcal", "2026-10-24,12:40,soup,1,300", "kcal"),
        ("expenses.csv", "date,time,item,qty,amount", "2026-10-24,12:40,train,1,300", "amount"),
        ("training.csv", "date,time,item,qty,kg", "2026-10-24,12:40,squat,5,300", "kg"),
    ],
)
def test_three_tables_take_an_older_keyed_call_alike(name, header, row, col):
    _save(name, f"{header}\n{row}\n")
    reply, err = _append(name, row, key="item", sum=col, replace="all")
    print(f"append paths 3 {name}: {reply!r}")
    assert err and "not an argument" in reply, reply
    assert _dump(name) == f"{header}\n{row}\n"
    reply, err = _append(name, row)
    assert not err and "appended 1 row" in reply, reply
    assert _dump(name) == f"{header}\n{row}\n{row}\n"
    assert f"{col} on 2026-10-24: 600 from 2 rows" in _tail(name, col, "2026-10-24")


def test_a_date_column_named_when_keeps_days_apart():
    _save("when.csv", "when,item,amount\n")
    for day, amount in (("2026-10-24", 500), ("2026-10-25", 450)):
        reply, err = _append("when.csv", f"{day},lunch,{amount}")
        print(f"append paths 4: {reply!r}")
        assert not err, reply
    assert _dump("when.csv") == "when,item,amount\n2026-10-24,lunch,500\n2026-10-25,lunch,450\n"
    assert "amount on 2026-10-24: 500 from 1 row" in _tail("when.csv", "amount", "2026-10-24")


def test_no_date_column_still_appends_and_the_total_says_why():
    _S.post(f"http://{_IP}/api/files/rm", data={"project": PROJECT, "name": "nodate.csv"}, timeout=10)
    reply, err = _append("nodate.csv", "item,amount\nlunch,450")
    print(f"append paths 5: {reply!r}")
    assert not err, reply
    assert _dump("nodate.csv") == "item,amount\nlunch,450\n"
    assert "no date column" in _tail("nodate.csv", "amount")
