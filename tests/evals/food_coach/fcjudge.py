"""The fresh-context judge.

One stateless call per transcript: the written rubric, the scenario's facts and
criteria, and the transcript go in; a verdict per criterion comes out. The judge
has no tools, no memory of any other run, and nothing from the assistant's own
prompt. There is no keyword matching anywhere: the judge reads the words.
"""

from __future__ import annotations

import hashlib
import json
import os
import time
import urllib.error
import urllib.request
from pathlib import Path

SYSTEM = (
    "You are a strict, literal evaluator of a personal food-coach assistant. You are "
    "given a rubric, the facts of one scenario, the criteria to decide, and a "
    "transcript. Judge only against the rubric and criteria. A criterion passes only "
    "when the reply clearly satisfies it; if the evidence is ambiguous, it fails. Do "
    "not reward length, friendliness or effort. Quote the few words you rely on. The "
    "facts are true; where the reply contradicts them, the facts win. Decide each "
    "criterion on its own, write the evidence first, and let the verdict follow from it. "
    "Answer only by calling the record_verdict tool."
)

VERDICT_SCHEMA = {
    "type": "object",
    "additionalProperties": False,
    "required": ["criteria", "summary"],
    "properties": {
        "criteria": {
            "type": "array",
            "items": {
                "type": "object",
                "additionalProperties": False,
                "required": ["id", "evidence", "verdict"],
                "properties": {
                    "id": {"type": "string"},
                    "evidence": {"type": "string"},
                    "verdict": {"type": "string", "enum": ["pass", "fail"]},
                },
            },
        },
        "summary": {"type": "string"},
    },
}


def load_keys(env_path: str | None = None) -> dict:
    """Provider keys by name from the process environment, then the dotenv file."""
    path = env_path or os.environ.get("NIMBUS_ENV_FILE", "")
    keys: dict = {}
    if path and Path(path).exists():
        for line in Path(path).read_text(encoding="utf-8").splitlines():
            line = line.strip()
            if not line or line.startswith("#") or "=" not in line:
                continue
            name, _, value = line.partition("=")
            keys[name.strip()] = value.strip().strip("\"'")
    for name in ("ANTHROPIC_API_KEY", "OPENAI_API_KEY"):
        if os.environ.get(name):
            keys[name] = os.environ[name]
    return keys


def render_transcript(history: list[dict], turns: list[dict]) -> str:
    """The transcript as the judge reads it (tool calls shortened)."""
    out = []
    if history:
        out.append("## Earlier chat (context only, not graded)")
        for h in history:
            who = "OWNER" if h["role"] == "user" else "ASSISTANT"
            out.append(f"{who}: {h['text']}")
        out.append("")
    out.append("## Graded turns")
    for n, t in enumerate(turns, 1):
        out.append(f"### Turn {n}")
        out.append(f"OWNER: {t['owner']}")
        for call, res in zip(t.get("tool_calls", []), t.get("tool_results", []) + [""] * 99):
            out.append(f"ASSISTANT CALLED: {call[:240]}")
            if res:
                out.append(f"TOOL RESULT: {res[:300]}")
        sent = t.get("deliveries") or ([t["reply"]] if t.get("reply") else [])
        if not sent:
            out.append("ASSISTANT REPLY: (no reply was delivered)")
        for k, msg in enumerate(sent, 1):
            tag = "ASSISTANT REPLY" if len(sent) == 1 else f"ASSISTANT MESSAGE {k} of {len(sent)}"
            out.append(f"{tag} (this is what the owner receives):\n{msg}")
    return "\n".join(out)


def build_user_prompt(rubric: str, scn: dict, facts: list[str], transcript: str) -> str:
    crit = "\n".join(f"- {c['id']}: {c['text']}" for c in scn["criteria"])
    fact_lines = "\n".join(f"- {f}" for f in facts)
    return (
        f"# Rubric\n{rubric}\n\n# Scenario: {scn['title']}\n"
        f"## Facts (true)\n{fact_lines}\n\n## Criteria to decide (one verdict each, all of them)\n{crit}\n\n"
        f"# Transcript\n{transcript}\n\n"
        "Return one verdict for every criterion id above, with a short quoted evidence."
    )


