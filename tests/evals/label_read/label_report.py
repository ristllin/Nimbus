#!/usr/bin/env python3
"""report - field accuracy per head and arm, the two decisions, labels.json and a RESULTS.md table.

  label_report.py --evidence <dir with one subdir per head> --out tests/evals/measured/labels.json --md <dir>/RESULTS.md

Decision rules (PRD section 5 and this lane's DECISION lines):
- Rendition: 800 px, unless the head alone at 1,280 px (P1280) reads at least one percentage point
  more fields right than at 800 px (P800) on claude-sonnet-5-5 or claude-haiku-5-5. The T arms are
  left out: both carry the same transcription, made from the 1,280 px photo the device downloads.
- Transcription, per head at the chosen rendition: kept unless the head alone (P) is at least as
  accurate as pixels plus transcription (T) and at or above 95% field accuracy. The design keeps it
  if the rule keeps it on either Claude head (one intake path serves every head).
"""

from __future__ import annotations

import argparse
import json
import statistics
import sys
from collections import defaultdict
from datetime import date
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent / "behaviour"))
sys.path.insert(0, str(HERE))
import stats  # noqa: E402  (exact Clopper-Pearson bounds from the RB-M0a eval)
from score import score  # noqa: E402

DECIDING_HEADS = ("claude-sonnet-5-5", "claude-haiku-5-5")
DECISION_ARMS = ("P800", "P1280", "T800", "T1280")
ARMS = (*DECISION_ARMS, "D800", "D1280")
RENDITION_MARGIN = 0.01
TRANSCRIPT_FLOOR = 0.95
HARD_TAGS = ("glare", "blur", "wide", "curved", "polyols", "low_light", "row_offset", "small_text", "running_text")


def jsonl(f: Path) -> list:
    """Stored rows; a line cut by a killed append is skipped."""
    out = []
    for x in f.read_text().splitlines() if f.exists() else []:
        try:
            out.append(json.loads(x))
        except ValueError:
            continue
    return out


def load(evidence: Path):
    """Every head's reads (one subdir per head) and the two shared transcription caches."""
    reads = [r for d in sorted(p for p in evidence.iterdir() if p.is_dir()) for r in jsonl(d / "reads.jsonl")]
    return reads, {"T": jsonl(evidence / "transcripts.jsonl"), "D": jsonl(evidence / "transcripts-device900.jsonl")}


def transcript_stats(rows: list) -> dict:
    return {
        "count": len(rows),
        "stopped_at_max_tokens": sum(1 for r in rows if r.get("stop") == "max_tokens"),
        "over_media_text_max": sum(1 for r in rows if len(r["text"].encode()) > 2048),
        "max_tokens": max((r.get("max_tokens", 0) for r in rows), default=0),
        "usd": round(sum(r["usd"] for r in rows), 4),
    }


def latest(reads: list) -> dict:
    """The last stored read per (label, head, arm): a rerun after a failure replaces it."""
    out = {}
    for r in reads:
        out[(r["id"], r["head"], r["arm"])] = r
    return out


def tally(labels: dict, cell: list) -> dict:
    """Raw counts over one head and arm: fields, confusions, misreads, extras, per hard tag."""
    t = {"k": 0, "n": 0, "kj": 0, "prot_bad": 0, "prot_n": 0, "extras": 0, "all_ok": 0, "failed": 0}
    by_tag = defaultdict(lambda: [0, 0])
    per_field = {}
    for r in cell:
        truth = labels[r["id"]]
        s = score(truth, r["read"] if r["ok"] else None)
        oks = [f["ok"] for f in s["fields"]]
        t["failed"] += not r["ok"]
        t["k"] += sum(oks)
        t["n"] += len(oks)
        t["all_ok"] += all(oks)
        t["kj"] += s["kj_kcal_confusions"]
        t["prot_bad"] += s["protein_misreads"]
        t["prot_n"] += sum(1 for f in s["fields"] if f["field"] == "protein")
        t["extras"] += s["extras"]
        for tag in set(truth["tags"]) & set(HARD_TAGS):
            by_tag[tag][0] += sum(oks)
            by_tag[tag][1] += len(oks)
        per_field.update({(r["id"], f["basis"], f["field"]): f["ok"] for f in s["fields"]})
    return {**t, "by_tag": by_tag, "per_field": per_field}


