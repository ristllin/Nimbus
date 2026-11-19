#!/usr/bin/env python3
"""Hard assertions for the owner-regression live suite.

Every scenario lists the assertions it must pass. An assertion is a deterministic
check over the driver's transcript (the replies, the tool calls, the final on-device
state: files, standing rules, memory rows) - a check of the thing the owner actually
hit. The judge model is a soft signal only (run_eval.py records it separately);
pass/fail comes from here.

Each assertion is a function (scenario, transcript, params) -> (pass, evidence),
registered under an id the scenarios reference:
  {"id": "file_row", "params": {"text": "skyr", "value": 262}}

Assertions check file lines and replies, never a domain field by name: a scenario's
"tracking" key names its file and columns (food, expenses and training are three
validation domains of equal weight; owner ruling 41, lane RB-REC7).

The day: the driver expands {{TODAY}} with the device's local day at run time;
assertions compute the same day from the local clock (a run never crosses midnight in
practice - see README).
"""

from __future__ import annotations

import csv
import io
import re
from datetime import date, timedelta

ASSERTIONS: dict = {}


def assertion(fn):
    ASSERTIONS[fn.__name__] = fn
    return fn


def known_ids() -> list:
    return sorted(ASSERTIONS)


# ---- transcript helpers ------------------------------------------------------------


def turns(t: dict) -> list:
    return t.get("turns") or []


def last_turn(t: dict) -> dict:
    ts = turns(t)
    return ts[-1] if ts else {}


def replies(t: dict) -> list:
    return [x.get("reply") or "" for x in turns(t)]


def last_reply(t: dict) -> str:
    return last_turn(t).get("reply") or ""


def tool_calls(t: dict) -> list:
    out = []
    for x in turns(t):
        out += x.get("tool_calls") or []
    return out


def files(t: dict) -> dict:
    return t.get("files") or {}


# ---- the tracking file (any domain) ------------------------------------------------
# A scenario that tracks something names its file and columns once (owner ruling 41: food is one
# validation domain among three, and the core knows none of them):
#   "tracking": {"path": "money/spending.csv", "key": "date", "text": "note", "value": "amount", "unit": ""}
# key: the column a day is matched on; text: the free-text column an entry is found by; value: the
# column a day total adds up; unit: the word a stated total carries ("kcal", "kg"; "" for money).
# Rows are read the way the shared file aids read them (ruling 49): a line whose first non-blank
# byte is '#' is a comment, never a row; a row whose field count differs from the header's is NOT
# read by position (no recovery): its figures are unknown, and the assertions say so.


def tracking(sc: dict, params: dict) -> dict:
    tr = {**(sc.get("tracking") or {}), **(params.get("tracking") or {})}
    for key in ("path", "key", "text", "value"):
        if not tr.get(key):
            raise KeyError(f"the scenario names no tracking {key} (scenario key 'tracking')")
    return tr


def tracked_rows(sc: dict, t: dict, params: dict | None = None) -> list:
    """The tracking file's rows as dicts by header name, each with raw (its text), line (1-based)
    and ok (its field count is the header's: only then are its cells read)."""
    tr = tracking(sc, params or {})
    header, rows = None, []
    for n, line in enumerate(files(t).get(tr["path"], "").split("\n"), 1):
        if not line.strip() or line.lstrip().startswith("#"):
            continue
        cells = next(csv.reader(io.StringIO(line)))
        if header is None:
            header = [h.strip() for h in cells]
            continue
        ok = len(cells) == len(header)
        row = dict(zip(header, (c.strip() for c in cells))) if ok else {header[0]: cells[0].strip()}
        rows.append({**row, "raw": line, "line": n, "ok": ok})
    return rows


def today() -> str:
    return date.today().isoformat()


def day_param(params: dict) -> str:
    d = params.get("date", "today")
    if d == "today":
        return today()
    if d == "yesterday":
        return (date.today() - timedelta(days=1)).isoformat()
    return d


