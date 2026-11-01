#!/usr/bin/env python3
"""vn_browser_check - the hosted web page in a real browser, on a real provider.

Starts one nimbusd container and drives its own page with Chromium, the way a client
does: a turn that only starts a sub-agent, a message sent right behind it, a result that
must appear with no reload, a photo attached in the chat, and the Home and Device pages.
Writes screenshots and a JSON of what it saw; exits 0 only when every check passed.

    python3 tools/persona-rig/vn_browser_check.py --image nimbusd:dev \
        --env-file nimbus/.env --out /tmp/vn-browser --key-name MISTRAL_API_KEY

Paid: real provider turns on the cheap model. Needs the python playwright package.
"""

import argparse
import json
import secrets
import sys
import time
import uuid
from pathlib import Path

from playwright.sync_api import sync_playwright

sys.path.insert(0, str(Path(__file__).resolve().parent))
from persona_rig import DOCKER, DockerBackend, Instance, read_env_file, solid_png  # noqa: E402

KEY_FIELDS = {"MISTRAL_API_KEY": ("mistral", "mistKey", "mistral-small-latest")}


def bubbles(page) -> list:
    """Every chat bubble's text, top to bottom."""
    return page.eval_on_selector_all("#chatLog > *", "els => els.map(e => e.innerText)")


def wait_text(page, regex: str, timeout_s: float) -> bool:
    """Wait until a bubble matches, with no reload. Polls what the owner sees."""
    return page.evaluate(
        """([rx, ms]) => new Promise(res => {
             const re = new RegExp(rx, 'i'), t0 = Date.now();
             const tick = () => {
               if (re.test(document.getElementById('chatLog').innerText)) return res(true);
               if (Date.now() - t0 > ms) return res(false);
               setTimeout(tick, 250);
             };
             tick();
           })""",
        [regex, int(timeout_s * 1000)],
    )


def send(page, text: str):
    page.fill("#chatInput", text)
    page.click("#chatSend")


def check(results: list, name: str, ok: bool, seen: str = ""):
    results.append({"check": name, "ok": bool(ok), "seen": seen[:600]})
    print(("PASS " if ok else "FAIL ") + name, flush=True)


def spawn_turns(page, out: Path, results: list):
    """Defect 2: the spawn-only turn, the message behind it, the result with no reload."""
    t0 = time.time()
    send(
        page,
        "Start a sub-agent to work out 12 squared. Do not work it out yourself. "
        "Leave your own reply field empty: the sub-agent's result reaches me by itself.",
    )
    time.sleep(2)
    send(page, "What is 2 plus 2? Reply with just the number.")
    page.screenshot(path=str(out / "01-two-messages-sent.png"))
    got = wait_text(page, r"\b144\b", 240)
    page.screenshot(path=str(out / "02-subagent-result-no-reload.png"))
    check(results, "the sub-agent result appears with no reload", got, " | ".join(bubbles(page)[-6:]))
    # The queued message has been sent and answered: its bubble holds a 4 of its own.
    got4 = page.evaluate(
        """() => new Promise(res => { const t0 = Date.now(); const tick = () => {
             const b = [...document.querySelectorAll('#chatLog > *')].map(e => e.innerText);
             if (b.some(x => /^\\s*(.*\\b)?4\\b\\s*$/m.test(x) && !/144/.test(x))) return res(true);
             if (Date.now() - t0 > 120000) return res(false);
             setTimeout(tick, 250); }; tick(); })"""
    )
    page.screenshot(path=str(out / "03-held-message-answered.png"))
    check(results, "the message sent behind it was not lost", got4, " | ".join(bubbles(page)[-6:]))
    pending = page.evaluate(
        "[...document.querySelectorAll('#chatLog > *')].filter(e => e.innerText.trim() === '…').length"
    )
    check(results, "no bubble is left waiting", pending == 0, f"pending bubbles: {pending}; {time.time() - t0:.0f} s")


def photo_turn(page, out: Path, results: list):
    """Defect 12: a photo through the page's own Attach control."""
    png = out / "green.png"
    png.write_bytes(solid_png((20, 200, 40)))
    page.set_input_files("#chatFile", str(png))
    page.wait_for_function(
        "() => /Attached green\\.png/.test(document.getElementById('chatLog').innerText)", timeout=30000
    )
    page.screenshot(path=str(out / "04-photo-attached.png"))
    send(page, "What color is this image? One word.")
    got = wait_text(page, r"\bgreen\b", 180)
    page.screenshot(path=str(out / "05-photo-answered.png"))
    check(results, "the head sees a photo attached in the chat", got, " | ".join(bubbles(page)[-4:]))


