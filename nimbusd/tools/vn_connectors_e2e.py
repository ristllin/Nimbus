#!/usr/bin/env python3
"""Virtual Nimbus connectors e2e: a Lumi-shaped instance pulls REAL calendar data.

Drives a running nimbusd over its HTTP control surface exactly as the web app
does, and records a redacted evidence bundle:

  (a) configure the Mistral head/sub model and the Studio connectors (gcal,
      notion, slack on prov=mistral), run POST /api/verify provider=mistral, and
      wait for the workspace probe to flip gcal usable; assert that the
      /api/connectors badge, the /api/tools Capabilities row and the model-facing
      catalog (/api/connectors/catalog) agree for every connector;
  (b) ask "What is on my calendar today?" over /api/chat and assert the reply
      (or the follow-up synthesis reply) names at least one event title that an
      independent oracle call sees on the same calendar;
  (c) ask a Notion and a Slack question and record what comes back (listed
      connectors are usable by rule; an empty or needs-connect answer is
      relayed as-is, never counted as a pass or a fail of the rule).

The oracle is ONE direct Mistral Conversations call with the google_calendar
connector (the same key the instance uses), so the assertion is on real data
the instance could not have invented.

Secrets never ride argv: the instance web token comes from NIMBUSD_WEB_TOKEN
and the oracle key from MISTRAL_ORACLE_KEY. Every artifact written is scrubbed
of both values and of any long token-like string. Paid calls: the instance's
turns plus one oracle call, all on the configured Mistral model.

Usage:
  NIMBUSD_WEB_TOKEN=... MISTRAL_ORACLE_KEY=... \\
    python3 nimbusd/tools/vn_connectors_e2e.py --base http://127.0.0.1:18787 \\
      --evidence <dir> [--model mistral-small-latest] [--skip-bc]
Exit 0 only when (a) and (b) pass.
"""

import argparse
import datetime
import json
import os
import re
import sys
import time
import urllib.error
import urllib.parse
import urllib.request

STUDIO = [
    {
        "name": "gcal",
        "type": "gcal",
        "prov": "mistral",
        "kind": "connector",
        "cid": "connector_googlecalendar",
        "en": 1,
    },
    {"name": "notion", "type": "notion", "prov": "mistral", "kind": "connector", "cid": "notion", "en": 1},
    {"name": "slack", "type": "slack", "prov": "mistral", "kind": "connector", "cid": "slack", "en": 1},
]

SECRETS = []


def redact(text):
    for s in SECRETS:
        if s:
            text = text.replace(s, s[:4] + "[REDACTED]")
    # Any long opaque token-like run (keys, bearer tokens) that slipped through.
    return re.sub(r"\b[A-Za-z0-9_\-]{32,}\b", lambda m: m.group(0)[:4] + "[REDACTED]", text)


class Instance:
    def __init__(self, base, token):
        self.base = base.rstrip("/")
        self.token = token

    def call(self, method, path, form=None, timeout=30):
        data = urllib.parse.urlencode(form).encode() if form is not None else None
        req = urllib.request.Request(self.base + path, data=data, method=method)
        req.add_header("X-Nimbus-Token", self.token)
        if data is not None:
            req.add_header("Content-Type", "application/x-www-form-urlencoded")
        try:
            with urllib.request.urlopen(req, timeout=timeout) as r:
                return r.status, r.read().decode("utf-8", "replace")
        except urllib.error.HTTPError as e:
            return e.code, e.read().decode("utf-8", "replace")

    def get_json(self, path, retries=40):
        # /api/tools and the memory panels answer an honest 503 while a turn or a
        # background probe holds the engine; retry like the web pollers do.
        for _ in range(retries):
            st, body = self.call("GET", path)
            if st == 200:
                return json.loads(body)
            time.sleep(0.5)
        raise RuntimeError(f"GET {path} never answered 200 (last {st})")