def day_rows(sc: dict, t: dict, params: dict) -> list:
    want, key = day_param(params), tracking(sc, params)["key"]
    return [r for r in tracked_rows(sc, t, params) if r.get(key, "").startswith(want)]


def num(s):
    try:
        return float(str(s).replace(",", "").strip())
    except (ValueError, TypeError):
        return None


def day_sum(sc: dict, t: dict, params: dict) -> tuple:
    """(the total over the day's readable rows, the day's rows a total cannot cover: wrong field
    count or a value that is not a number)."""
    col, total, unread = tracking(sc, params)["value"], 0.0, []
    for r in day_rows(sc, t, params):
        v = num(r.get(col, "")) if r["ok"] else None
        if v is not None:
            total += v
        elif not r["ok"] or r.get(col, "").strip():
            unread.append(r)
    return total, unread


def ci(hay: str, needle: str) -> bool:
    return needle.lower() in hay.lower()


# ---- generic assertions ------------------------------------------------------------


@assertion
def reply_ok(sc: dict, t: dict, params: dict):
    """The last live turn completed and answered with a real reply."""
    lt = last_turn(t)
    ok = bool(lt.get("ok")) and len(last_reply(t).strip()) >= 10
    return ok, f"ok={lt.get('ok')} reply_len={len(last_reply(t))}"


PLUMBING_RE = [
    r"\b[\w.-]+\.(csv|json|txt|md|bin)\b",  # artifact file paths
    r"row key",
    r"read back confirmed",
    r"memory id",
    r"\bmem id\b",
    r"artifact\.save",
    r"files\.(append|read|list|delete)",
    r"memory_search",
    r"\bm[0-9a-f]{7,}\b",  # memory row ids
]


@assertion
def no_plumbing(sc: dict, t: dict, params: dict):
    """No reply carries internal plumbing (file paths, row keys, tool names, ids)."""
    hits = []
    for r in replies(t):
        for pat in PLUMBING_RE:
            m = re.search(pat, r, re.IGNORECASE)
            if m:
                hits.append(f"{pat}: ...{r[max(0, m.start() - 30) : m.end() + 30]}...")
    return not hits, "; ".join(hits[:3]) if hits else "no plumbing in any reply"


@assertion
def no_correction_line(sc: dict, t: dict, params: dict):
    """No delivered reply carries a device correction/note line (nothing was withdrawn)."""
    hits = []
    for r in replies(t):
        for mark in ("\n\n(Correction:", "\n\n(Note:"):
            if mark in r:
                hits.append(r[r.find(mark) :][:80])
    return not hits, "; ".join(hits) if hits else "no correction line in any reply"


@assertion
def reply_max_len(sc: dict, t: dict, params: dict):
    """The graded reply is not a raw dump (params: max, default 4000 chars)."""
    mx = int(params.get("max", 4000))
    n = len(last_reply(t))
    return n <= mx, f"last reply {n} chars (max {mx})"


@assertion
def reply_contains_any(sc: dict, t: dict, params: dict):
    """The graded reply contains at least one of params['any'] (case-insensitive)."""
    r = last_reply(t)
    hits = [w for w in params.get("any", []) if ci(r, w)]
    return bool(hits), f"matched {hits}" if hits else f"none of {params.get('any')} in reply"


@assertion
def reply_contains_all(sc: dict, t: dict, params: dict):
    r = last_reply(t)
    missing = [w for w in params.get("all", []) if not ci(r, w)]
    return not missing, f"missing {missing}" if missing else "all present"


@assertion
def reply_forbids(sc: dict, t: dict, params: dict):
    """No reply contains any of params['forbidden'] (case-insensitive substrings)."""
    hits = []
    for r in replies(t):
        for w in params.get("forbidden", []):
            if ci(r, w):
                hits.append(w)
    return not hits, f"found {sorted(set(hits))}" if hits else "none present"


