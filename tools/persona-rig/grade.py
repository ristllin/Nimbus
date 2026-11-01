"""grade - the hard assertions a persona step can make.

Every assertion is one small check over a step record (what the rig captured) and
returns (passed, what was seen). The table is closed: a persona naming an assertion
that is not in ASSERTIONS is refused when it loads, so a typo can never read as a pass.
test_persona_rig.py holds one passing and one failing case for every name here.

The soft signal, an independent LLM judge, lives in tests/hil/qa_judge.py and is
wired in persona_rig.py; it never changes a verdict.
"""

from __future__ import annotations

import json
import re
import sys
from pathlib import Path

# What the engine says when a turn did not produce a real answer. A reply holding one
# of these was delivered, but it is a failure notice, not an answer.
ERROR_REPLY_RE = re.compile(
    r"that didn'?t finish|couldn'?t start that agent|needs credits|no provider key|"
    r"rate limit|try again (later|in)|something went wrong|ran out of|request failed|"
    r"\bHTTP \d{3}\b|^\s*\{\s*\"|could not be read|\A\s*(done|placeholder)\.?\s*\Z",
    re.I | re.M,
)


def _reply_text(rec: dict) -> str:
    """Every assistant message this step produced, the turn's own reply first."""
    parts = [rec.get("reply") or ""] + [r for r in rec.get("replies", []) if r != rec.get("reply")]
    return "\n".join(p for p in parts if p)


def _tool_names(rec: dict) -> list:
    """Raises when no trace exists (no turn completed), so a tool check fails, never passes."""
    return [t.get("name", "") for t in rec["tools"]]


def _dig(obj, dotted: str):
    """obj['a']['b'] for "a.b"; None when any part is absent."""
    for part in dotted.split("."):
        if not isinstance(obj, dict) or part not in obj:
            return None
        obj = obj[part]
    return obj


def _seen(text: str, cap: int = 300) -> str:
    return text if len(text) <= cap else text[:cap] + "..."


def reply_nonempty(rec, _arg, _ctx):
    text = _reply_text(rec)
    return bool(text.strip()), _seen(text) or "empty reply"


def no_error_reply(rec, _arg, _ctx):
    text = _reply_text(rec)
    m = ERROR_REPLY_RE.search(text)
    return m is None, f"error wording {m.group(0)!r} in: {_seen(text)}" if m else "no error wording"


def reply_contains(rec, arg, _ctx):
    text = _reply_text(rec)
    return str(arg).lower() in text.lower(), _seen(text)


def reply_not_contains(rec, arg, _ctx):
    text = _reply_text(rec)
    return str(arg).lower() not in text.lower(), _seen(text)


def reply_regex(rec, arg, _ctx):
    text = _reply_text(rec)
    return re.search(arg, text, re.I | re.S) is not None, _seen(text)


def reply_not_regex(rec, arg, _ctx):
    text = _reply_text(rec)
    return re.search(arg, text, re.I | re.S) is None, _seen(text)


def tool_called(rec, arg, _ctx):
    names = _tool_names(rec)
    return any(arg in n for n in names), f"tools: {names}"


def tool_not_called(rec, arg, _ctx):
    names = _tool_names(rec)
    return not any(arg in n for n in names), f"tools: {names}"


def _tool_field_regex(rec, arg, field):
    """{"tool": "files_write", "regex": "x"}: a call of that tool whose args or result match."""
    rows = [t for t in rec["tools"] if arg["tool"] in t.get("name", "")]
    hit = any(re.search(arg["regex"], t.get(field, ""), re.I | re.S) for t in rows)
    return hit, f"{field} of {arg['tool']}: {[_seen(t.get(field, ''), 200) for t in rows]}"


def tool_args_regex(rec, arg, _ctx):
    return _tool_field_regex(rec, arg, "args")


def tool_result_regex(rec, arg, _ctx):
    """The daemon folds each anatomy line at 200 characters, so only a result's start can match."""
    return _tool_field_regex(rec, arg, "result")


def log_regex(rec, arg, _ctx):
    text = rec.get("log", "")
    m = re.search(arg, text, re.I)
    return m is not None, m.group(0) if m else f"no match in {len(text)} log bytes"


