"""persona_schema - load persona files and refuse anything that would mislead.

A persona runs on real money and its verdict is trusted, so a mistyped field must stop
the run, not fall back to a default and pass. Every persona, scenario and step field is in
a table here. An unknown field or a wrong type is refused when the file loads.
"""

from __future__ import annotations

import copy
import datetime
import difflib
import hashlib
import json
import re
import urllib.request
from pathlib import Path

from grade import ASSERTIONS, check_arg


class PersonaError(ValueError):
    """A persona file that cannot run. Refused at load, before anything is spent."""


PLACEHOLDER = re.compile(r"\$\{([a-z_][a-z0-9_]*)\}")
SAFE_NAME = r"[A-Za-z0-9][A-Za-z0-9_.-]{0,62}"
FILLER_WORDS = "harbor lantern quartz meadow copper violet ember thistle marble willow".split()
STEP_KINDS = ("say", "http", "upload", "restart", "sleep")
# The form field a provider's key is posted in.
KEY_FIELDS = {"openai": "oaiKey", "anthropic": "antKey", "mistral": "mistKey", "zai": "zaiKey", "cumulo": "cumuloKey"}
# What a persona may ask of a key: the verdict /api/orch must report before any turn runs.
KEY_VERDICTS = ("verified", "rejected", "nocredits")
NUMBER = (int, float)

PERSONA_FIELDS = {
    "name": str,
    "description": str,
    "display_name": str,
    "timezone": str,
    "directive": str,
    "monthly_cap_cents": int,
    "keys": dict,
    "key_expect": dict,
    "instance_env_keys": list,
    "env": dict,
    "models": dict,
    "host": str,
    "members": list,
    "scenarios": list,
}
SCENARIO_FIELDS = {
    "id": str,
    "description": str,
    "steps": list,
    "needs": list,
    "continue_conversation": bool,
    "known_defect": str,
    "may_pass": bool,
}
ACTION_FIELDS = {"say": str, "http": dict, "upload": dict, "restart": bool, "sleep": NUMBER}
SAY_FIELDS = {
    "as": str,
    "timeout_s": NUMBER,
    "settle_s": NUMBER,
    "await_replies": int,
    "await_reply_regex": str,
    "repeat": int,
    "pad": int,
    "judge": bool,
}
STEP_FIELDS = {**ACTION_FIELDS, **SAY_FIELDS, "expect": list, "description": str}
HTTP_FIELDS = {"method": str, "path": str, "form": dict, "json": (dict, list)}
UPLOAD_FIELDS = {"project": str, "name": str, "png_rgb": list, "text": str, "b64": str}
MEMBER_FIELDS = {"chat": str, "name": str, "role": str}
MODEL_ROLES = {"head": dict, "sub": dict}
MARKER_FIELDS = ("known_defect", "may_pass")


def _type_name(kind) -> str:
    names = {str: "text", int: "a whole number", bool: "true or false", dict: "an object", list: "a list"}
    if kind is NUMBER:
        return "a number"
    return " or ".join(_type_name(k) for k in kind) if isinstance(kind, tuple) else names.get(kind, str(kind))


def _is_type(value, kind) -> bool:
    if isinstance(kind, tuple):
        return any(_is_type(value, k) for k in kind)
    if kind in (int, float):  # a bool is an int in Python, and never what a persona means
        return isinstance(value, kind) and not isinstance(value, bool)
    return isinstance(value, kind)


def _check_fields(obj, table: dict, where: str):
    """Refuse an unknown field (with the likely spelling) or a value of the wrong type."""
    if not isinstance(obj, dict):
        raise PersonaError(f"{where}: must be an object, got {obj!r}")
    for key, value in obj.items():
        if key not in table:
            near = difflib.get_close_matches(key, list(table), n=1)
            hint = f" (did you mean {near[0]!r}?)" if near else ""
            raise PersonaError(f"{where}: unknown field {key!r}{hint}")
        if not _is_type(value, table[key]):
            raise PersonaError(f"{where}: {key} must be {_type_name(table[key])}, got {value!r}")


def _fill(text: str, values: dict, where: str) -> str:
    def one(m):
        if m.group(1) not in values:
            raise PersonaError(f"{where}: no value for ${{{m.group(1)}}}")
        return str(values[m.group(1)])

    return PLACEHOLDER.sub(one, text)


