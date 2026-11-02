#!/usr/bin/env python3
"""vn_browser_check - the hosted web page in a real browser, on a real provider.

Starts one nimbusd container and drives its own page with Chromium, the way a client
does: a turn that only starts a sub-agent, a message sent right behind it, a result that
must appear with no reload, a photo attached in the chat, the Home and Device pages, and a
walk over every control it can see, changing each one and reading the server's answer.
Writes screenshots and a JSON of what it saw; exits 0 only when every check passed.

    python3 tools/persona-rig/vn_browser_check.py --image nimbusd:dev \
        --env-file nimbus/.env --out /tmp/vn-browser --key-name MISTRAL_API_KEY

Paid: real provider turns. With --walk-only it costs nothing: no key, no turn, only the
walk. Needs the python playwright package.
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

KEY_FIELDS = {"MISTRAL_API_KEY": ("mistral", "mistKey", "mistral-medium-latest")}  # slug, field, head model


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


def photo_turn(page, inst, out: Path, results: list):
    """Defect 12: a photo through the page's own Attach control, named as a desktop names it."""
    png = out / "Screenshot 2026-11-02 at 14.33.12 green.png"
    png.write_bytes(solid_png((20, 200, 40)))
    page.set_input_files("#chatFile", str(png))
    page.wait_for_function(
        "() => /Attached Screenshot 2026-11-02 at 14\\.33\\.12 green\\.png\\. It goes/.test(document.getElementById('chatLog').innerText)",
        timeout=30000,
    )
    page.screenshot(path=str(out / "04-photo-attached.png"))
    send(page, "What color is this image? One word.")
    # The answer's own bubble: the page's "Attached green.png" must not satisfy this.
    got = page.evaluate(
        """() => new Promise(res => { const t0 = Date.now(); const tick = () => {
             const b = [...document.querySelectorAll('#chatLog > *')].map(e => e.innerText);
             const i = b.findIndex(x => /What color is this image/.test(x));
             if (i >= 0 && b.slice(i + 1).some(x => /\\bgreen\\b/i.test(x))) return res(true);
             if (Date.now() - t0 > 180000) return res(false);
             setTimeout(tick, 250); }; tick(); })"""
    )
    page.screenshot(path=str(out / "05-photo-answered.png"))
    check(results, "the head sees a photo attached in the chat", got, " | ".join(bubbles(page)[-4:]))
    _, listing = inst.get("/api/files/list?project=chat")
    check(results, "the photo left no entry in Files", '"att-' not in listing, listing)


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


# What the walk leaves alone: a key field costs a verify, and the chat box is a chat.
WALK_SKIP = {"chatInput", "directive", "devName", "connBlob", "skMd", "memq", "filesProj", "upProj"}
SET_CONTROL = """(i) => {
  const e = document.getElementById(i);
  if (!e || !e.offsetParent) return false;
  if (e.type === 'checkbox') e.checked = !e.checked;
  else if (e.tagName === 'SELECT') {
    const o = [...e.options].find((x) => x.value !== e.value && !x.disabled);
    if (!o) return false;
    e.value = o.value;
  } else if (e.type === 'number') e.value = String((+e.value || 10) + 1);
  else if (i === 'custBase') e.value = 'https://proxy.example/v1';
  else if (i === 'tgToken') e.value = '123456789:AAFakeTokenForTheWalkOnly0123456789abc';
  else e.value = 'walk1';
  e.dispatchEvent(new Event('input', {bubbles: true}));
  e.dispatchEvent(new Event('change', {bubbles: true}));
  return true;
}"""
PRESS_SAVE = """(i) => {
  const e = document.getElementById(i);
  let s = e && e.closest('details.setgroup,.sec,.subpane,.pane');
  while (s) {
    const b = [...s.querySelectorAll('button')].find(
      (x) => x.offsetParent && /^(Save|Save Changes|Apply)$/.test(x.innerText.trim()));
    if (b) { b.click(); return b.id || b.innerText; }
    s = s.parentElement && s.parentElement.closest('details.setgroup,.sec,.subpane,.pane');
  }
  return '';
}"""


