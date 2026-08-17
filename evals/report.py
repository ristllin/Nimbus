#!/usr/bin/env python3
"""report - render one or more benchmark corpora into a comparable report.

Replaces the earlier compare / gen_report / render_history / render_fabric_report
scripts (whose wedge-detection rules disagreed) with ONE aggregation function and
ONE wedge rule. Reads corpus JSONL files (the schema `qa_recorder.py` writes; see
SCHEMA.md), aggregates per-file and per-category, and prints a report. With two or
more files it also prints deltas (first file -> last file).

Usage:
  python3 evals/report.py <a.jsonl> [<b.jsonl> ...] [--format text|md|html]

Formats:
  text  ASCII table + a per-scenario grid (P pass / F fail / - not run). Default.
  md    GitHub-flavored Markdown tables.
  html  a self-contained page with minimal inlined CSS (no external deps).
"""

from __future__ import annotations

import argparse
import html
import json
import os
import sys
from collections import defaultdict

HERE = os.path.dirname(os.path.abspath(__file__))

# --------------------------------------------------------------------------- #
# THE one wedge/timeout rule (single source of truth).
#
# A rep is "wedged" (a device/runner failure, distinct from a dishonest reply)
# when EITHER the reply is empty OR the oracle detail names a terminal failure.
# The earlier scripts disagreed on the terminal-failure set; this is the
# documented superset: "timed out" OR "wedge" OR "runner error".
# --------------------------------------------------------------------------- #
WEDGE_MARKERS = ("timed out", "wedge", "runner error")


def is_wedge(row: dict) -> bool:
    oracle = row.get("oracle") or {}
    detail = (oracle.get("detail") or "").lower()
    if not (row.get("reply") or ""):
        return True
    return any(marker in detail for marker in WEDGE_MARKERS)


def category_map() -> dict:
    """id -> category, from the scenario source of truth (best effort).

    Returns {} on a checkout without the HIL package importable; callers then
    fall back to '?' so the report still renders.
    """
    try:
        sys.path.insert(0, os.path.join(HERE, "..", "tests", "hil"))
        from scenarios_complex import SCENARIOS  # noqa: E402

        return {s["id"]: s["cat"] for s in SCENARIOS}
    except Exception:
        return {}


def mean(xs: list) -> float:
    return sum(xs) / len(xs) if xs else 0.0


def load(path: str) -> list[dict]:
    if os.path.isdir(path):
        path = os.path.join(path, "corpus.jsonl")
    with open(path) as f:
        return [json.loads(line) for line in f if line.strip()]


def aggregate(rows: list[dict], catmap: dict) -> dict:
    """The ONE canonical aggregation. Returns per-file totals, per-category
    breakdown, and per-scenario pass/fail (same rule as summarize.py)."""
    total = {"n": 0, "oracle_pass": 0, "wedge": 0, "honest_viol": 0, "scores": []}
    by_cat: dict[str, dict] = defaultdict(lambda: {"n": 0, "oracle_pass": 0, "wedge": 0, "scores": []})
    per_scenario: dict[str, dict] = {}

    for r in rows:
        sid = r.get("scenario", "?")
        cat = catmap.get(sid, "?")
        oracle = r.get("oracle") or {}
        judge = r.get("judge") or {}
        wedged = is_wedge(r)
        oracle_pass = oracle.get("passed") is True
        score = judge.get("score")

        for bucket in (total, by_cat[cat]):
            bucket["n"] += 1
            if oracle_pass:
                bucket["oracle_pass"] += 1
            if wedged:
                bucket["wedge"] += 1
            if isinstance(score, (int, float)):
                bucket["scores"].append(score)

        # An honesty violation counts only on a reply that actually landed - a
        # wedge is a device failure, not a lie.
        if not wedged and judge.get("honest") is False:
            total["honest_viol"] += 1

        ps = per_scenario.setdefault(sid, {"reps": 0, "oracle_fails": 0, "judge_fails": 0, "wedge": 0})
        ps["reps"] += 1
        if oracle.get("passed") is False:
            ps["oracle_fails"] += 1
        if judge.get("honest") is False:
            ps["judge_fails"] += 1
        if wedged:
            ps["wedge"] += 1

    for ps in per_scenario.values():
        ps["pass"] = ps["oracle_fails"] == 0 and ps["judge_fails"] == 0

    return {"total": total, "by_cat": dict(by_cat), "per_scenario": per_scenario}


def label_for(path: str) -> str:
    base = os.path.basename(path.rstrip("/"))
    if base == "corpus.jsonl":
        base = os.path.basename(os.path.dirname(path.rstrip("/"))) or base
    return os.path.splitext(base)[0]


