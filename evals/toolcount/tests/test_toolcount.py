"""Host tests for the tool-count benchmark. No network, no keys, no hardware.

Run: python3 -m pytest evals/toolcount/tests
"""

from __future__ import annotations

import json
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))

import conditions as C  # noqa: E402
import providers as P  # noqa: E402
import run_toolcount as R  # noqa: E402
import score as S  # noqa: E402

CATALOG = C.load_catalog(ROOT / "catalog.json")
TASKS = json.loads((ROOT / "tasks.json").read_text())["tasks"]
CORE = {t["name"] for t in CATALOG if t.get("core")}
ALWAYS = {t["name"] for t in CATALOG if t.get("always")}
NAMES = {t["name"] for t in CATALOG}


# --- catalog / task integrity (guards the benchmark against silent drift) ---


def test_catalog_has_unique_names():
    names = [t["name"] for t in CATALOG]
    assert len(names) == len(set(names))


def test_curated_is_in_target_band():
    # the owner's stated stable band is ~15-20 tools
    assert 15 <= len(CORE) <= 20


def test_always_base_is_small():
    assert 1 <= len(ALWAYS) <= 6
    assert ALWAYS <= CORE  # every always-on tool is also curated


def test_every_expected_tool_exists():
    for t in TASKS:
        if t["expected_tool"] is not None:
            assert t["expected_tool"] in NAMES, t["id"]


def test_subset_invariants():
    for t in TASKS:
        exp = t["expected_tool"]
        if t["subset"] == "core":
            assert exp in CORE, f"{t['id']} core but {exp} not curated"
        elif t["subset"] == "coverage":
            assert exp not in CORE, f"{t['id']} coverage but {exp} curated"
        elif t["subset"] == "none":
            assert exp is None, t["id"]


# --- conditions ---


def test_exposed_counts():
    assert len(C.exposed_tools("full", CATALOG)) == len(CATALOG)
    assert len(C.exposed_tools("curated", CATALOG)) == len(CORE)
    lazy = C.exposed_tools("lazy", CATALOG)
    assert any(t["function"]["name"] == "search_tools" for t in lazy)
    assert len(lazy) == len(ALWAYS) + 1


def test_schema_shape():
    sch = C.to_schema(CATALOG[0])
    assert sch["type"] == "function"
    assert "name" in sch["function"] and "parameters" in sch["function"]


def test_lazy_retrieval_finds_non_always_tools():
    for t in TASKS:
        exp = t["expected_tool"]
        if exp is None or exp in ALWAYS:
            continue
        hits = {h["name"] for h in C.search_catalog(t["prompt"], CATALOG, k=5)}
        assert exp in hits, f"lazy search cannot reach {exp} for {t['id']}"


# --- mock end-to-end + scoring ---


def _run_mock(reps=1):
    prov = P.build_provider("mock:mock")
    rows = []
    for cond in ("full", "curated", "lazy"):
        for task in TASKS:
            for rep in range(reps):
                row = R._run_one_turn(prov, task, cond, CATALOG, mock=True)
                row["rep"] = rep
                row["subset"] = task["subset"]
                row["judge"] = None
                rows.append(row)
    return rows


def test_mock_full_selects_expected():
    prov = P.build_provider("mock:mock")
    for task in TASKS:
        row = R._run_one_turn(prov, task, "full", CATALOG, mock=True)
        assert row["selected_correct"], task["id"]


def test_mock_lazy_recovers_coverage_via_search():
    prov = P.build_provider("mock:mock")
    cov = [t for t in TASKS if t["subset"] == "coverage"][0]
    row = R._run_one_turn(prov, cov, "lazy", CATALOG, mock=True)
    assert row["search_hops"] >= 1
    assert row["selected_correct"]


def test_curated_cannot_answer_coverage():
    prov = P.build_provider("mock:mock")
    cov = [t for t in TASKS if t["subset"] == "coverage"][0]
    row = R._run_one_turn(prov, cov, "curated", CATALOG, mock=True)
    assert not row["selected_correct"]  # tool was cut from curated


def test_summary_shape_and_metrics():
    rows = _run_mock(reps=2)
    summ = S.summarize(rows, run="unit", date="2026-08-30")
    assert summ["schema_version"] == S.SCHEMA_VERSION
    assert set(summ["conditions"]) == {"full", "curated", "lazy"}
    key = "mock/mock|full"
    cell = summ["cells"][key]["overall"]
    assert cell["n"] > 0
    assert 0.0 <= cell["selection_accuracy"] <= 1.0
    # full condition with the deterministic mock is perfect selection
    assert summ["cells"]["mock/mock|full"]["subset:core"]["selection_accuracy"] == 1.0


def test_usd_estimate_monotonic():
    a = P.usd_estimate("gpt-4o-mini", 1000, 100)
    b = P.usd_estimate("gpt-4o-mini", 2000, 200)
    assert b > a >= 0