def mean(xs: list, digits: int):
    return round(statistics.mean(xs), digits) if xs else None


def summarise(labels: dict, cell: list) -> dict:
    t = tally(labels, cell)
    usd = [r["usd"] for r in cell]
    return {
        "labels": len(cell),
        "failed_reads": t["failed"],
        "field_accuracy": stats.rate(t["k"], t["n"], "lower"),
        "labels_all_correct": {"k": t["all_ok"], "n": len(cell)},
        "kj_kcal_confusions": t["kj"],
        "protein_misreads": {"k": t["prot_bad"], "n": t["prot_n"]},
        "extra_figures": t["extras"],
        "by_tag": {tag: {"k": v[0], "n": v[1]} for tag, v in sorted(t["by_tag"].items())},
        "usd_total": round(sum(usd), 4),
        "usd_per_read": mean(usd, 5),
        "prompt_tokens_mean": mean([r["usage"].get("prompt", 0) for r in cell], 0),
        "jpeg_bytes_mean": mean([r["jpeg_bytes"] for r in cell], 0),
        "seconds_median": round(statistics.median(r["seconds"] for r in cell), 2) if cell else None,
        "_per_field": t["per_field"],
    }


def discordant(a: dict, b: dict) -> dict:
    """Fields right in arm a and wrong in b, and the reverse, over fields scored in both."""
    keys = a.keys() & b.keys()
    return {"a_only": sum(1 for x in keys if a[x] and not b[x]), "b_only": sum(1 for x in keys if b[x] and not a[x])}


def acc(cell: dict) -> float:
    return cell["field_accuracy"]["rate"] or 0.0


def decide(heads: dict, n_labels: int) -> dict:
    """The two design decisions; refuses unless both Claude heads read every label in all four arms."""
    gains = {}
    for head in DECIDING_HEADS:
        h = heads.get(head, {})
        short = [a for a in DECISION_ARMS if h.get(a, {}).get("labels", 0) < n_labels]
        if short:
            raise SystemExit(f"{head} has not read all {n_labels} labels in {', '.join(short)}; no decision")
        gains[head] = round(acc(h["P1280"]) - acc(h["P800"]), 4)
    px = 1280 if any(g >= RENDITION_MARGIN for g in gains.values()) else 800
    per_head = {}
    for head, h in heads.items():
        p, t = h.get(f"P{px}"), h.get(f"T{px}")
        if not p or not t:
            continue
        drop = acc(p) >= acc(t) and acc(p) >= TRANSCRIPT_FLOOR
        per_head[head] = {"pixels_only": acc(p), "with_transcription": acc(t), "keep_transcription": not drop}
    keep = any(per_head[h]["keep_transcription"] for h in DECIDING_HEADS)
    return {
        "rendition_px": px,
        "rendition_rule": f"800 px unless 1,280 px pixels alone read >= {100 * RENDITION_MARGIN:.0f} point more fields right on Sonnet or Haiku",
        "rendition_gain_1280_minus_800": gains,
        "transcription_rule": "kept unless the head alone is at least as accurate and at or above 95% field accuracy (PRD section 5)",
        "transcription_per_head": per_head,
        "keep_transcription": keep,
    }


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--evidence", required=True)
    ap.add_argument("--manifest", default=str(HERE / "labels.json"))
    ap.add_argument("--out", required=True)
    ap.add_argument("--md", required=True)
    args = ap.parse_args()
    man = json.loads(Path(args.manifest).read_text())
    labels = {lab["id"]: lab for lab in man["labels"]}
    reads, transcripts = load(Path(args.evidence))
    cells = defaultdict(list)
    for (lid, head, arm), r in latest(reads).items():
        if lid in labels:
            cells[(head, arm)].append(r)
    heads: dict = defaultdict(dict)
    for (head, arm), cell in sorted(cells.items()):
        heads[head][arm] = summarise(labels, cell)
    pairs = {}
    for head, h in heads.items():
        pf = {a: h[a].pop("_per_field") for a in list(h)}
        pr = {}
        for a, b in (
            ("P1280", "P800"),
            ("T1280", "T800"),
            ("T800", "P800"),
            ("T1280", "P1280"),
            ("T800", "D800"),
            ("T1280", "D1280"),
        ):
            if a in pf and b in pf:
                pr[f"{a}_vs_{b}"] = discordant(pf[a], pf[b])
        pairs[head] = pr
    decision = decide(heads, len(labels))
    tusd = sum(t["usd"] for rows in transcripts.values() for t in rows)
    spend = round(sum(r["usd"] for r in reads) + tusd, 4)
    doc = {
        "lane": "RB-M1",
        "generated": date.today().isoformat(),
        "method": "tests/evals/label_read/README.md",
        "labels": len(labels),
        "scored_fields": sum(len(c) for lab in labels.values() for c in lab["columns"].values()),
        "transcriber": {
            "model": "claude-haiku-5-5",
            "effort": "low",
            "px": 1280,
            "T_full": transcript_stats(transcripts["T"]),
            "D_device_cap": transcript_stats(transcripts["D"]),
        },
        "decision": decision,
        "heads": heads,
        "paired_fields": pairs,
        "lane_spend_usd": spend,
    }
    Path(args.out).write_text(json.dumps(doc, indent=1) + "\n")
    Path(args.md).write_text(markdown(doc))
    print(markdown(doc))
    return 0