def _substitute(node, values: dict, where: str):
    """${x} from the matrix row, in values and object keys. A bare $ is plain text.

    A string that is exactly one ${x} takes the row's value with its own JSON type, so a
    number, a list or a whole scenario list can vary per row. The rest fill as text.
    """
    if isinstance(node, str):
        whole = PLACEHOLDER.fullmatch(node)
        if whole and whole.group(1) in values:
            rest = {k: v for k, v in values.items() if k != whole.group(1)}  # a value cannot name itself
            return _substitute(copy.deepcopy(values[whole.group(1)]), rest, where)
        return _fill(node, values, where)
    if isinstance(node, list):
        return [_substitute(n, values, where) for n in node]
    if isinstance(node, dict):
        return {_fill(k, values, where): _substitute(v, values, where) for k, v in node.items()}
    return node


def filler(n_bytes: int, seed: int) -> str:
    """Readable, distinct padding (a fold counts distinct content, not repeats)."""
    words, i = [], seed
    size = 0
    while size < n_bytes:
        w = f"{FILLER_WORDS[i % len(FILLER_WORDS)]}{i % 977}"
        words.append(w)
        size += len(w) + 1
        i = i * 31 + 7
    return " ".join(words)


def _expand_repeats(steps: list) -> list:
    """A step with "repeat": N becomes N steps; "$i" counts from 1 and "pad" adds filler."""
    out = []
    for step in steps:
        count = step.get("repeat")
        if not count:
            out.append(step)
            continue
        body = {k: v for k, v in step.items() if k not in ("repeat", "pad")}
        for i in range(1, count + 1):
            one = {**body, "say": body["say"].replace("$i", str(i))}
            if step.get("pad"):
                one["say"] = f"{one['say']}\n\n{filler(step['pad'], i * 101)}"
            out.append(one)
    return out


def known_defects() -> dict:
    """The defect ids a persona may mark. A marker names a row here, so a typo cannot excuse a failure."""
    return json.loads((Path(__file__).with_name("known_defects.json")).read_text())


def _check_marker(item: dict, where: str):
    defect = item.get("known_defect", "")
    if defect and defect not in known_defects():
        raise PersonaError(f"{where}: {defect!r} is not in known_defects.json")
    if "may_pass" in item and "known_defect" not in item:  # empty: a matrix row switched the marker off
        raise PersonaError(f"{where}: may_pass needs a known_defect")


def _validate_expect(item, step: dict, where: str):
    if not isinstance(item, dict):
        raise PersonaError(f"{where}: an expect item is one name and one value: {item!r}")
    names = [k for k in item if k not in MARKER_FIELDS]
    if len(names) != 1:
        raise PersonaError(f"{where}: an expect item is one name and one value: {item!r}")
    name = names[0]
    if name not in ASSERTIONS:
        near = difflib.get_close_matches(name, list(ASSERTIONS), n=1)
        raise PersonaError(f"{where}: unknown assertion {name!r}" + (f" (did you mean {near[0]!r}?)" if near else ""))
    problem = check_arg(name, item[name])
    if problem:
        raise PersonaError(f"{where}: {name} {problem}")
    if name == "turn_resolved" and "as" in step:
        raise PersonaError(f"{where}: a member turn has no turn id, so turn_resolved cannot be checked on it")
    _check_marker(item, where)


def _validate_step(step: dict, where: str):
    _check_fields(step, STEP_FIELDS, where)
    kinds = [k for k in STEP_KINDS if k in step]
    if len(kinds) != 1:
        raise PersonaError(f"{where}: a step needs exactly one of {STEP_KINDS}, got {kinds}")
    if "say" not in step:
        stray = sorted(set(step) & set(SAY_FIELDS))
        if stray:
            raise PersonaError(f"{where}: only a say step takes {stray}")
    if "say" in step and not step.get("expect"):
        raise PersonaError(f"{where}: a turn with no expect proves nothing")
    if "http" in step:
        _check_fields(step["http"], HTTP_FIELDS, f"{where} http")
    if "upload" in step:
        _check_fields(step["upload"], UPLOAD_FIELDS, f"{where} upload")
    for item in step.get("expect", []):
        _validate_expect(item, step, where)


def _validate_scenario(sc: dict, earlier: set, where: str):
    _check_fields(sc, SCENARIO_FIELDS, where)
    for need in sc.get("needs", []):
        if need not in earlier:
            raise PersonaError(f"{where}: needs {need!r}, which no earlier scenario provides")
    _check_marker(sc, where)
    if not sc.get("steps"):
        raise PersonaError(f"{where}: no steps")
    for n, step in enumerate(sc["steps"]):
        _check_fields(step, STEP_FIELDS, f"{where}#{n}")
    sc["steps"] = _expand_repeats(sc["steps"])
    for n, step in enumerate(sc["steps"]):
        _validate_step(step, f"{where}#{n}")


