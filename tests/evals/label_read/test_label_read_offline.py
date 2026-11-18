#!/usr/bin/env python3
"""Free self-test of the label-read eval: no keys, no network.

python3 tests/evals/label_read/test_label_read_offline.py
"""

from __future__ import annotations

import copy
import io
import json
import sys
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
sys.path.insert(0, str(HERE.parent / "behaviour"))

import label_report as report  # noqa: E402
import read_labels as run  # noqa: E402
from score import FIELDS, norm, same, score  # noqa: E402

MANIFEST = json.loads((HERE / "labels.json").read_text())
LABELS = MANIFEST["labels"]


def perfect(truth: dict) -> dict:
    """A read that copies every printed figure of every column."""
    return {
        "columns": [
            {"basis_kind": b, "basis_text": b, **{f: c.get(f) for f in FIELDS}} for b, c in truth["columns"].items()
        ]
    }


class Norm(unittest.TestCase):
    def test_decimal_comma_units_and_spacing(self):
        for a, b in (("30,4 g", "30.4"), ("2104 kJ", "2104"), ("0,107", "0.107"), ("26.0", "26"), ("(0.6g)", "0.6")):
            self.assertTrue(same(a, b), (a, b))

    def test_a_copied_ri_share_is_not_part_of_the_figure(self):
        self.assertTrue(same("8,6 (12%)", "8.6"))
        self.assertTrue(same("0 (0%)", "0"))
        self.assertFalse(same("8,6 (12%)", "12"))

    def test_less_than_is_part_of_the_figure(self):
        self.assertTrue(same("<0,5 g", "<0.5"))
        self.assertFalse(same("0.5", "<0.5"))
        self.assertFalse(same("<0.5", "0.5"))

    def test_absent_is_never_equal(self):
        for v in (None, "", "-", "null"):
            self.assertIsNone(norm(v))
            self.assertFalse(same(v, None))

    def test_different_digits_differ(self):
        self.assertFalse(same("10.2", "10.3"))
        self.assertFalse(same("1501", "1510"))


class Score(unittest.TestCase):
    def test_a_perfect_read_scores_every_field_on_every_label(self):
        for lab in LABELS:
            s = score(lab, perfect(lab))
            self.assertTrue(all(f["ok"] for f in s["fields"]), lab["id"])
            self.assertEqual((s["kj_kcal_confusions"], s["protein_misreads"], s["extras"]), (0, 0, 0), lab["id"])

    def test_counter_a_changed_figure_is_caught_on_every_label(self):
        for lab in LABELS:
            read = perfect(lab)
            col = read["columns"][0]
            f = next(f for f in FIELDS if col.get(f) is not None)
            col[f] = col[f] + "1"
            s = score(lab, read)
            self.assertEqual(sum(not x["ok"] for x in s["fields"]), 1, lab["id"])

    def test_kj_read_as_kcal_and_swapped_pair(self):
        lab = next(x for x in LABELS if "energy_kj" in x["columns"]["per100"])
        t = lab["columns"]["per100"]
        for kj, kc in ((t["energy_kj"], t["energy_kj"]), (t["energy_kcal"], t["energy_kj"])):
            read = perfect(lab)
            read["columns"][0]["energy_kj"], read["columns"][0]["energy_kcal"] = kj, kc
            self.assertEqual(score(lab, read)["kj_kcal_confusions"], 1)

    def test_protein_misread_counted(self):
        lab = next(x for x in LABELS if "protein" in x["columns"]["per100"])
        read = perfect(lab)
        read["columns"][0]["protein"] = "99"
        self.assertEqual(score(lab, read)["protein_misreads"], 1)

    def test_missing_column_and_failed_read_are_wrong_not_skipped(self):
        lab = next(x for x in LABELS if "serving" in x["columns"])
        read = perfect(lab)
        read["columns"] = [c for c in read["columns"] if c["basis_kind"] != "serving"]
        s = score(lab, read)
        self.assertEqual(sum(not f["ok"] for f in s["fields"]), len(lab["columns"]["serving"]))
        self.assertFalse(any(f["ok"] for f in score(lab, None)["fields"]))

    def test_extras_count_unprinted_rows_but_not_sodium_or_unscored(self):
        lab = next(x for x in LABELS if "polyols" not in x["columns"]["per100"] and not x["unscored"])
        read = perfect(lab)
        read["columns"][0]["polyols"] = "8.9"
        read["columns"][0]["sodium_mg"] = "2"
        self.assertEqual(score(lab, read)["extras"], 1)
        blind = next(x for x in LABELS if x["unscored"])
        basis, field = blind["unscored"][0].split(".")
        read = perfect(blind)
        next(c for c in read["columns"] if c["basis_kind"] == basis)[field] = "123"
        self.assertEqual(score(blind, read)["extras"], 0)

    def test_an_invented_column_counts_as_extras(self):
        lab = next(x for x in LABELS if "serving" not in x["columns"])
        read = perfect(lab)
        read["columns"].append({"basis_kind": "serving", "fat": "1.0", "protein": "2.0", "sodium_mg": "5"})
        self.assertEqual(score(lab, read)["extras"], 2)

    def test_malformed_columns_do_not_crash(self):
        s = score(LABELS[0], {"columns": ["junk", 3, {"basis_kind": "nope"}]})
        self.assertFalse(any(f["ok"] for f in s["fields"]))

    def test_first_column_of_a_basis_wins(self):
        lab = LABELS[0]
        read = perfect(lab)
        wrong = copy.deepcopy(read["columns"][0])
        wrong["fat"] = "999"
        read["columns"].append(wrong)
        self.assertTrue(all(f["ok"] for f in score(lab, read)["fields"]))