# --------------------------------------------------------------------------- #
# Renderers
# --------------------------------------------------------------------------- #
def _metric_rows(files: list[dict]) -> list[tuple]:
    """(label, value) tuples per file for the headline metrics."""
    out = []
    for f in files:
        t = f["agg"]["total"]
        out.append(
            (
                f["label"],
                t["n"],
                t["oracle_pass"],
                mean(t["scores"]),
                t["honest_viol"],
                t["wedge"],
            )
        )
    return out


def render_text(files: list[dict]) -> str:
    lines = ["Nimbus eval report", ""]
    lines.append(f"{'run':22} {'n':>4} {'oraclePass':>11} {'meanJudge':>10} {'honestViol':>11} {'wedged':>7}")
    for label, n, op, mj, hv, wd in _metric_rows(files):
        lines.append(f"{label[:22]:22} {n:>4} {op:>11} {mj:>10.2f} {hv:>11} {wd:>7}")

    if len(files) >= 2:
        a, b = files[0], files[-1]
        ta, tb = a["agg"]["total"], b["agg"]["total"]
        lines += ["", f"deltas ({a['label']} -> {b['label']})"]
        lines.append(f"{'metric':22} {'from':>10} {'to':>10} {'delta':>8}")

        def drow(name, va, vb, fmt="{:d}"):
            d = vb - va
            ds = f"{d:+.2f}" if isinstance(d, float) else f"{d:+d}"
            lines.append(f"{name:22} {fmt.format(va):>10} {fmt.format(vb):>10} {ds:>8}")

        drow("oracle pass", ta["oracle_pass"], tb["oracle_pass"])
        drow("mean judge", mean(ta["scores"]), mean(tb["scores"]), "{:.2f}")
        drow("honesty violations", ta["honest_viol"], tb["honest_viol"])
        drow("wedged", ta["wedge"], tb["wedge"])

    # per-category (last file)
    last = files[-1]
    cats = last["agg"]["by_cat"]
    if cats:
        lines += [
            "",
            f"per-category ({last['label']})",
            f"{'category':22} {'n':>4} {'oraclePass':>11} {'meanJudge':>10} {'wedged':>7}",
        ]
        for cat in sorted(cats):
            c = cats[cat]
            lines.append(f"{cat[:22]:22} {c['n']:>4} {c['oracle_pass']:>11} {mean(c['scores']):>10.2f} {c['wedge']:>7}")

    # scenario grid
    all_scen = sorted({s for f in files for s in f["agg"]["per_scenario"]})
    if all_scen:
        lines += ["", "scenario grid (P pass / F fail / - not run)"]
        header = f"{'scenario':40} " + " ".join(f"{f['label'][:10]:>10}" for f in files)
        lines.append(header)
        for sid in all_scen:
            cells = []
            for f in files:
                e = f["agg"]["per_scenario"].get(sid)
                cells.append(f"{('P' if e['pass'] else 'F') if e else '-':>10}")
            lines.append(f"{sid[:40]:40} " + " ".join(cells))
    return "\n".join(lines) + "\n"


def render_md(files: list[dict]) -> str:
    lines = [
        "# Nimbus eval report",
        "",
        "| run | n | oracle pass | mean judge | honesty viol | wedged |",
        "|---|---:|---:|---:|---:|---:|",
    ]
    for label, n, op, mj, hv, wd in _metric_rows(files):
        lines.append(f"| {label} | {n} | {op} | {mj:.2f} | {hv} | {wd} |")

    if len(files) >= 2:
        a, b = files[0], files[-1]
        ta, tb = a["agg"]["total"], b["agg"]["total"]
        lines += [
            "",
            f"## Deltas ({a['label']} -> {b['label']})",
            "",
            "| metric | from | to | delta |",
            "|---|---:|---:|---:|",
        ]
        rows = [
            ("oracle pass", ta["oracle_pass"], tb["oracle_pass"], False),
            ("mean judge", mean(ta["scores"]), mean(tb["scores"]), True),
            ("honesty violations", ta["honest_viol"], tb["honest_viol"], False),
            ("wedged", ta["wedge"], tb["wedge"], False),
        ]
        for name, va, vb, is_float in rows:
            if is_float:
                lines.append(f"| {name} | {va:.2f} | {vb:.2f} | {vb - va:+.2f} |")
            else:
                lines.append(f"| {name} | {va} | {vb} | {vb - va:+d} |")

    all_scen = sorted({s for f in files for s in f["agg"]["per_scenario"]})
    if all_scen:
        lines += [
            "",
            "## Scenario grid (P pass / F fail / - not run)",
            "",
            "| scenario | " + " | ".join(f["label"] for f in files) + " |",
            "|---|" + "---|" * len(files),
        ]
        for sid in all_scen:
            cells = []
            for f in files:
                e = f["agg"]["per_scenario"].get(sid)
                cells.append(("P" if e["pass"] else "F") if e else "-")
            lines.append(f"| {sid} | " + " | ".join(cells) + " |")
    return "\n".join(lines) + "\n"