def oracle_titles(key, model, day):
    """Event titles on `day`, straight from Mistral's google_calendar connector."""
    body = {
        "model": model,
        "inputs": (
            f"List the title of every event on my Google Calendar on {day}. "
            "Reply with a JSON array of the exact titles and nothing else."
        ),
        "tools": [{"type": "connector", "connector_id": "google_calendar"}],
        "store": False,
    }
    req = urllib.request.Request(
        "https://api.mistral.ai/v1/conversations",
        data=json.dumps(body).encode(),
        method="POST",
        headers={"Authorization": "Bearer " + key, "Content-Type": "application/json"},
    )
    with urllib.request.urlopen(req, timeout=120) as r:
        out = json.loads(r.read().decode())
    ran_tool = any(o.get("type") == "tool.execution" for o in out.get("outputs", []))
    text = ""
    for o in out.get("outputs", []):
        if o.get("type") == "message.output":
            c = o.get("content")
            text = c if isinstance(c, str) else "".join(p.get("text", "") for p in c if isinstance(p, dict))
    m = re.search(r"\[.*\]", text, re.S)
    titles = []
    if m:
        try:
            titles = [t.strip() for t in json.loads(m.group(0)) if isinstance(t, str) and t.strip()]
        except ValueError:
            titles = []
    return ran_tool, titles, out.get("usage", {}), text


def wait_healthy(inst, secs=60):
    end = time.time() + secs
    while time.time() < end:
        try:
            with urllib.request.urlopen(inst.base + "/healthz", timeout=5) as r:
                if r.status == 200:
                    return True
        except (urllib.error.URLError, ConnectionError, TimeoutError):
            pass
        time.sleep(1)
    return False


def badge_auth(conns, name):
    for c in conns.get("configured", []):
        if c.get("name") == name:
            return c.get("auth")
    return None


def availability(tools, name):
    for t in tools.get("tools", []):
        if t.get("group") == "connector" and t.get("name") == name:
            return t.get("availability")
    return None


# The engine's 429 replies, one per quota window (rateLimitReply in
# lib/harness/src/rate_limit.cpp; test_vn_connectors_e2e.py pins this table to
# it). Matched on that copy, never on a word it may drop: the old "rate-limited"
# match went dead when the copy changed (CUM-460) and the retry silently stopped.
# Every 429 reply carries one of these refusal clauses; the engine's other
# failure replies ("That didn't finish - ...", "so that couldn't finish") do not.
RATE_LIMIT_CLAUSES = ("so that didn't finish", "so waiting won't help")
# Distinctive phrase -> window slug (rateLimitSlug); the first match wins, and a
# 429 reply that names no window is "unknown".
RATE_LIMIT_WINDOWS = (
    ("per-minute limit", "minute"),
    ("midnight UTC", "day-utc"),
    ("daily limit", "day"),
    ("waiting won't help", "plan"),
    ("quota is used up", "quota"),
)
# Only these can reopen within `settle` seconds; a spent daily or monthly quota
# or a plan that allows no such request cannot, so retrying them only spends time.
RETRY_WINDOWS = ("minute", "unknown")


def rate_limit_window(text):
    """The quota window a 429 reply names ("minute", "day-utc", ...), or None
    when the text is not one of the engine's 429 replies."""
    if not any(clause in text for clause in RATE_LIMIT_CLAUSES):
        return None
    return next((slug for phrase, slug in RATE_LIMIT_WINDOWS if phrase in text), "unknown")


def ask_with_retry(inst, text, secs, settle):
    """ask(), retried ONCE after `settle` seconds when the provider rate-limited it
    on a window that can reopen by then (per-minute, or one it did not name).

    A per-minute provider quota is not a product failure; every attempt is kept
    in the evidence, with the window each 429 reply named, so a retry is visible,
    never hidden."""
    attempts = []
    for _ in range(2):
        replies, meta = ask(inst, text, secs)
        windows = [rate_limit_window(r["text"]) for r in replies]
        attempts.append({"meta": meta, "replies": replies, "rate_limit": [w for w in windows if w]})
        # A rate-limited SYNTHESIS still delivers the sub-agent's raw result; only
        # retry when nothing substantive came back at all.
        substantive = [r for r, w in zip(replies, windows) if not w and not r["text"].startswith("On it.")]
        retry = any(w in RETRY_WINDOWS for w in windows)
        if not retry or len(substantive) > 1 or any("[FRESH RESULTS]" in r["text"] for r in substantive):
            break
        time.sleep(settle)
    return attempts