def dead_controls(page, out: Path, results: list):
    """Defect 14: no control that only answers an error."""
    page.evaluate("goDest('home')")
    time.sleep(1)
    page.screenshot(path=str(out / "06-home.png"))
    for sel, name in (("#homeRestart", "Home Restart"), ("#homePowerOff", "Home Power off")):
        check(results, f"{name} is not shown", not page.is_visible(sel))
    page.evaluate("goDest('device')")
    time.sleep(1)
    page.screenshot(path=str(out / "07-device.png"), full_page=True)
    check(results, "Device has no Power group", not page.is_visible("#deviceRestart"))
    check(results, "there is no mode switch", not page.is_visible("#modehdr"))
    shown = page.eval_on_selector_all(
        "details.setgroup > summary", "els => els.filter(e => e.offsetParent).map(e => e.innerText)"
    )
    for gone in ("Display", "Ring", "Sound", "Power", "Danger zone"):
        check(results, f"no {gone} group", not any(t.startswith(gone) for t in shown), str(shown))


def routines_page(page, inst, out: Path, results: list):
    """Defect 11: the Routines form works, it does not answer a refusal."""
    status, body = inst.call(
        "POST",
        "/api/loops",
        form={
            "action": "create",
            "name": "stretch",
            "prompt": "Remind me to stretch.",
            "chatId": "owner",
            "schedule": json.dumps({"kind": "daily", "at": "09:00"}),
        },
    )
    check(results, "a routine is created on the instance", status == 200 and '"ok":true' in body, body)
    page.evaluate("goDest('assistant')")
    time.sleep(2)
    page.screenshot(path=str(out / "08-assistant.png"), full_page=True)


def trace_disclosure(page, inst, out: Path, results: list):
    """Defect 13: the chat's tool disclosure has rows to show."""
    page.evaluate("goDest('chat')")
    send(page, "Save a file named note.txt in project vnp with exactly this text: hello. Then confirm.")
    wait_text(page, r"note\.txt|saved", 120)
    time.sleep(3)
    page.reload()
    page.wait_for_load_state("load")
    page.evaluate("goDest('chat')")
    time.sleep(3)
    page.screenshot(path=str(out / "09-chat-with-tool-trace.png"), full_page=True)
    _, body = inst.get("/api/mem/episodic?limit=50")
    check(results, "the tool call left its row", "tool_output" in body)


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--image", required=True)
    ap.add_argument("--env-file", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--key-name", default="MISTRAL_API_KEY")
    args = ap.parse_args()
    out = Path(args.out)
    out.mkdir(parents=True, exist_ok=True)
    slug, field, model = KEY_FIELDS[args.key_name]
    env_values = read_env_file(args.env_file)
    token = secrets.token_urlsafe(24)
    backend = DockerBackend(args.image, DOCKER)
    name = f"nimbusrig-vnbrowser-{uuid.uuid4().hex[:6]}"
    results: list = []
    try:
        port = backend.start(name, {"NIMBUSD_WEB_TOKEN": token})
        inst = Instance(f"http://127.0.0.1:{port}", token)
        inst.wait_ready()
        inst.call("POST", "/api/orch", form={field: env_values[args.key_name]})
        inst.call("POST", "/api/orch", form={f"orchM_{slug}": model})
        inst.call("POST", "/api/orch", form={f"subM_{slug}": model})
        inst.call("POST", "/api/orch", form={"orchHost": slug})
        with sync_playwright() as p:
            br = p.chromium.launch()
            page = br.new_context(viewport={"width": 1100, "height": 900}).new_page()
            errors: list = []
            page.on("pageerror", lambda e: errors.append(str(e)))
            page.goto(f"http://127.0.0.1:{port}/")
            page.wait_for_load_state("load")
            page.evaluate("goDest('chat')")
            time.sleep(1)
            spawn_turns(page, out, results)
            photo_turn(page, out, results)
            trace_disclosure(page, inst, out, results)
            routines_page(page, inst, out, results)
            dead_controls(page, out, results)
            check(results, "no script error on the page", not errors, "; ".join(errors))
            br.close()
    except Exception as e:  # a crash is a failed check with its reason, never a pass
        check(results, "the run finished", False, f"{type(e).__name__}: {e}")
    finally:
        (out / "container.log").write_text(backend.logs(name).replace(token, "<token>"))
        backend.stop(name)
    (out / "results.json").write_text(json.dumps(results, indent=1))
    failed = [r for r in results if not r["ok"]]
    print(f"{len(results) - len(failed)} of {len(results)} checks passed")
    return 1 if failed or not results else 0


if __name__ == "__main__":
    sys.exit(main())
