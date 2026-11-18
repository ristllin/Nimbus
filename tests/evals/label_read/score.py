#!/usr/bin/env python3
"""score - field accuracy of one label read against the hand-verified truth in labels.json.

Pure functions, no network. A field is one printed figure of one column (per 100 g or ml, or per
serving). It is correct when the read figure equals the printed one after unit and decimal-comma
normalisation; a missing or extra '<' is wrong, and so is a figure read from the wrong column.
"""

from __future__ import annotations

import re

FIELDS = (
    "energy_kj",
    "energy_kcal",
    "fat",
    "saturates",
    "carbohydrate",
    "sugars",
    "polyols",
    "fibre",
    "protein",
    "salt",
    "sodium_mg",
)
BASES = ("per100", "serving")

_UNIT = re.compile(r"(kcal|kj|mg|g|ml|cal)\b", re.I)


def norm(v):
    """A printed figure as a comparable token: '<0.5', '12.0', 'trace', or None when absent."""
    if v is None:
        return None
    s = str(v).strip().lower().replace(" ", "").replace(" ", "")
    if not s or s in ("-", "null", "none", "n/a"):
        return None
    if s.startswith(("trace", "tr", "spuren", "traces")):
        return "trace"
    s = _UNIT.sub("", s).replace(",", ".").strip("()")
    lt = s.startswith("<") or s.startswith("&lt;")
    s = s.lstrip("<").removeprefix("&lt;")
    try:
        x = float(s)
    except ValueError:
        return s
    return ("<" if lt else "") + repr(round(x, 3))


def same(a, b) -> bool:
    na = norm(a)
    return na is not None and na == norm(b)


def columns(read: dict) -> dict:
    """The read's columns keyed by basis; the first column of each basis wins."""
    out = {}
    for c in (read or {}).get("columns") or []:
        if not isinstance(c, dict):
            continue
        b = c.get("basis_kind")
        if b in BASES and b not in out:
            out[b] = c
    return out


def score(truth: dict, read: dict) -> dict:
    """Per-field results for one label: every printed field of every truth column.

    extras counts figures the read gives for a row the label does not print (sodium aside: a label
    that prints sodium in g has no scored sodium row), and every figure of a column the label does not
    print at all.
    """
    got = columns(read)
    unscored = set(truth.get("unscored") or [])
    fields, kj_kcal, protein, extras = [], 0, 0, 0
    for basis, tcol in truth["columns"].items():
        rcol = got.get(basis) or {}
        extras += sum(
            1
            for f in FIELDS
            if f != "sodium_mg" and f not in tcol and f"{basis}.{f}" not in unscored and norm(rcol.get(f)) is not None
        )
        for f in FIELDS:
            if f not in tcol:
                continue
            ok = same(rcol.get(f), tcol[f])
            fields.append({"basis": basis, "field": f, "truth": tcol[f], "read": rcol.get(f), "ok": ok})
            if f == "protein" and not ok:
                protein += 1
        if kj_kcal_confused(tcol, rcol):
            kj_kcal += 1
    for basis, rcol in got.items():
        if basis not in truth["columns"]:
            extras += sum(1 for f in FIELDS if f != "sodium_mg" and norm(rcol.get(f)) is not None)
    return {"fields": fields, "kj_kcal_confusions": kj_kcal, "protein_misreads": protein, "extras": extras}


def kj_kcal_confused(tcol: dict, rcol: dict) -> bool:
    """The kJ figure read as kcal, the kcal figure read as kJ, or the pair swapped."""
    tkj, tkc = norm(tcol.get("energy_kj")), norm(tcol.get("energy_kcal"))
    rkj, rkc = norm(rcol.get("energy_kj")), norm(rcol.get("energy_kcal"))
    if tkj is None or tkc is None or tkj == tkc:
        return False
    return (rkc is not None and rkc == tkj) or (rkj is not None and rkj == tkc)