def ask(inst, text, secs):
    """Post one chat turn; collect every assistant reply delivered after it."""
    st, body = inst.call("GET", "/api/replies?after=0")
    after = json.loads(body).get("lastSeq", 0) if st == 200 else 0
    st, body = inst.call("POST", "/api/chat", {"text": text})
    if st != 200:
        return [], f"POST /api/chat -> {st}"
    turn = json.loads(body).get("turn")
    replies, t0 = [], time.time()
    first_at = None
    while time.time() - t0 < secs:
        st, body = inst.call("GET", f"/api/replies?after={after}")
        if st == 200:
            for e in json.loads(body).get("replies", []):
                after = max(after, e.get("seq", 0))
                if e.get("role") == "assistant" and e.get("text"):
                    replies.append({"seq": e.get("seq"), "t": round(time.time() - t0, 1), "text": e["text"]})
                    first_at = first_at or time.time()
        # A spawned sub reports back in a later synthesis turn: keep listening a
        # while after the first reply, then stop once things go quiet.
        if first_at and time.time() - first_at > 90:
            break
        time.sleep(2)
    return replies, f"turn={turn}"


def step_a(inst, ev, model):
    log = {}
    st, body = inst.call("POST", "/api/orch", {"orchM_mistral": model})
    log["set_model"] = [st, body]
    st, body = inst.call("POST", "/api/connectors", {"blob": json.dumps(STUDIO)})
    log["set_connectors"] = [st, body]
    log["connectors_before_verify"] = inst.get_json("/api/connectors")
    st, body = inst.call("POST", "/api/verify", {"provider": "mistral"})
    log["verify"] = [st, body]
    flipped = False
    for _ in range(60):
        conns = inst.get_json("/api/connectors")
        if badge_auth(conns, "gcal") == 1:
            flipped = True
            break
        time.sleep(1)
    conns = inst.get_json("/api/connectors")
    tools = inst.get_json("/api/tools")
    st, catalog = inst.call("GET", "/api/connectors/catalog")
    orch = inst.get_json("/api/orch")
    agree = {}
    for c in STUDIO:
        n = c["name"]
        a = badge_auth(conns, n)
        av = availability(tools, n)
        cat_usable = f"{n} (not usable" not in catalog
        agree[n] = {
            "auth": a,
            "availability": av,
            "catalog_usable": cat_usable,
            "agree": (a == 1) == (av in ("orchestrator-direct", "subsessions-only")) == cat_usable,
        }
    ev("a_api_connectors.json", json.dumps(conns, indent=2))
    ev(
        "a_api_tools_connectors.json",
        json.dumps([t for t in tools.get("tools", []) if t.get("group") == "connector"], indent=2),
    )
    ev("a_catalog.txt", catalog)
    ev("a_api_orch.json", json.dumps(orch, indent=2))
    ev("a_steps.json", json.dumps(log, indent=2))
    ok = flipped and all(v["agree"] for v in agree.values()) and agree["gcal"]["auth"] == 1
    return ok, {"gcal_flipped_usable": flipped, "agreement": agree}


def title_hit(title, text):
    """How the reply names an oracle event: the exact title, or at least two of its
    distinctive words (6+ letters) - real calendar data the model could not invent,
    while tolerating a reply that shortens a long title."""
    low = text.lower()
    if title.lower() in low:
        return "exact"
    words = {w for w in re.findall(r"[a-z0-9]+", title.lower()) if len(w) >= 6}
    found = sorted(w for w in words if w in low)
    return f"words:{len(found)}/{len(words)}" if len(found) >= 2 else ""


