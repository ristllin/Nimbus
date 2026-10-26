"""The scenarios and the world each run starts from.

A scenario file (scenarios/*.json) names a food-log set, the skills the device
has, the earlier chat turns and the owner messages to send. This module turns it
into the JSON the C++ driver reads, and into the facts the judge is told are true.
"""

from __future__ import annotations

import datetime as dt
import json
from pathlib import Path

HERE = Path(__file__).resolve().parent
LOG_PATH = "nutrition/food-log.csv"
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


def build_log_csv(rows: list[list], today: dt.date, columns: str) -> str:
    """The CSV text the device's food log holds for `today`."""
    out = [columns]
    for n, (meal, item, qty, unit, kcal, protein) in enumerate(rows, 1):
        meal_id = f"{today:%Y%m%d}-{meal}-{n:02d}"
        out.append(f"{today.isoformat()},,{meal_id},{meal},{item},{qty},{unit},{kcal},{protein},estimate,medium,")
    return "\n".join(out) + "\n"


def _num(x: float) -> str:
    return str(int(x)) if float(x).is_integer() else f"{x:.1f}"


def fact_values(scn: dict, cfg: dict) -> dict:
    """The numbers the scenario's facts may mention."""
    sets = log_sets()["sets"]
    rows = sets[scn["world"]["log_set"]]["rows"]
    kcal, protein = totals(rows)
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
    if scn["world"].get("log_set") not in log_sets()["sets"]:
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
    return [
        f"The owner's daily food targets are {t['kcal']} kcal and a protein floor of "
        f"{t['protein_g']} g. The food log is the file {LOG_PATH} (project nutrition)."
    ]


def skill_seed(ids: list[str]) -> list[dict]:
    return [{"id": i, "md": (HERE / "profiles" / f"{i}.md").read_text(encoding="utf-8")} for i in ids]


def run_payload(scn: dict, cfg: dict, today: dt.date) -> dict:
    """What the C++ driver is given for one run of `scn`."""
    sets = log_sets()
    rows = sets["sets"][scn["world"]["log_set"]]["rows"]
    directive = (HERE / "profiles" / "stored_directive.txt").read_text(encoding="utf-8").strip()
    return {
        "profile": {"directive": directive, "clock": True},
        "seed": {
            "files": [{"path": LOG_PATH, "text": build_log_csv(rows, today, sets["columns"])}],
            "skills": skill_seed(scn["world"].get("skills", [])),
            "memory": memory_seed(cfg),
            "history": scn["world"].get("history", []),
        },
        "turns": scn["turns"],
    }