# Words that make a clause a withdrawal: the head is taking a claim back, not making it.
WITHDRAWN_CUES = (
    "wrong", "mistake", "no message", "any message", "can't find", "cannot find", "can not find", "never said",
    "didn't say", "did not say", "not something you", "no record", "no source", "not backed", "shouldn't have",
    "should not have", "don't have", "do not have", "found no", "not find", "nothing showing", "withdraw",
    "retract", "apolog", "sorry", "not treating", "claimed",
)  # fmt: skip
CLAUSE_BREAK = re.compile(r"[.;!?\n,]|\b(?:but|however|although|though|yet|still)\b", re.IGNORECASE)


def _quoted(text: str, at: int) -> bool:
    """`at` lies inside a double-quoted span (straight pair, or a curly open without its close)."""
    before = text[:at]
    return before.count('"') % 2 == 1 or before.rfind("\u201c") > before.rfind("\u201d")


def _withdrawn(text: str, at: int) -> bool:
    """The occurrence at `at` is quoted, or stands in a clause that already holds a withdrawal cue before it."""
    if _quoted(text, at):
        return True
    start = 0
    for m in CLAUSE_BREAK.finditer(text[:at]):
        start = m.end()
    clause = text[start:at].lower().replace("\u2019", "'")
    return any(cue in clause for cue in WITHDRAWN_CUES)


@assertion
def reply_forbids_unless_withdrawn(sc: dict, t: dict, params: dict):
    """No reply makes any of params['forbidden'] as a claim of its own (case-insensitive substrings).

    An honest retraction names the line it withdraws ("I was wrong to say 'as you told me'",
    "I shouldn't have stated it as something you told me"). A literal substring check graded
    every such retraction as a defense (REGR-RUN-2: all five claude reps of lc-defended-hallucination
    were retractions, and the same on mistral-large-4-0), so an occurrence is excused when it sits
    inside a quotation or in a clause that already carries a withdrawal cue before it. A defense
    ("as you told me, the burger was logged"), and a claim that follows a withdrawal in another
    clause or sentence ("That was wrong. You told me you had it"), still fail."""
    hits = []
    for r in replies(t):
        low = r.lower()
        for w in params.get("forbidden", []):
            at = low.find(w.lower())
            while at != -1:
                if not _withdrawn(r, at):
                    hits.append(w)
                    break
                at = low.find(w.lower(), at + 1)
    return not hits, f"found {sorted(set(hits))} as a claim" if hits else "none claimed"


@assertion
def reply_last_forbids(sc: dict, t: dict, params: dict):
    """The graded (last) reply contains none of params['forbidden'].

    For scenarios whose graded turn is not the first one (a scheduled routine
    firing after an owner turn, a synthesis after a sub): an earlier reply may
    legitimately say the forbidden word (the owner turn that sets up the rule),
    only the graded reply must not."""
    r = last_reply(t)
    hits = [w for w in params.get("forbidden", []) if ci(r, w)]
    return not hits, f"found {hits} in the graded reply" if hits else "none in the graded reply"


@assertion
def reply_forbids_re(sc: dict, t: dict, params: dict):
    """No reply matches any regex in params['forbidden_re']."""
    hits = []
    for r in replies(t):
        for pat in params.get("forbidden_re", []):
            if re.search(pat, r, re.IGNORECASE):
                hits.append(pat)
    return not hits, f"matched {hits}" if hits else "no match"


@assertion
def reply_contains_re(sc: dict, t: dict, params: dict):
    """The graded reply matches params['re'] (regex, case-insensitive)."""
    m = re.search(params["re"], last_reply(t), re.IGNORECASE)
    return bool(m), (m.group(0) if m else f"no match for {params['re']}")


@assertion
def request_contains(sc: dict, t: dict, params: dict):
    """The graded turn's first provider request carried params['text'] (case-sensitive).

    What the head had before any tool call: the history fix's invariant is that the
    conversation reaches the request (a head that searches the log for what was left out
    still answers, so the reply alone cannot show the cut). Needs the driver's request0."""
    body = last_turn(t).get("request0")
    if body is None:
        return False, "no request0 in the transcript (driver too old, or the turn sent nothing)"
    ok = params["text"] in body
    return ok, f"{params['text']!r} {'in' if ok else 'missing from'} the {len(body)} B first request"