_HTML_CSS = """
:root { color-scheme: light dark; }
body { font: 15px/1.5 system-ui, sans-serif; margin: 2rem auto; max-width: 60rem;
       padding: 0 1rem; color: #1a1a1a; background: #fff; }
h1 { font-size: 1.4rem; } h2 { font-size: 1.1rem; margin-top: 2rem; }
table { border-collapse: collapse; width: 100%; margin: 1rem 0; font-size: 0.9rem; }
th, td { padding: 0.4rem 0.6rem; text-align: right; border-bottom: 1px solid #ddd; }
th:first-child, td:first-child { text-align: left; }
thead th { border-bottom: 2px solid #999; }
.pass { color: #1a7f37; font-weight: 600; } .fail { color: #b3261e; font-weight: 600; }
.na { color: #999; }
@media (prefers-color-scheme: dark) {
  body { color: #e6e6e6; background: #121212; }
  th, td { border-color: #333; } thead th { border-color: #666; }
  .pass { color: #4ac26b; } .fail { color: #ff6b5e; } .na { color: #777; }
}
""".strip()


def _cell(mark: str) -> str:
    if mark == "P":
        return '<td class="pass">P</td>'
    if mark == "F":
        return '<td class="fail">F</td>'
    return '<td class="na">-</td>'


def render_html(files: list[dict]) -> str:
    esc = html.escape
    p = [f"<style>{_HTML_CSS}</style>", "<h1>Nimbus eval report</h1>"]
    p.append(
        "<table><thead><tr><th>run</th><th>n</th><th>oracle pass</th>"
        "<th>mean judge</th><th>honesty viol</th><th>wedged</th></tr></thead><tbody>"
    )
    for label, n, op, mj, hv, wd in _metric_rows(files):
        p.append(
            f"<tr><td>{esc(label)}</td><td>{n}</td><td>{op}/{n}</td><td>{mj:.2f}</td><td>{hv}</td><td>{wd}</td></tr>"
        )
    p.append("</tbody></table>")

    if len(files) >= 2:
        a, b = files[0], files[-1]
        ta, tb = a["agg"]["total"], b["agg"]["total"]
        p.append(f"<h2>Deltas ({esc(a['label'])} &rarr; {esc(b['label'])})</h2>")
        p.append("<table><thead><tr><th>metric</th><th>from</th><th>to</th><th>delta</th></tr></thead><tbody>")
        p.append(
            f"<tr><td>oracle pass</td><td>{ta['oracle_pass']}</td><td>{tb['oracle_pass']}</td>"
            f"<td>{tb['oracle_pass'] - ta['oracle_pass']:+d}</td></tr>"
        )
        ma, mb = mean(ta["scores"]), mean(tb["scores"])
        p.append(f"<tr><td>mean judge</td><td>{ma:.2f}</td><td>{mb:.2f}</td><td>{mb - ma:+.2f}</td></tr>")
        p.append(
            f"<tr><td>honesty violations</td><td>{ta['honest_viol']}</td><td>{tb['honest_viol']}</td>"
            f"<td>{tb['honest_viol'] - ta['honest_viol']:+d}</td></tr>"
        )
        p.append(
            f"<tr><td>wedged</td><td>{ta['wedge']}</td><td>{tb['wedge']}</td>"
            f"<td>{tb['wedge'] - ta['wedge']:+d}</td></tr>"
        )
        p.append("</tbody></table>")

    all_scen = sorted({s for f in files for s in f["agg"]["per_scenario"]})
    if all_scen:
        p.append("<h2>Scenario grid (P pass &middot; F fail &middot; - not run)</h2>")
        p.append(
            "<table><thead><tr><th>scenario</th>"
            + "".join(f"<th>{esc(f['label'])}</th>" for f in files)
            + "</tr></thead><tbody>"
        )
        for sid in all_scen:
            row = [f"<td>{esc(sid)}</td>"]
            for f in files:
                e = f["agg"]["per_scenario"].get(sid)
                row.append(_cell(("P" if e["pass"] else "F") if e else "-"))
            p.append("<tr>" + "".join(row) + "</tr>")
        p.append("</tbody></table>")
    return "\n".join(p) + "\n"


RENDERERS = {"text": render_text, "md": render_md, "html": render_html}


def build_report(paths: list[str], fmt: str) -> str:
    catmap = category_map()
    files = []
    for path in paths:
        rows = load(path)
        files.append({"label": label_for(path), "agg": aggregate(rows, catmap)})
    return RENDERERS[fmt](files)


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description="Render benchmark corpora into a report.")
    ap.add_argument("corpora", nargs="+", help="one or more corpus .jsonl paths (2+ enables deltas)")
    ap.add_argument("--format", choices=list(RENDERERS), default="text")
    a = ap.parse_args(argv)
    sys.stdout.write(build_report(a.corpora, a.format))
    return 0


if __name__ == "__main__":
    sys.exit(main())
