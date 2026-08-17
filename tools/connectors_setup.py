#!/usr/bin/env python3
"""connectors_setup - push connector configs from .env to a Nimbus board.

Reads the repo-root .env (GITIGNORED - connector secrets) and upserts each
connector that has its credential present, via the token-gated
POST /api/connectors `patch` path (secret-preserving upsert).

Usage:
  python3 tools/connectors_setup.py            # configure all with creds present
  python3 tools/connectors_setup.py --list     # just show current board state
  python3 tools/connectors_setup.py --only github,google,mistral

No secret is ever printed. Safe to re-run (idempotent upsert).
"""

import argparse
import json
import sys
import urllib.parse
import urllib.request
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent


def load_env() -> dict:
    env = {}
    p = ROOT / ".env"
    if not p.exists():
        sys.exit(".env not found at repo root - create it first (see tools/connectors_setup docstring).")
    for line in p.read_text().splitlines():
        line = line.strip()
        if not line or line.startswith("#") or "=" not in line:
            continue
        k, v = line.split("=", 1)
        env[k.strip()] = v.strip()
    return env


def api(ip: str, token: str, method: str, params: dict | None = None):
    url = f"http://{ip}/api/connectors?t={token}"
    data = urllib.parse.urlencode(params).encode() if params else None
    req = urllib.request.Request(url, data=data, method=method)
    if data:
        req.add_header("Content-Type", "application/x-www-form-urlencoded")
    with urllib.request.urlopen(req, timeout=10) as r:
        return json.loads(r.read().decode())


def patch(ip, token, obj) -> bool:
    r = api(ip, token, "POST", {"patch": json.dumps(obj)})
    return bool(r.get("ok"))


def build_connectors(env: dict) -> list[tuple[str, dict]]:
    """(label, patch-object) for each connector whose credential is present."""
    out = []
    if env.get("GITHUB_PAT"):
        out.append(
            (
                "github",
                {
                    "name": "github",
                    "type": "github",
                    "prov": "openai",
                    "kind": "mcp",
                    "url": env.get("GITHUB_MCP_URL", "https://api.githubcopilot.com/mcp/"),
                    "tok": env["GITHUB_PAT"],
                    "en": 1,
                },
            )
        )
    if env.get("GOOGLE_OAUTH_REFRESH_TOKEN"):
        # OpenAI first-party Gmail connector, authorized via the T3 OAuth broker.
        oauth = {
            "rurl": env.get("GOOGLE_TOKEN_URL", "https://oauth2.googleapis.com/token"),
            "cid": env.get("GOOGLE_OAUTH_CLIENT_ID", ""),
            "sec": env.get("GOOGLE_OAUTH_CLIENT_SECRET", ""),
            "rtok": env["GOOGLE_OAUTH_REFRESH_TOKEN"],
        }
        out.append(
            (
                "gmail",
                {
                    "name": "gmail",
                    "type": "gmail",
                    "prov": "openai",
                    "kind": "connector",
                    "cid": "connector_gmail",
                    "oauth": oauth,
                    "en": 1,
                },
            )
        )
    if env.get("MISTRAL_STUDIO_CONNECTOR_NAME"):
        nm = env["MISTRAL_STUDIO_CONNECTOR_NAME"]
        out.append(
            (
                f"mistral:{nm}",
                {
                    "name": nm,
                    "type": nm,
                    "prov": "mistral",
                    "kind": "builtin",
                    "en": 1,
                },
            )
        )
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--list", action="store_true", help="show board state only")
    ap.add_argument("--only", default="", help="comma list of labels to configure")
    args = ap.parse_args()

    env = load_env()
    ip = env.get("NIMBUS_BOARD1_IP")
    token = env.get("NIMBUS_BOARD1_TOKEN")
    if not ip or not token:
        sys.exit("Set NIMBUS_BOARD1_IP and NIMBUS_BOARD1_TOKEN in .env")

    state = api(ip, token, "GET")
    print(f"board {ip}  host={state.get('host')}  keyed={state.get('keyed')}")
    print("configured:", [(c["name"], c["prov"], c["en"], c["hasTok"]) for c in state.get("configured", [])])
    if args.list:
        return

    only = {s.strip() for s in args.only.split(",") if s.strip()}
    for label, obj in build_connectors(env):
        if only and label not in only:
            continue
        ok = patch(ip, token, obj)
        print(f"  {'OK ' if ok else 'FAIL'} {label} ({obj['prov']}/{obj['kind']})")
    print("done - secrets stay on the device (write-only); re-run anytime.")


if __name__ == "__main__":
    main()