def step_b(inst, ev, oracle_key, model, secs, settle):
    # The instance's turn FIRST, in a fresh provider minute: a small key's
    # per-minute token quota is shared by the head turn, the spawned sub and the
    # synthesis turn. The independent oracle runs after, on the same day.
    q = "What is on my calendar today?"
    attempts = ask_with_retry(inst, q, secs, settle)
    ev("b_transcript.json", json.dumps({"prompt": q, "attempts": attempts}, indent=2))
    time.sleep(settle)
    day = datetime.date.today().isoformat()
    ran_tool, titles, usage, oracle_text = oracle_titles(oracle_key, model, day)
    ev(
        "b_oracle.json",
        json.dumps({"day": day, "tool_ran": ran_tool, "titles": titles, "usage": usage, "text": oracle_text}, indent=2),
    )
    joined = "\n".join(r["text"] for a in attempts for r in a["replies"])
    hits = {t: title_hit(t, joined) for t in titles}
    hits = {t: h for t, h in hits.items() if h}
    if not titles:
        ok, why = False, "oracle saw no events today: nothing stable to assert on"
    else:
        ok, why = bool(hits), f"{len(hits)} of {len(titles)} oracle events named in the replies"
    return ok, {
        "oracle_tool_ran": ran_tool,
        "oracle_events": len(titles),
        "hit_rules": list(hits.values()),
        "why": why,
        "attempts": len(attempts),
        "replies": sum(len(a["replies"]) for a in attempts),
    }


def step_c(inst, ev, secs, settle):
    out = {}
    for tag, q in (
        ("notion", "Using the Notion connector, list the titles of up to 3 pages in my Notion workspace."),
        ("slack", "Using the Slack connector, what are the names of up to 3 channels in my Slack workspace?"),
    ):
        time.sleep(settle)
        attempts = ask_with_retry(inst, q, secs, settle)
        ev(f"c_{tag}_transcript.json", json.dumps({"prompt": q, "attempts": attempts}, indent=2))
        final = attempts[-1]["replies"]
        out[tag] = {
            "attempts": len(attempts),
            "replies": len(final),
            "first": (final[0]["text"][:300] if final else ""),
        }
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--base", required=True)
    ap.add_argument("--evidence", required=True)
    ap.add_argument("--model", default="mistral-small-latest")
    ap.add_argument("--turn-secs", type=int, default=240)
    ap.add_argument("--settle", type=int, default=65, help="seconds between paid calls (per-minute provider quotas)")
    ap.add_argument("--skip-bc", action="store_true", help="configuration + probe only (no paid calls)")
    args = ap.parse_args()
    token = os.environ.get("NIMBUSD_WEB_TOKEN", "")
    oracle_key = os.environ.get("MISTRAL_ORACLE_KEY", "")
    SECRETS.extend([token, oracle_key])
    os.makedirs(args.evidence, exist_ok=True)

    def ev(name, text):
        with open(os.path.join(args.evidence, name), "w") as f:
            f.write(redact(text))

    inst = Instance(args.base, token)
    if not wait_healthy(inst):
        print("FAIL: instance never became healthy")
        return 1
    summary = {"utc": datetime.datetime.now(datetime.timezone.utc).isoformat(), "model": args.model}
    ok_a, summary["a"] = step_a(inst, ev, args.model)
    print(f"(a) probe + agreement: {'PASS' if ok_a else 'FAIL'} {json.dumps(summary['a'])}")
    ok_b = True
    if not args.skip_bc:
        if not oracle_key:
            print("FAIL: MISTRAL_ORACLE_KEY is required for (b)")
            return 1
        ok_b, summary["b"] = step_b(inst, ev, oracle_key, args.model, args.turn_secs, args.settle)
        print(f"(b) real calendar turn: {'PASS' if ok_b else 'FAIL'} {redact(json.dumps(summary['b']))}")
        summary["c"] = step_c(inst, ev, args.turn_secs, args.settle)
        print(f"(c) notion/slack (relayed, not graded): {redact(json.dumps(summary['c']))}")
    summary["verdict"] = "PASS" if (ok_a and ok_b) else "FAIL"
    ev("summary.json", json.dumps(summary, indent=2))
    print(f"E2E {summary['verdict']}")
    return 0 if (ok_a and ok_b) else 1


if __name__ == "__main__":
    sys.exit(main())