class Manifest(unittest.TestCase):
    def test_at_least_30_labels_with_the_coverage_the_brief_asks_for(self):
        self.assertGreaterEqual(len(LABELS), 30)
        tags = [t for lab in LABELS for t in lab["tags"]]
        for need in ("kj_kcal", "per100", "serving", "polyols", "fibre"):
            self.assertIn(need, tags)
        self.assertTrue(any(lab["languages"] != ["en"] for lab in LABELS))
        self.assertTrue(any(t in tags for t in ("glare", "angle", "curved")))

    def test_every_label_is_attributed_and_its_photo_exists(self):
        ids = [lab["id"] for lab in LABELS]
        self.assertEqual(len(ids), len(set(ids)))
        for lab in LABELS:
            self.assertTrue(lab["product_url"].startswith("https://world.openfoodfacts.org/product/"), lab["id"])
            self.assertTrue(lab["image_url"].startswith("https://images.openfoodfacts.org/"), lab["id"])
            self.assertEqual(lab["license"], "CC BY-SA 3.0")
            self.assertIn(lab["product_url"], lab["attribution"])
            self.assertTrue((HERE / "photos" / lab["file"]).is_file(), lab["file"])

    def test_every_truth_figure_is_a_number_as_printed(self):
        for lab in LABELS:
            self.assertTrue(set(lab["columns"]) <= {"per100", "serving"}, lab["id"])
            for b, col in lab["columns"].items():
                self.assertTrue(set(col) <= set(FIELDS), lab["id"])
                for f, v in col.items():
                    n = norm(v)
                    self.assertIsNotNone(n, (lab["id"], b, f))
                    float(n.lstrip("<"))
            for u in lab["unscored"]:
                b, f = u.split(".")
                self.assertIn(b, ("per100", "serving"))
                self.assertNotIn(f, lab["columns"].get(b, {}))

    def test_photo_set_stays_small_and_large_enough_for_both_renditions(self):
        total = sum((HERE / "photos" / lab["file"]).stat().st_size for lab in LABELS)
        self.assertLess(total, 15 * 1024 * 1024)
        on_disk = {p.name for p in (HERE / "photos").iterdir()}
        self.assertEqual(on_disk, {lab["file"] for lab in LABELS}, "a photo without a manifest row or the reverse")
        for lab in LABELS:
            self.assertGreaterEqual(max(lab["stored_px"]), 1280, lab["id"])
            self.assertLessEqual(max(lab["stored_px"]), 1600, lab["id"])


class Wire(unittest.TestCase):
    def test_schema_asks_for_every_scored_field(self):
        self.assertEqual(tuple(run.FIELDS), tuple(FIELDS))
        item = run.SCHEMA["properties"]["columns"]["items"]
        self.assertEqual(set(item["required"]), {"basis_kind", "basis_text", *FIELDS})
        self.assertFalse(item["additionalProperties"])

    def test_rendition_long_edge(self):
        try:
            from PIL import Image
        except ImportError:
            self.skipTest("Pillow not installed")
        photo = HERE / "photos" / LABELS[0]["file"]
        for px in (800, 1280):
            im = Image.open(io.BytesIO(run.rendition(photo, px)))
            self.assertEqual(max(im.size), px)
            self.assertEqual(im.format, "JPEG")

    def test_photo_line_matches_the_device_labels(self):
        self.assertEqual(
            run.photo_line("0123abcd", None), "Photo 1 of 1 [p0123abcd] (attached: the photo itself is in this message)"
        )
        self.assertEqual(
            run.photo_line("0123abcd", "Energie 2104 kJ"),
            "Photo 1 of 1 [p0123abcd] (transcribed by claude-haiku-5-5) Energie 2104 kJ",
        )

    def test_transcription_is_capped_on_a_character_boundary(self):
        long = "é" * 3000
        out = run.capped(long)
        self.assertTrue(out.endswith(run.CUT_MARK))
        self.assertLessEqual(len(out[: -len(run.CUT_MARK)].encode()), run.MEDIA_TEXT_MAX)
        self.assertEqual(run.capped("short"), "short")

    def test_arm_names(self):
        self.assertEqual(run.arm_px("T1280"), ("T", 1280))
        self.assertEqual(run.arm_px("D800"), ("D", 800))
        self.assertEqual(run.arm_px("P800"), ("P", 800))
        with self.assertRaises(SystemExit):
            run.arm_px("X800")