def log_not_regex(rec, arg, _ctx):
    text = rec.get("log", "")
    m = re.search(arg, text, re.I)
    return m is None, _seen(m.group(0)) if m else f"no match in {len(text)} log bytes"


def _fabrication_map() -> list:
    """The HIL suite's claim families: (claim phrases, tool names that back them)."""
    sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tests" / "hil"))
    from scenarios_complex import FAB_DONE_MAP

    return FAB_DONE_MAP


def no_fabricated_action(rec, _arg, _ctx):
    """A completed-action claim ("email sent") needs a tool of the matching family this turn."""
    text = _reply_text(rec).lower()
    names = " ".join(_tool_names(rec)).lower()
    for phrases, families in _fabrication_map():
        for phrase in phrases:
            if phrase in text and not any(f in names for f in families):
                return False, f"claims {phrase!r} with tools: {_tool_names(rec)}"
    return True, f"tools: {_tool_names(rec)}"


def http_status(rec, arg, _ctx):
    got = rec.get("http", {}).get("status")
    return got == arg, f"status {got}"


def body_contains(rec, arg, _ctx):
    body = rec.get("http", {}).get("body", "")
    return str(arg) in body, _seen(body)


def body_not_contains(rec, arg, _ctx):
    body = rec.get("http", {}).get("body", "")
    return str(arg) not in body, _seen(body)


def body_regex(rec, arg, _ctx):
    body = rec.get("http", {}).get("body", "")
    return re.search(arg, body, re.I) is not None, _seen(body)


def turn_resolved(rec, arg, _ctx):
    """Did the turn's own poll answer, as the web page needs? false asserts it did not."""
    got = rec.get("turn_resolved")
    return got is bool(arg), f"turn_resolved = {got}, last poll: {_seen(rec.get('last_poll', ''))}"


def tokens_max(rec, arg, _ctx):
    u = rec["usage_delta"]
    got = int(u["in"]) + int(u["out"])
    return got <= arg, f"{got} tokens"


def file_contains(_rec, arg, ctx):
    """{"project": "p", "name": "n", "text": "x"}: read through the Files panel's own route."""
    status, body = ctx.file_bytes(arg.get("project", ""), arg["name"])
    return status == 200 and arg.get("text", "") in body, f"status {status}: {_seen(body)}"


def file_absent(_rec, arg, ctx):
    status, body = ctx.file_bytes(arg.get("project", ""), arg["name"])
    return status == 404, f"status {status}: {_seen(body)}"


def api_min(_rec, arg, ctx):
    """{"path": "/api/mem/stats", "field": "vectors", "min": 1}: a number the API reports."""
    status, body = ctx.get(arg["path"])
    try:
        got = _dig(json.loads(body), arg["field"])
    except ValueError:
        return False, f"status {status}, not JSON: {_seen(body)}"
    return isinstance(got, (int, float)) and got >= arg["min"], f"{arg['field']} = {got}"


def api_equals(_rec, arg, ctx):
    """{"path": "/api/orch", "field": "orchHost", "value": "mistral"}: a value the API reports."""
    status, body = ctx.get(arg["path"])
    try:
        got = _dig(json.loads(body), arg["field"])
    except ValueError:
        return False, f"status {status}, not JSON: {_seen(body)}"
    return got == arg["value"], f"{arg['field']} = {got!r}"


ASSERTIONS = {
    "reply_nonempty": reply_nonempty,
    "no_error_reply": no_error_reply,
    "reply_contains": reply_contains,
    "reply_not_contains": reply_not_contains,
    "reply_regex": reply_regex,
    "reply_not_regex": reply_not_regex,
    "tool_called": tool_called,
    "tool_not_called": tool_not_called,
    "tool_args_regex": tool_args_regex,
    "tool_result_regex": tool_result_regex,
    "no_fabricated_action": no_fabricated_action,
    "log_regex": log_regex,
    "log_not_regex": log_not_regex,
    "http_status": http_status,
    "body_contains": body_contains,
    "body_not_contains": body_not_contains,
    "body_regex": body_regex,
    "turn_resolved": turn_resolved,
    "tokens_max": tokens_max,
    "file_contains": file_contains,
    "file_absent": file_absent,
    "api_min": api_min,
    "api_equals": api_equals,
}


