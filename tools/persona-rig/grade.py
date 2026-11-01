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

# What the engine says when a turn did not produce a real answer. A reply holding one
# of these was delivered, but it is a failure notice, not an answer.
ERROR_REPLY_RE = re.compile(
    r"that didn'?t finish|couldn'?t start that agent|needs credits|no provider key|"
    r"rate limit|try again (later|in)|something went wrong|ran out of|request failed|"
    r"\bHTTP \d{3}\b|^\s*\{\s*\"",
    re.I | re.M,
)


def _reply_text(rec: dict) -> str:
    """Every assistant message this step produced, the turn's own reply first."""
    parts = [rec.get("reply") or ""] + [r for r in rec.get("replies", []) if r != rec.get("reply")]
    return "\n".join(p for p in parts if p)


def _tool_names(rec: dict) -> list:
    return [t.get("name", "") for t in rec.get("tools", [])]


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


def log_regex(rec, arg, _ctx):
    text = rec.get("log", "")
    m = re.search(arg, text, re.I)
    return m is not None, m.group(0) if m else f"no match in {len(text)} log bytes"


def log_not_regex(rec, arg, _ctx):
    text = rec.get("log", "")
    m = re.search(arg, text, re.I)
    return m is None, _seen(m.group(0)) if m else f"no match in {len(text)} log bytes"


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
    u = rec.get("usage_delta", {})
    got = int(u.get("in", 0)) + int(u.get("out", 0))
    return got <= arg, f"{got} tokens"


def file_contains(_rec, arg, ctx):
    """{"project": "p", "name": "n", "text": "x"}: read through the Files panel's own route."""
    status, body = ctx.file_bytes(arg.get("project", ""), arg["name"])
    return status == 200 and arg.get("text", "") in body, f"status {status}: {_seen(body)}"


def file_absent(_rec, arg, ctx):
    status, body = ctx.file_bytes(arg.get("project", ""), arg["name"])
    return status != 200, f"status {status}: {_seen(body)}"


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


def grade_step(rec: dict, expect: list, ctx) -> list:
    """Run a step's assertions; a check that raises fails, it never skips."""
    results = []
    for item in expect:
        name, arg = next(iter(item.items()))
        try:
            ok, seen = ASSERTIONS[name](rec, arg, ctx)
        except Exception as e:  # a broken check is a failed check, never a pass
            ok, seen = False, f"check raised {type(e).__name__}: {e}"
        results.append({"check": name, "arg": arg, "pass": bool(ok), "seen": seen})
    return results
