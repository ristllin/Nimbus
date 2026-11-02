#!/usr/bin/env python3
"""Lane-artifact honesty checks for lane PERSONA.

The coverage doc (coverage/PERSONA-2026-11.md at the program root) must not claim a
persona class that never ran, and its round-history counts must match the raw evidence
under lanes/PERSONA/evidence/. Both are checked here so a wrong claim fails a test
instead of reaching the orchestrator.

These tests only run where the lane layout exists (this tree at lanes/<LANE>/nimbus,
with ../evidence and ../../../coverage/PERSONA-2026-11.md). On a clean checkout they
skip, so CI is unaffected.
"""

import json
import re
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
TREE = HERE.parents[1]  # .../lanes/PERSONA/nimbus
LANE = TREE.parent  # .../lanes/PERSONA
PROGRAM = TREE.parents[2]  # .../program-2026-10-18
EVIDENCE = LANE / "evidence"
COVERAGE_DOC = PROGRAM / "coverage" / "PERSONA-2026-11.md"

# The TASK.md minimum persona list: (class label, keyword). A class is covered when the
# keyword appears in a runnable pa-*.json persona file name or description; otherwise
# the coverage doc must mark the class NOT RUN on a line naming the keyword.
TASK_CLASSES = [
    ("first-time owner with one key", "first-time"),
    ("Cumulo-key-only client with a small wallet", "cumulo"),
    ("BYOK on each provider alone", "byok"),
    ("food coach daily user", "food"),
    ("commuter with routines and briefings", "commuter"),
    ("researcher with web and sub-agents", "researcher"),
    ("family with owner and two members", "family"),
    ("privacy-minded user", "privacy"),
    ("Telegram-first user", "telegram"),
    ("user hitting a spend ceiling", "ceiling"),
    ("provider going down mid-conversation", "provider-down"),
    ("long conversation forcing compaction", "long-conv"),
    ("non-English speaker", "non-english"),
    ("someone confused and rude", "rude"),
    ("onboarding", "onboarding"),
    ("recovery after a restart", "restart"),
    ("owner signal files as a class", "owner-signals"),
]

ROUND_LINE = re.compile(r"^- Round (\d+): (\d+)(?: failing cells \((\d+) failing scenarios\))?", re.M)


def _pa_persona_text() -> str:
    parts = []
    for f in sorted((HERE / "personas").glob("pa-*.json")):
        parts.append(f.name.lower())
        try:
            parts.append(json.loads(f.read_text()).get("description", "").lower())
        except json.JSONDecodeError:
            pass
    return " ".join(parts)


def _evidence_rounds() -> dict:
    """round number -> (failing cells, failing scenarios), recomputed from summary.json."""
    out = {}
    if not EVIDENCE.is_dir():
        return out
    for d in sorted(EVIDENCE.iterdir()):
        m = re.fullmatch(r"round-(\d+)", d.name)
        if not m or not d.is_dir():
            continue
        fail_cells = fail_scen = 0
        for s in sorted(d.glob("pa-*/summary.json")):
            j = json.loads(s.read_text())
            if j.get("pass") is False:
                fail_cells += 1
            fail_scen += sum(1 for sc in j.get("scenarios", []) if sc.get("pass") is False)
        out[int(m.group(1))] = (fail_cells, fail_scen)
    return out


@unittest.skipUnless(COVERAGE_DOC.is_file(), "lane coverage doc not present outside the lane layout")
class CoverageDocClaims(unittest.TestCase):
    def test_every_task_minimum_class_is_covered_or_marked_not_run(self):
        doc = COVERAGE_DOC.read_text()
        pa = _pa_persona_text()
        for label, keyword in TASK_CLASSES:
            if keyword in pa:
                continue
            hits = [ln for ln in doc.splitlines() if "not run" in ln.lower() and keyword in ln.lower()]
            self.assertTrue(
                hits,
                f"TASK class {label!r} has no pa-*.json persona (keyword {keyword!r}) "
                "and the coverage doc has no NOT RUN line naming it",
            )

    def test_round_history_counts_match_the_raw_evidence(self):
        doc = COVERAGE_DOC.read_text()
        stated = {}
        for m in ROUND_LINE.finditer(doc):
            stated[int(m.group(1))] = (int(m.group(2)), m.group(3) and int(m.group(3)))
        self.assertTrue(stated, "no round-history lines found in the coverage doc")
        evidence = _evidence_rounds()
        self.assertEqual(sorted(stated), sorted(evidence), "doc rounds and evidence rounds differ")
        for r, (cells, scen) in sorted(evidence.items()):
            doc_cells, doc_scen = stated[r]
            self.assertEqual(
                doc_cells,
                cells,
                f"round-{r}: doc says {doc_cells} failing cells, evidence says {cells}",
            )
            if doc_scen is not None:
                self.assertEqual(
                    doc_scen,
                    scen,
                    f"round-{r}: doc says {doc_scen} failing scenarios, evidence says {scen}",
                )


if __name__ == "__main__":
    unittest.main()