def markdown(doc: dict) -> str:
    d = doc["decision"]
    lines = [
        f"# Label-read accuracy (RB-M1, {doc['generated']})",
        "",
        f"{doc['labels']} real label photos, {doc['scored_fields']} scored figures. Field accuracy is figures read exactly right"
        " over figures printed (one-sided 95% lower bound in brackets).",
        "",
        "| Head | Arm | Field accuracy | Labels all right | kJ/kcal confusions | Protein misreads | Extra figures | Failed | USD/read | Prompt tokens |",
        "|---|---|---|---|---|---|---|---|---|---|",
    ]
    for head, h in doc["heads"].items():
        for arm in ARMS:
            if arm not in h:
                continue
            c = h[arm]
            fa = c["field_accuracy"]
            lines.append(
                f"| {head} | {arm} | {fa['k']}/{fa['n']} = {100 * fa['rate']:.1f}% (>= {100 * fa['lower95']:.1f}%) |"
                f" {c['labels_all_correct']['k']}/{c['labels_all_correct']['n']} | {c['kj_kcal_confusions']} |"
                f" {c['protein_misreads']['k']}/{c['protein_misreads']['n']} | {c['extra_figures']} | {c['failed_reads']} |"
                f" {c['usd_per_read']:.4f} | {c['prompt_tokens_mean']} |"
            )
    lines += [
        "",
        "P = pixels only; T = pixels plus the intake transcription (claude-haiku-5-5, effort low, 1,280 px, room to finish);"
        " D = pixels plus the transcription as the device makes it today (900 output tokens). The number is the long edge sent.",
        "",
        f"Device-capped transcriptions cut at max_tokens: {doc['transcriber']['D_device_cap']['stopped_at_max_tokens']}"
        f" of {doc['transcriber']['D_device_cap']['count']}; full ones: {doc['transcriber']['T_full']['stopped_at_max_tokens']}"
        f" of {doc['transcriber']['T_full']['count']}.",
        "",
        "## Decisions",
        "",
        f"- Rendition: **{d['rendition_px']} px**. Rule: {d['rendition_rule']}. Gain of 1,280 over 800: "
        + ", ".join(f"{h} {100 * g:+.1f} points" for h, g in d["rendition_gain_1280_minus_800"].items())
        + ".",
        f"- Transcription: **{'kept' if d['keep_transcription'] else 'dropped'}**. Rule: {d['transcription_rule']}.",
    ]
    for head, t in d["transcription_per_head"].items():
        lines.append(
            f"  - {head}: alone {100 * t['pixels_only']:.1f}%, with transcription {100 * t['with_transcription']:.1f}%:"
            f" {'keep' if t['keep_transcription'] else 'drop'}"
        )
    lines += ["", "## Paired fields (right in the first arm only / right in the second only)", ""]
    for head, pr in doc["paired_fields"].items():
        lines.append(f"- {head}: " + "; ".join(f"{k} {v['a_only']}/{v['b_only']}" for k, v in pr.items()))
    lines += ["", f"Lane spend in this evidence: {doc['lane_spend_usd']:.2f} USD.", ""]
    return "\n".join(lines)


if __name__ == "__main__":
    sys.exit(main())
