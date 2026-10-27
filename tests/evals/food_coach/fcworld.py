"""The scenarios and the world each run starts from.

A scenario file (scenarios/*.json) names a food-log set, the skills the device
has, the earlier chat turns and the owner messages to send. This module turns it
into the JSON the C++ driver reads, and into the facts the judge is told are true.

The food log is the device's daily ledger (one canonical file, one row per eating
occasion: date,meal,items,kcal,protein_g). A scenario may also give its ledger rows
directly (`ledger_rows`), standing rules as the owner's device holds them (`prefs`)
and a conversation summary from an earlier fold (`summary`).
"""

from __future__ import annotations

import datetime as dt
import json
from pathlib import Path

HERE = Path(__file__).resolve().parent
LOG_PATH = "food/food-log.csv"
REQUIRED_KEYS = ("id", "title", "kind", "seen", "world", "turns", "facts", "criteria")


def load_json(path: Path) -> dict:
    return json.loads(Path(path).read_text(encoding="utf-8"))


def load_config() -> dict:
    return load_json(HERE / "config.json")


def load_scenarios(ids: list[str] | None = None) -> list[dict]:
    """Every scenario, in file-name order; `ids` narrows (an unknown id is an error)."""
    found = [load_json(p) for p in sorted((HERE / "scenarios").glob("*.json"))]
    if not ids:
        return found
    by_id = {s["id"]: s for s in found}
    missing = [i for i in ids if i not in by_id]
    if missing:
        raise KeyError(f"unknown scenario id(s): {', '.join(missing)}")
    return [by_id[i] for i in ids]


def log_sets() -> dict:
    return load_json(HERE / "profiles" / "log_sets.json")


def totals(rows: list[list]) -> tuple[float, float]:
    """(kcal, protein_g) of a food-log set; row = meal, item, qty, unit, kcal, protein."""
    return sum(r[4] for r in rows), sum(r[5] for r in rows)


def ledger_rows(rows: list[list]) -> list[list]:
    """A food-log set as ledger rows (meal, items, kcal, protein): one per meal, in order."""
    meals: dict = {}
    for meal, item, qty, unit, kcal, protein in rows:
        m = meals.setdefault(meal, [meal, [], 0.0, 0.0])
        m[1].append(f"{item} {_num(qty)} {unit}")
        m[2] += kcal
        m[3] += protein
    return [[m, " + ".join(items), k, p] for m, items, k, p in meals.values()]


def build_log_csv(rows: list[list], today: dt.date, columns: str) -> str:
    """The ledger text the device holds for `today`; `rows` are ledger rows."""
    out = [columns]
    for meal, items, kcal, protein in rows:
        out.append(f"{today.isoformat()},{meal},{items},{_num(kcal)},{_num(protein)}")
    return "\n".join(out) + "\n"


def world_rows(scn: dict) -> list[list]:
    """The scenario's ledger rows: given directly, or from its food-log set."""
    if "ledger_rows" in scn["world"]:
        return scn["world"]["ledger_rows"]
    return ledger_rows(log_sets()["sets"][scn["world"]["log_set"]]["rows"])


def _num(x: float) -> str:
    return str(int(x)) if float(x).is_integer() else f"{x:.1f}"


def fact_values(scn: dict, cfg: dict) -> dict:
    """The numbers the scenario's facts may mention."""
    rows = world_rows(scn)
    kcal, protein = sum(r[2] for r in rows), sum(r[3] for r in rows)
    t = cfg["targets"]
    remaining = t["kcal"] - kcal
    return {
        "target_kcal": _num(t["kcal"]),
        "target_protein": _num(t["protein_g"]),
        "logged_kcal": _num(kcal),
        "logged_protein": _num(protein),
        "remaining_kcal": _num(remaining),
        "remaining_protein": _num(t["protein_g"] - protein),
        "lo_kcal": _num(round(remaining * 0.88)),
        "hi_kcal": _num(round(remaining * 1.04)),
    }


def facts_for(scn: dict, cfg: dict) -> list[str]:
    values = fact_values(scn, cfg)
    return [f.format(**values) for f in scn["facts"]]


def _criteria_problems(criteria: list[dict]) -> list[str]:
    ids = [c.get("id") for c in criteria]
    problems = []
    if len(set(ids)) != len(ids) or not all(ids):
        problems.append("criterion ids must be unique and non-empty")
    if not any(c.get("required") for c in criteria):
        problems.append("at least one criterion must be required")
    return problems


def _shape_problems(scn: dict) -> list[str]:
    problems = []
    if scn["kind"] not in ("defect", "guard"):
        problems.append("kind must be defect or guard")
    if "ledger_rows" not in scn["world"] and scn["world"].get("log_set") not in log_sets()["sets"]:
        problems.append("unknown log_set")
    if not scn["turns"] or not all(isinstance(t, str) and t.strip() for t in scn["turns"]):
        problems.append("turns must be non-empty strings")
    return problems


def validate_scenario(scn: dict, cfg: dict) -> list[str]:
    """Problems with a scenario file; an empty list means it is usable."""
    missing = [f"missing key {k}" for k in REQUIRED_KEYS if k not in scn]
    if missing:
        return missing
    problems = _shape_problems(scn) + _criteria_problems(scn["criteria"])
    try:
        facts_for(scn, cfg)
    except (KeyError, IndexError, ValueError) as exc:
        problems.append(f"facts do not format: {exc}")
    return problems + _history_problems(scn["world"].get("history", []))


def _history_problems(history: list[dict]) -> list[str]:
    roles = [h.get("role") for h in history]
    if any(r not in ("user", "assistant") for r in roles):
        return ["history role must be user or assistant"]
    if roles and roles[0] != "user":
        return ["history must start with a user turn"]
    if any(a == b for a, b in zip(roles, roles[1:])):
        return ["history must alternate user and assistant"]
    return []


def memory_seed(cfg: dict) -> list[str]:
    t = cfg["targets"]
    project, name = LOG_PATH.split("/", 1)
    return [
        f"The owner's daily food targets are {t['kcal']} kcal and a protein floor of "
        f"{t['protein_g']} g. The food log is the file {name} (project {project})."
    ]


def skill_seed(ids: list[str]) -> list[dict]:
    return [{"id": i, "md": (HERE / "profiles" / f"{i}.md").read_text(encoding="utf-8")} for i in ids]


def run_payload(scn: dict, cfg: dict, today: dt.date) -> dict:
    """What the C++ driver is given for one run of `scn`."""
    directive = (HERE / "profiles" / "stored_directive.txt").read_text(encoding="utf-8").strip()
    world = scn["world"]
    return {
        "profile": {"directive": directive, "clock": True},
        "seed": {
            "files": [{"path": LOG_PATH, "text": build_log_csv(world_rows(scn), today, log_sets()["columns"])}],
            "skills": skill_seed(world.get("skills", [])),
            "memory": memory_seed(cfg),
            "history": world.get("history", []),
            "prefs": world.get("prefs", []),
            "summary": world.get("summary", ""),
        },
        "turns": scn["turns"],
    }