def cell(k: int, n: int = 1000) -> dict:
    return {"labels": 40, "field_accuracy": {"k": k, "n": n, "rate": k / n}}


def heads(s: dict, h: dict) -> dict:
    return {
        "claude-sonnet-5-5": {a: cell(v) for a, v in s.items()},
        "claude-haiku-5-5": {a: cell(v) for a, v in h.items()},
    }


class Pipeline(unittest.TestCase):
    """read_labels' plan and resume, and label_report end to end over a fixture evidence dir."""

    def test_plan_covers_every_label_and_arm_or_both_transcriptions(self):
        labs = LABELS[:3]
        self.assertEqual(len(run.plan(labs, ["P800", "T1280"], False)), 6)
        self.assertEqual({j for _, j in run.plan(labs, ["P800"], True)}, {"T", "D"})

    def test_a_cut_line_is_skipped(self):
        import tempfile

        with tempfile.TemporaryDirectory() as d:
            f = Path(d) / "reads.jsonl"
            f.write_text('{"a": 1}\n{"a": 2, "cut')
            self.assertEqual(run.load_jsonl(f), [{"a": 1}])
            self.assertEqual(report.jsonl(f), [{"a": 1}])

    def test_report_end_to_end(self):
        import tempfile

        with tempfile.TemporaryDirectory() as d:
            ev = Path(d)
            for head in report.DECIDING_HEADS:
                (ev / head).mkdir()
                rows = []
                for lab in LABELS:
                    for arm in report.DECISION_ARMS:
                        read = perfect(lab)
                        if arm == "P800" and head == "claude-haiku-5-5":
                            read["columns"][0]["protein"] = "999"  # one misread per label at 800 px
                        rows.append(
                            {
                                "id": lab["id"],
                                "head": head,
                                "arm": arm,
                                "ok": True,
                                "read": read,
                                "usd": 0.001,
                                "usage": {"prompt": 100},
                                "jpeg_bytes": 1,
                                "seconds": 1.0,
                            }
                        )
                (ev / head / "reads.jsonl").write_text("".join(json.dumps(r) + "\n" for r in rows))
            out, md = ev / "labels.json", ev / "RESULTS.md"
            argv = sys.argv
            sys.argv = ["label_report.py", "--evidence", str(ev), "--out", str(out), "--md", str(md)]
            try:
                import contextlib

                with contextlib.redirect_stdout(io.StringIO()):
                    report.main()
            finally:
                sys.argv = argv
            doc = json.loads(out.read_text())
            h = doc["heads"]["claude-haiku-5-5"]
            n = doc["scored_fields"]
            self.assertEqual(h["P1280"]["field_accuracy"]["k"], n)
            self.assertEqual(
                h["P800"]["protein_misreads"]["k"], sum(1 for x in LABELS if "protein" in x["columns"]["per100"])
            )
            self.assertEqual(doc["decision"]["rendition_px"], 1280)  # Haiku gains over a point at 1,280
            self.assertEqual(doc["decision"]["keep_transcription_for"], [])  # perfect alone at 1,280
            self.assertIn("| claude-haiku-5-5 | P800 |", md.read_text())


class Decide(unittest.TestCase):
    SAME = {"P800": 960, "P1280": 960, "T800": 960, "T1280": 960}

    def test_800_unless_1280_pixels_alone_gain_a_point_on_a_claude_head(self):
        self.assertEqual(report.decide(heads(self.SAME, self.SAME), 40)["rendition_px"], 800)
        small = dict(self.SAME, P1280=965)  # +0.5 points
        self.assertEqual(report.decide(heads(small, self.SAME), 40)["rendition_px"], 800)
        big = dict(self.SAME, P1280=975)  # +1.5 points on Haiku only
        self.assertEqual(report.decide(heads(self.SAME, big), 40)["rendition_px"], 1280)
        t_only = dict(self.SAME, T1280=990)  # the T arms do not move the rendition
        self.assertEqual(report.decide(heads(t_only, t_only), 40)["rendition_px"], 800)

    def test_transcription_kept_unless_alone_is_as_good_and_at_95(self):
        d = report.decide(heads(self.SAME, self.SAME), 40)
        self.assertEqual(d["keep_transcription_for"], [])  # 96% alone, equal with transcription: dropped
        better = dict(self.SAME, T800=970)
        d = report.decide(heads(self.SAME, better), 40)
        self.assertEqual(d["keep_transcription_for"], ["claude-haiku-5-5"])  # helps Haiku only
        low = {a: 940 for a in self.SAME}
        self.assertEqual(len(report.decide(heads(low, low), 40)["keep_transcription_for"]), 2)  # under 95%

    def test_a_missing_or_partial_arm_refuses_to_decide(self):
        partial = {"P800": 960, "P1280": 960, "T800": 960}
        with self.assertRaises(SystemExit):
            report.decide(heads(self.SAME, partial), 40)
        with self.assertRaises(SystemExit):
            report.decide(heads(self.SAME, self.SAME), 41)  # one label unread in every arm


if __name__ == "__main__":
    unittest.main(verbosity=1)