@assertion
def tool_called(sc: dict, t: dict, params: dict):
    """Some tool call contains params['name'] (e.g. 'web.' for any web lookup)."""
    name = params.get("name", "")
    hits = [c for c in tool_calls(t) if name.lower() in c.lower()]
    return bool(hits), f"{len(hits)} call(s) with {name!r}" if hits else f"no tool call with {name!r}"


# ---- tracking-file assertions (file lines, any domain) ------------------------------


def _text_hits(sc: dict, t: dict, params: dict, needle: str) -> list:
    """Rows whose text column (the whole line when the column is "*") holds needle; with
    params['date'] only rows of that day."""
    col = tracking(sc, params)["text"]
    rows = day_rows(sc, t, params) if "date" in params else tracked_rows(sc, t, params)
    return [r for r in rows if ci(r["raw"] if col == "*" else r.get(col, r["raw"]), needle)]


@assertion
def file_row(sc: dict, t: dict, params: dict):
    """A readable row with params['text'] in its text column and value == params['value'] (null:
    the cell is empty, an unknown) and each column of params['cols'] equal when given; with
    params['date'] the row must be of that day."""
    col, want, cols = tracking(sc, params)["value"], params.get("value"), params.get("cols") or {}
    for r in _text_hits(sc, t, params, params.get("text", "")):
        if r["ok"] and num(r.get(col)) == want and all(num(r.get(k)) == v for k, v in cols.items()):
            return True, f"line {r['line']}: {r['raw']}"
    unread = [r["line"] for r in _text_hits(sc, t, params, params.get("text", "")) if not r["ok"]]
    why = f"; lines {unread} hold it with a wrong field count" if unread else ""
    return False, f"no readable row with {params.get('text')!r} {col}={want} {cols or ''}{why}"


@assertion
def file_row_value_near(sc: dict, t: dict, params: dict):
    """A readable row with params['text'] has its value within params['tol'] of params['expected']."""
    col, exp, tol = tracking(sc, params)["value"], float(params.get("expected", 0)), float(params.get("tol", 3))
    for r in _text_hits(sc, t, params, params.get("text", "")):
        v = num(r.get(col)) if r["ok"] else None
        if v is not None and abs(v - exp) <= tol:
            return True, f"line {r['line']}: {r['raw']} ({col} {v:g})"
    return False, f"no readable row with {params.get('text')!r} near {exp:g} +/- {tol:g}"


@assertion
def file_row_value_ne(sc: dict, t: dict, params: dict):
    """No row with params['text'] carries the wrong value params['value'] in the value column (or
    params['col']), readable or not: a wrong figure in a row with a wrong field count is still in
    the file."""
    col, bad_v = params.get("col") or tracking(sc, params)["value"], params.get("value")
    bad = [r["raw"] for r in _text_hits(sc, t, params, params.get("text", ""))
           if (num(r.get(col)) == bad_v if r["ok"] else re.search(rf"(?<![\d.]){bad_v:g}(?![\d.])", r["raw"]))]
    return not bad, f"wrong row present: {bad}" if bad else f"no {params.get('text')!r} row at {bad_v:g}"


@assertion
def file_contains_any(sc: dict, t: dict, params: dict):
    """Some row's text column holds one of params['any']."""
    col = tracking(sc, params)["text"]
    text = " ".join(r["raw"] if col == "*" else r.get(col, r["raw"]) for r in tracked_rows(sc, t, params))
    hits = [w for w in params.get("any", []) if ci(text, w)]
    return bool(hits), f"matched {hits}" if hits else f"none of {params.get('any')} in the file's rows"