# What each assertion takes, checked when a persona loads, so a wrong value is refused
# instead of raising mid-run and reading as a failed check (or worse, a passing one).
_NUM = (int, float)
_TOOL_FIELD = {"tool": str, "regex": str}
_FILE = {"project": str, "name": str, "text": str}
ARG_SHAPES = {
    "reply_nonempty": bool,
    "no_error_reply": bool,
    "reply_contains": str,
    "reply_not_contains": str,
    "reply_regex": str,
    "reply_not_regex": str,
    "tool_called": str,
    "tool_not_called": str,
    "tool_args_regex": _TOOL_FIELD,
    "tool_result_regex": _TOOL_FIELD,
    "no_fabricated_action": bool,
    "log_regex": str,
    "log_not_regex": str,
    "http_status": int,
    "body_contains": str,
    "body_not_contains": str,
    "body_regex": str,
    "turn_resolved": bool,
    "tokens_max": _NUM,
    "file_contains": _FILE,
    "file_absent": _FILE,
    "api_min": {"path": str, "field": str, "min": _NUM},
    "api_equals": {"path": str, "field": str, "value": object},
}
REGEX_ARGS = {"reply_regex", "reply_not_regex", "log_regex", "log_not_regex", "body_regex"}
OPTIONAL_ARG_FIELDS = {"project", "text"}
# Checks that read the running instance: they cannot be re-graded from a stored step.
LIVE_CHECKS = {"file_contains", "file_absent", "api_min", "api_equals"}


def _plain_type(value, kind) -> bool:
    if kind is object:
        return True
    if kind is bool or isinstance(value, bool):
        return isinstance(value, bool) and kind is bool
    return isinstance(value, kind)


def _check_arg_fields(arg, shape: dict):
    if not isinstance(arg, dict):
        return f"takes {sorted(shape)}, got {arg!r}"
    unknown = sorted(set(arg) - set(shape))
    if unknown:
        return f"has unknown fields {unknown}"
    missing = sorted(set(shape) - set(arg) - OPTIONAL_ARG_FIELDS)
    if missing:
        return f"is missing {missing}"
    bad = [k for k, v in arg.items() if not _plain_type(v, shape[k])]
    return f"has a wrong type in {bad}: {arg!r}" if bad else ""


def check_arg(name: str, arg) -> str:
    """Empty when the value fits the assertion, else what is wrong with it."""
    shape = ARG_SHAPES[name]
    if isinstance(shape, dict):
        return _check_arg_fields(arg, shape)
    if not _plain_type(arg, shape):
        return f"takes a value like {shape}, got {arg!r}"
    if name in REGEX_ARGS:
        try:
            re.compile(arg)
        except re.error as e:
            return f"has a bad regex: {e}"
    return ""


def _grade_one(rec: dict, name: str, arg, ctx) -> tuple:
    try:
        ok, seen = ASSERTIONS[name](rec, arg, ctx)
    except Exception as e:  # a broken check is a failed check, never a pass
        ok, seen = False, f"check raised {type(e).__name__}: {e}"
    return bool(ok), seen


def grade_step(rec: dict, expect: list, ctx) -> list:
    """Run a step's assertions; a check that raises fails, it never skips."""
    results = []
    for item in expect:
        name = next(k for k in item if k not in ("known_defect", "may_pass"))
        ok, seen = _grade_one(rec, name, item[name], ctx)
        row = {"check": name, "arg": item[name], "pass": ok, "seen": seen}
        if item.get("known_defect"):
            row.update(known_defect=item["known_defect"], may_pass=bool(item.get("may_pass")))
        results.append(row)
    return results


def regrade_step(rec: dict) -> list:
    """Stored verdicts that today's graders would change: [(check, stored, now)]. Live checks are skipped."""
    flips = []
    for row in rec.get("checks", []):
        if row["check"] not in ASSERTIONS or row["check"] in LIVE_CHECKS:
            continue
        ok, _seen = _grade_one(rec, row["check"], row["arg"], None)
        if ok != row["pass"]:
            flips.append((row["check"], row["pass"], ok))
    return flips