def _validate_providers(p: dict, name: str):
    for slug in [*p.get("keys", {}), *p.get("models", {}).get("head", {}), *p.get("models", {}).get("sub", {})]:
        if slug not in KEY_FIELDS:
            raise PersonaError(f"{name}: unknown provider {slug!r}, one of {sorted(KEY_FIELDS)}")
    _check_fields(p.get("models", {}), MODEL_ROLES, f"{name} models")
    if p.get("host") and p["host"] not in KEY_FIELDS:
        raise PersonaError(f"{name}: unknown host {p['host']!r}, one of {sorted(KEY_FIELDS)}")
    for slug, verdict in p.get("key_expect", {}).items():
        if slug not in p.get("keys", {}) or verdict not in KEY_VERDICTS:
            raise PersonaError(f"{name}: key_expect {slug}={verdict!r} needs a key and one of {KEY_VERDICTS}")
    for member in p.get("members", []):
        _check_fields(member, MEMBER_FIELDS, f"{name} member")
        if not member.get("chat"):
            raise PersonaError(f"{name}: a member needs a chat id")


def validate_persona(p: dict) -> dict:
    """Refuse what would silently test nothing: no name, no scenario, no step, a bad check, a typo."""
    name = p.get("name", "") if isinstance(p, dict) else ""
    if not isinstance(name, str) or not re.fullmatch(SAFE_NAME, name):
        raise PersonaError(f"bad persona name {name!r}")
    _check_fields(p, PERSONA_FIELDS, name)
    _validate_providers(p, name)
    if not p.get("scenarios"):
        raise PersonaError(f"{name}: no scenarios")
    seen = set()
    for sc in p["scenarios"]:
        sid = sc.get("id", "") if isinstance(sc, dict) else ""
        if not isinstance(sid, str) or not re.fullmatch(SAFE_NAME, sid) or sid in seen:
            raise PersonaError(f"{name}: scenario id bad or repeated: {sid!r}")
        _validate_scenario(sc, seen, f"{name}/{sid}")
        seen.add(sid)
    return p


def fetch_truth(var: str, spec: dict, truth_dir: Path) -> str:
    """A known answer read from its source now, with the raw body kept beside the value.

    Without this a graded value is a number someone typed in once. A source that cannot
    be read stops the run: nothing has been spent yet.
    """
    req = urllib.request.Request(spec["url"], headers={"User-Agent": "Mozilla/5.0 persona-rig"})
    try:
        with urllib.request.urlopen(req, timeout=30) as r:
            status, body = r.status, r.read()
    except OSError as e:
        raise PersonaError(f"truth {var}: cannot read {spec['url']}: {e}") from e
    m = re.search(spec["regex"], body.decode("utf-8", "replace"))
    if not m:
        raise PersonaError(f"truth {var}: {spec['regex']!r} found nothing in {spec['url']}")
    truth_dir.mkdir(parents=True, exist_ok=True)
    (truth_dir / f"{var}.body").write_bytes(body)
    record = {
        "var": var,
        "url": spec["url"],
        "regex": spec["regex"],
        "value": m.group(1),
        "status": status,
        "at": datetime.datetime.now(datetime.timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ"),
        "sha256": hashlib.sha256(body).hexdigest(),
        "matched": m.group(0),
    }
    (truth_dir / f"{var}.json").write_text(json.dumps(record, indent=1))
    return m.group(1)


def load_personas(path: str, truth_dir=None) -> list:
    """One persona, or a matrix: {"vars": {}, "truth": {}, "matrix": [{}], "persona": {}}.

    With no truth_dir (a dry load) a truth value reads as a placeholder and nothing is fetched.
    """
    raw = json.loads(Path(path).read_text())
    if "matrix" not in raw:
        return [validate_persona(raw)]
    _check_fields(raw, {"description": str, "vars": dict, "truth": dict, "matrix": list, "persona": dict}, path)
    if not raw["matrix"]:
        raise PersonaError(f"{path}: empty matrix")
    truth = {}
    for var, spec in raw.get("truth", {}).items():
        truth[var] = fetch_truth(var, spec, truth_dir) if truth_dir else f"UNFETCHED-{var}"
    out = []
    for row in raw["matrix"]:
        values = {**raw.get("vars", {}), **truth, **row}
        out.append(validate_persona(_substitute(raw["persona"], values, path)))
    return out