def _post(url: str, headers: dict, body: dict, timeout: int = 120, tries: int = 4) -> dict:
    data = json.dumps(body).encode()
    last = ""
    for attempt in range(tries):
        req = urllib.request.Request(url, data=data, headers=headers | {"content-type": "application/json"})
        try:
            with urllib.request.urlopen(req, timeout=timeout) as resp:
                return json.loads(resp.read())
        except urllib.error.HTTPError as exc:
            last = f"HTTP {exc.code}: {exc.read().decode(errors='replace')[:300]}"
            if exc.code not in (408, 429, 500, 502, 503, 504, 529):
                break
        except (urllib.error.URLError, TimeoutError) as exc:
            last = f"{type(exc).__name__}: {exc}"
        time.sleep(5 * (attempt + 1))
    raise RuntimeError(f"judge request failed: {last}")


def _anthropic(model: str, key: str, user: str) -> tuple[dict, dict]:
    body = {
        "model": model,
        "max_tokens": 2500,
        "system": SYSTEM,
        "messages": [{"role": "user", "content": user}],
        "tools": [
            {
                "name": "record_verdict",
                "description": "Record one verdict per criterion.",
                "input_schema": VERDICT_SCHEMA,
            }
        ],
        "tool_choice": {"type": "auto"},
    }
    resp = _post(
        "https://api.anthropic.com/v1/messages",
        {"x-api-key": key, "anthropic-version": "2023-06-01"},
        body,
    )
    for block in resp.get("content", []):
        if block.get("type") == "tool_use":
            return block["input"], resp.get("usage", {})
    raise RuntimeError("judge returned no verdict block")


def _openai(model: str, key: str, user: str) -> tuple[dict, dict]:
    body = {
        "model": model,
        "input": [{"role": "system", "content": SYSTEM}, {"role": "user", "content": user}],
        "text": {"format": {"type": "json_schema", "name": "verdict", "schema": VERDICT_SCHEMA, "strict": True}},
        "reasoning": {"effort": "low"},
        "max_output_tokens": 6000,
    }
    resp = _post("https://api.openai.com/v1/responses", {"authorization": f"Bearer {key}"}, body)
    for item in resp.get("output", []):
        for part in item.get("content", []) if item.get("type") == "message" else []:
            if part.get("type") == "output_text":
                return json.loads(part["text"]), resp.get("usage", {})
    raise RuntimeError("judge returned no verdict text")


def call_judge(spec: str, keys: dict, user: str) -> tuple[dict, dict]:
    """spec is 'provider/model'. Returns (verdict, provider usage object)."""
    provider, _, model = spec.partition("/")
    if provider == "anthropic":
        return _anthropic(model, keys["ANTHROPIC_API_KEY"], user)
    if provider == "openai":
        return _openai(model, keys["OPENAI_API_KEY"], user)
    raise ValueError(f"unknown judge provider {provider!r}")


def rubric_sha(rubric: str, scn: dict, facts: list[str]) -> str:
    """A short fingerprint of everything the judge is told, so mixed rubrics show."""
    blob = json.dumps([SYSTEM, rubric, scn["criteria"], facts, VERDICT_SCHEMA], sort_keys=True)
    return hashlib.sha256(blob.encode()).hexdigest()[:12]


def normalize(verdict: dict, scn: dict) -> dict:
    """Every scenario criterion gets exactly one verdict; a missing one fails."""
    by_id = {c.get("id"): c for c in verdict.get("criteria", [])}
    rows = []
    for crit in scn["criteria"]:
        got = by_id.get(crit["id"])
        ok = bool(got) and got.get("verdict") == "pass"
        rows.append(
            {
                "id": crit["id"],
                "required": bool(crit.get("required")),
                "pass": ok,
                "evidence": (got or {}).get("evidence", "no verdict returned"),
            }
        )
    passed = all(r["pass"] for r in rows if r["required"])
    return {"criteria": rows, "pass": passed, "summary": verdict.get("summary", "")}
