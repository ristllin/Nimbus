#!/usr/bin/env python3
"""Hard assertions for the owner-regression live suite.

Every scenario lists the assertions it must pass. An assertion is a deterministic
check over the driver's transcript (the replies, the tool calls, the final on-device
state: files, standing rules, memory rows) - a check of the thing the owner actually
hit. The judge model is a soft signal only (run_eval.py records it separately);
pass/fail comes from here.

Each assertion is a function (scenario, transcript, params) -> (pass, evidence),
registered under an id the scenarios reference:
  {"id": "ledger-row", "params": {"item": "skyr", "kcal": 262}}

The ledger day: the driver expands {{TODAY}} with the device's local day at run time;
assertions compute the same day from the local clock (a run never crosses midnight in
practice - see README).
"""

from __future__ import annotations

import csv
import io
import re
from datetime import date, timedelta

FOOD_LEDGER = "food/food-log.csv"

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


def ledger_rows(t: dict, path: str = FOOD_LEDGER) -> list:
    """The food ledger's rows as dicts (header date,meal,items,kcal,protein_g)."""
    text = files(t).get(path, "")
    rows = []
    for row in csv.reader(io.StringIO(text)):
        if not row or row[0].strip().lower() == "date":
            continue
        rows.append(
            {
                "date": row[0].strip() if len(row) > 0 else "",
                "meal": row[1].strip() if len(row) > 1 else "",
                "items": row[2].strip() if len(row) > 2 else "",
                "kcal": row[3].strip() if len(row) > 3 else "",
                "protein_g": row[4].strip() if len(row) > 4 else "",
                "raw": ",".join(row),
            }
        )
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


def day_rows(t: dict, params: dict) -> list:
    want = day_param(params)
    return [r for r in ledger_rows(t) if r["date"] == want]


def num(s):
    try:
        return float(str(s).replace(",", "").strip())
    except (ValueError, TypeError):
        return None


def day_kcal_sum(t: dict, params: dict) -> float:
    total = 0.0
    for r in day_rows(t, params):
        v = num(r["kcal"])
        if v is not None:
            total += v
    return total


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
def tool_called(sc: dict, t: dict, params: dict):
    """Some tool call contains params['name'] (e.g. 'web.' for any web lookup)."""
    name = params.get("name", "")
    hits = [c for c in tool_calls(t) if name.lower() in c.lower()]
    return bool(hits), f"{len(hits)} call(s) with {name!r}" if hits else f"no tool call with {name!r}"


# ---- ledger / state assertions -----------------------------------------------------


@assertion
def ledger_row(sc: dict, t: dict, params: dict):
    """A food-ledger row exists with params['item'] in items and kcal == params['kcal']
    (and protein_g == params['protein'] when given)."""
    item, kcal = params.get("item", ""), params.get("kcal")
    prot = params.get("protein")
    for r in ledger_rows(t):
        if item.lower() in r["items"].lower() and num(r["kcal"]) == kcal:
            if prot is None or num(r["protein_g"]) == prot:
                return True, f"row: {r['raw']}"
    return False, f"no row with item~{item!r} kcal={kcal} protein={prot}"


@assertion
def ledger_row_kcal_near(sc: dict, t: dict, params: dict):
    """A ledger row with params['item'] has kcal within params['tol'] of params['expected']."""
    item = params.get("item", "")
    exp = float(params.get("expected", 0))
    tol = float(params.get("tol", 3))
    for r in ledger_rows(t):
        if item.lower() in r["items"].lower():
            v = num(r["kcal"])
            if v is not None and abs(v - exp) <= tol:
                return True, f"row: {r['raw']} (kcal {v})"
    return False, f"no row with item~{item!r} near {exp} +/- {tol}"


@assertion
def ledger_row_kcal_ne(sc: dict, t: dict, params: dict):
    """No ledger row with params['item'] has kcal == params['kcal'] (the misread figure)."""
    item, kcal = params.get("item", ""), params.get("kcal")
    bad = [r["raw"] for r in ledger_rows(t) if item.lower() in r["items"].lower() and num(r["kcal"]) == kcal]
    return not bad, f"misread row present: {bad}" if bad else f"no {item!r} row at {kcal} kcal"


@assertion
def ledger_contains_any(sc: dict, t: dict, params: dict):
    """Some ledger row's items contain one of params['any']."""
    text = " ".join(r["items"] for r in ledger_rows(t))
    hits = [w for w in params.get("any", []) if ci(text, w)]
    return bool(hits), f"matched {hits}" if hits else f"none of {params.get('any')} in ledger items"


@assertion
def ledger_forbids(sc: dict, t: dict, params: dict):
    """No ledger row contains params['forbidden']; where=files/all also checks every
    stored file, where=reply/all also checks every reply."""
    where = params.get("where", "ledger")
    hay = [r["raw"] for r in ledger_rows(t)]
    if where in ("files", "all"):
        hay += [f"{k}={v}" for k, v in files(t).items()]
    if where in ("reply", "all"):
        hay += replies(t)
    hits = []
    for h in hay:
        for w in params.get("forbidden", []):
            if ci(h, w):
                hits.append(w)
    return not hits, f"found {sorted(set(hits))}" if hits else "none present"


@assertion
def ledger_row_count(sc: dict, t: dict, params: dict):
    """At least params['min'] ledger rows for the day (params['date'], default today)."""
    n = len(day_rows(t, params))
    return n >= int(params.get("min", 1)), f"{n} rows (min {params.get('min')})"


@assertion
def ledger_day_total(sc: dict, t: dict, params: dict):
    """Every 'total ... N kcal' the graded reply states equals the receipt: the sum of
    the day's kcal rows in the final ledger. With require_stated (default true) the
    reply must state a total at all."""
    expected = day_kcal_sum(t, params)
    r = last_reply(t)
    matches = re.findall(r"total[^.\n]{0,60}?(\d[\d,\.]*)\s*kcal", r, re.IGNORECASE)
    stated = [v for v in (num(m) for m in matches) if v is not None]
    require = params.get("require_stated", True)
    if require and not stated:
        return False, f"no total stated in reply (receipt says {expected:g} kcal)"
    bad = [v for v in stated if abs(v - expected) > 0.5]
    if bad:
        return False, f"reply totals {stated} != receipt {expected:g} kcal"
    return True, f"reply totals {stated} == receipt {expected:g} kcal"


@assertion
def memory_contains(sc: dict, t: dict, params: dict):
    """The final memory rows contain params['text'] (the write actually landed)."""
    hay = " ".join(m.get("text", "") for m in t.get("memory") or [])
    ok = ci(hay, params.get("text", ""))
    return ok, "found in memory" if ok else f"{params.get('text')!r} not in memory rows"


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