@assertion
def file_forbids(sc: dict, t: dict, params: dict):
    """No row of the tracking file contains params['forbidden']; where=files/all also checks
    every stored file, where=reply/all also checks every reply."""
    where = params.get("where", "file")
    hay = [r["raw"] for r in tracked_rows(sc, t, params)]
    if where in ("files", "all"):
        hay += [f"{k}={v}" for k, v in files(t).items()]
    if where in ("reply", "all"):
        hay += replies(t)
    hits = sorted({w for h in hay for w in params.get("forbidden", []) if ci(h, w)})
    return not hits, f"found {hits}" if hits else "none present"


@assertion
def file_row_count(sc: dict, t: dict, params: dict):
    """At least params['min'] (and at most params['max'] when given) rows for the day."""
    n = len(day_rows(sc, t, params))
    ok = n >= int(params.get("min", 1)) and ("max" not in params or n <= int(params["max"]))
    return ok, f"{n} rows (min {params.get('min')}, max {params.get('max', '-')})"


def _stated(reply: str, unit: str) -> tuple:
    """(figures stated as a total, figures stated in plain day wording) in a reply."""
    fig = r"(-?\d[\d,]*(?:\.\d+)?)"
    u = rf"\s*{re.escape(unit)}" if unit else ""
    cur = r"[£$€]?\s*"
    strong = re.findall(rf"total[^.\n]{{0,60}}?{cur}{fig}{u}", reply, re.IGNORECASE)
    weak = re.findall(rf"\b(?:so far|today|day|spent)\b[^.\n]{{0,60}}?{cur}{fig}{u}", reply, re.IGNORECASE)
    weak += re.findall(rf"{cur}{fig}{u}[^.\n]{{0,40}}?\b(?:so far today|today so far|for today|today)\b", reply, re.IGNORECASE)
    clean = lambda xs: [v for v in (num(x) for x in xs) if v is not None]  # noqa: E731
    return clean(strong), clean(weak)


def _names(reply: str, row: dict) -> bool:
    """The reply points at a row: its line number, or the first word of its free text (third field)."""
    if re.search(rf"\blines? {row['line']}\b", reply):
        return True
    cells = row["raw"].split(",")
    word = re.match(r"[^\W\d_]{3,}", cells[2].strip()) if len(cells) > 2 else None
    return bool(word) and ci(reply, word.group(0))


@assertion
def file_day_total(sc: dict, t: dict, params: dict):
    """The graded reply states the day's figure and it equals the device sum over the day's
    readable rows in the final file (the read_file sum). A stated 'total' that disagrees always
    fails. With require_stated (default true) the reply must state the matching figure at all.
    When the day has rows a total cannot cover (a wrong field count, a value that is not a
    number), the reply must name them (by line, or by their text) - ruling 49: a total that
    silently misses rows is a FAIL."""
    tr = tracking(sc, params)
    expected, unread = day_sum(sc, t, params)
    r = last_reply(t)
    strong, weak = _stated(r, tr.get("unit", ""))
    stated = strong + weak
    full = params.get("full")  # the figure over every row as written, when the rows say it plainly
    if full is not None and any(abs(v - float(full)) <= 0.005 for v in stated):
        return True, f"the full figure {float(full):g} stated (stated {stated})"
    if params.get("require_stated", True) and not any(abs(v - expected) <= 0.005 for v in stated):
        return False, f"no day figure stated matching the device sum {expected:g} (stated {stated})"
    bad = [v for v in strong if abs(v - expected) > 0.005]
    if bad:
        return False, f"reply totals {strong} != device sum {expected:g}"
    if unread and any(abs(v - expected) <= 0.005 for v in stated) and not any(_names(r, u) for u in unread):
        return False, f"total {expected:g} leaves out lines {[u['line'] for u in unread]} and the reply does not say so"
    return True, f"day figure stated {stated} == device sum {expected:g}"