def walk_controls(page, out: Path, results: list):
    """Defect 14, the class: every control a client can see answers something other than a refusal.

    Opens every page and sub-page, changes every visible field, presses its section's Save,
    and fails on any answer that says the instance does not have it, or a route it does not
    serve. Also names a sub-page that keeps a "loading" line for good.
    """
    refused: list = []

    def on_response(r):
        url = r.url.split("/api/", 1)[-1]
        if "/api/" not in r.url:
            return
        try:
            body = r.text()[:200] if r.status >= 400 else ""
        except Exception:  # a body already gone reads as empty
            body = ""
        if "Not available" in body or r.status in (404, 405, 501):
            refused.append(f"{r.request.method} /api/{url} {r.status} {body[:120]}")

    page.on("response", on_response)
    stuck, changed = [], []
    for dest in ("home", "memory", "assistant", "device"):
        page.evaluate(f"goDest('{dest}')")
        time.sleep(1)
        subs = page.eval_on_selector_all(".subtab", "els => els.filter(e => e.offsetParent).map(e => e.dataset.sp)")
        for sp in subs or [""]:
            if sp:
                page.evaluate(f"document.querySelector('.subtab[data-sp={sp}]').click()")
                time.sleep(1)
            page.evaluate("document.querySelectorAll('details').forEach(d => { d.open = true; })")
            time.sleep(1.5)
            loading = page.evaluate(
                "[...document.querySelectorAll('div,p')].filter(e => e.offsetParent && !e.children.length && "
                "/^loading/i.test(e.innerText.trim())).map(e => e.id || e.innerText)"
            )
            if loading:
                stuck.append(f"{dest}/{sp}: {loading}")
            ids = page.evaluate(
                "[...document.querySelectorAll('input,select,textarea')]"
                ".filter(e => e.offsetParent && e.id && e.type !== 'file').map(e => e.id)"
            )
            for i in ids:
                if i in WALK_SKIP or i.startswith(("key_", "cc_")):
                    continue
                if page.evaluate(SET_CONTROL, i):
                    changed.append(f"{dest}/{sp}:{i}")
                    time.sleep(0.25)
                    page.evaluate(PRESS_SAVE, i)
                    time.sleep(0.45)
            page.screenshot(path=str(out / f"walk-{dest}-{sp or 'page'}.png"), full_page=True)
    page.remove_listener("response", on_response)
    (out / "walk.json").write_text(json.dumps({"changed": changed, "refused": refused, "stuck": stuck}, indent=1))
    check(results, f"the walk changed real controls ({len(changed)})", len(changed) > 20, str(changed)[:600])
    check(results, "no control a client can see answers a refusal", not refused, "; ".join(sorted(set(refused))))
    check(results, "no sub-page stays on a loading line", not stuck, "; ".join(stuck))


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
    back = wait_text(page, r"note\.txt", 60)
    check(results, "the conversation is back after a reload", back, " | ".join(bubbles(page)[-4:]))
    time.sleep(1)
    page.evaluate("document.querySelectorAll('#chatLog details').forEach(d => { d.open = true; })")
    opened = page.locator("#chatLog details").count()
    check(results, "the chat shows the turn's tool disclosure", opened > 0, f"{opened} disclosures")
    page.screenshot(path=str(out / "09-chat-with-tool-trace.png"), full_page=True)
    _, body = inst.get("/api/mem/episodic?limit=50")
    check(results, "the tool call left its row", "tool_output" in body)


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--image", required=True)
    ap.add_argument("--env-file", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--key-name", default="MISTRAL_API_KEY")
    ap.add_argument("--walk-only", action="store_true", help="free: no key and no turn, only the control walk")
    args = ap.parse_args()
    out = Path(args.out)
    out.mkdir(parents=True, exist_ok=True)
    slug, field, model = KEY_FIELDS[args.key_name]
    env_values = {} if args.walk_only else read_env_file(args.env_file)
    token = secrets.token_urlsafe(24)
    backend = DockerBackend(args.image, DOCKER)
    name = f"nimbusrig-vnbrowser-{uuid.uuid4().hex[:6]}"
    results: list = []
    try:
        port = backend.start(name, {"NIMBUSD_WEB_TOKEN": token})
        inst = Instance(f"http://127.0.0.1:{port}", token)
        inst.wait_ready()
        if not args.walk_only:
            inst.call("POST", "/api/orch", form={field: env_values[args.key_name]})
            inst.call("POST", "/api/orch", form={f"orchM_{slug}": model})
            inst.call("POST", "/api/orch", form={f"subM_{slug}": "mistral-small-latest"})
            inst.call("POST", "/api/orch", form={"orchHost": slug})
        with sync_playwright() as p:
            br = p.chromium.launch()
            ctx = br.new_context(viewport={"width": 1100, "height": 900}, extra_http_headers={"X-Nimbus-Token": token})
            page = ctx.new_page()
            errors: list = []
            page.on("pageerror", lambda e: errors.append(str(e)))
            page.goto(f"http://127.0.0.1:{port}/")
            page.wait_for_load_state("load")
            if not args.walk_only:
                page.evaluate("goDest('chat')")
                time.sleep(1)
                spawn_turns(page, out, results)
                photo_turn(page, inst, out, results)
                trace_disclosure(page, inst, out, results)
                routines_page(page, inst, out, results)
            dead_controls(page, out, results)
            walk_controls(page, out, results)
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