@assertion
def memory_contains(sc: dict, t: dict, params: dict):
    """The long-term (vector) store holds a row containing params['text']: the write landed.

    The transcript's `memory` list is the EPISODIC chat log. It always holds the owner's own
    message and the reply, so reading it made this assertion unable to fail (lane QFIX fix pass,
    review finding 2: lc-save-status and lc-embed-parked passed in every rep of a head that made
    no write at all). `durable_memory` is the long-term store the driver dumps; it holds only
    what a memory write stored. A record without it (an older driver) cannot show a write landed,
    so it fails rather than passes. The scenario must set "localMemory": true: the driver runs
    with embeddings off, where every write is refused, unless it is given the local store."""
    if "durable_memory" not in t:
        return False, "the transcript has no durable_memory (older driver): cannot show a write landed"
    hay = " ".join(m.get("content", "") for m in t.get("durable_memory") or [])
    ok = ci(hay, params.get("text", ""))
    return (
        ok,
        "found in long-term memory"
        if ok
        else f"{params.get('text')!r} not in the long-term store ({len(t.get('durable_memory') or [])} rows)",
    )


@assertion
def memory_forbids(sc: dict, t: dict, params: dict):
    hay = " ".join(m.get("text", "") for m in t.get("memory") or [])
    hits = [w for w in params.get("forbidden", []) if ci(hay, w)]
    return not hits, f"found {sorted(set(hits))}" if hits else "none in memory"


@assertion
def prefs_active(sc: dict, t: dict, params: dict):
    """A standing rule containing params['text'] is active (not pending, not evicted)."""
    for p in t.get("prefs") or []:
        if ci(p.get("text", ""), params.get("text", "")) and not p.get("pending") and not p.get("evicted"):
            return True, f"active rule {p.get('id')}: {p.get('text')!r}"
    return False, f"no active rule containing {params.get('text')!r}"


def expand_days(text: str) -> str:
    t0 = date.today()
    repl = {
        "{{TODAY}}": t0.isoformat(),
        "{{YESTERDAY}}": (t0 - timedelta(days=1)).isoformat(),
        "{{TOMORROW}}": (t0 + timedelta(days=1)).isoformat(),
    }
    for k, v in repl.items():
        text = text.replace(k, v)
    return text


@assertion
def file_written(sc: dict, t: dict, params: dict):
    """files[params['path']] exists and contains params['contains'] (if given)."""
    path = params["path"]
    text = files(t).get(path)
    if text is None:
        return False, f"{path} absent from final files"
    if "contains" in params and not ci(text, params["contains"]):
        return False, f"{path} does not contain {params['contains']!r}"
    return True, f"{path} present ({len(text)} bytes)"


@assertion
def file_unchanged(sc: dict, t: dict, params: dict):
    """files[params['path']] is byte-identical to the scenario's seeded text."""
    path = params["path"]
    seed = None
    for st in sc.get("steps", []):
        if st.get("do") == "file" and st.get("path") == path:
            seed = expand_days(st.get("text", ""))
    if seed is None:
        return False, f"scenario does not seed {path}"
    now = files(t).get(path)
    return now == seed, "unchanged" if now == seed else f"changed ({len(now or '')} vs {len(seed)} bytes)"


# ---- runner ------------------------------------------------------------------------


def run_assertion(entry, sc: dict, t: dict) -> dict:
    """One scenario assertion entry -> {id, pass, evidence}. Unknown ids fail loudly."""
    aid = entry["id"] if isinstance(entry, dict) else entry
    params = entry.get("params", {}) if isinstance(entry, dict) else {}
    fn = ASSERTIONS.get(aid)
    if fn is None:
        return {"id": aid, "pass": False, "evidence": f"unknown assertion id {aid!r}"}
    try:
        ok, ev = fn(sc, t, params)
    except Exception as e:  # an assertion that cannot run is a failure, not a pass
        return {"id": aid, "pass": False, "evidence": f"assertion error: {e}"}
    return {"id": aid, "pass": bool(ok), "evidence": ev}


def run_all(sc: dict, t: dict) -> list:
    return [run_assertion(e, sc, t) for e in sc.get("assertions", [])]
