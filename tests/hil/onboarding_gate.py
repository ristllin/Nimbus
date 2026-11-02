#!/usr/bin/env python3
"""The onboarding gate (bench row H27): a factory-fresh unit to a first answer, every step asserted.

    onboarding_gate.py --port P --mac MAC --out DIR --env-file F                    # a test image, driven by console
    onboarding_gate.py --port P --mac MAC --out DIR --env-file F --factory-reset    # reset the unit first
    onboarding_gate.py --port P --mac MAC --out DIR --env-file F --wipe-build B     # erase and full-flash B first
    onboarding_gate.py --no-console --ip IP --token-file T --out DIR --env-file F   # a production image, by hand

On a test image the screen is driven through the console (RENDER?, MENU?, TAP, CALGATE?), so the first-run screens
are checked without a finger. A production image has no console, so those steps become precise questions for the
person at the bench and the rest runs over the web API the wizard itself uses. The Wi-Fi password and the provider key
come from the env file by name and are never logged.

The gate stops at the first step that fails, because a later step on a half-set-up unit proves nothing: the steps
after it are listed as not run and the gate is FAIL. A step that cannot run is BLOCKED, never PASS. Evidence: one
JSON file per step, gate.json with the whole table, and console.log.

The fresh-context walkthrough of docs/quick-start that row H27 also asks for is a person's or an agent's reading of
the docs, not this script. Run it after this gate passes; the gate proves the path works, the walkthrough proves the
docs lead a stranger along it.
"""

from __future__ import annotations

import argparse
import dataclasses
import json
import os
import re
import subprocess
import sys
import time
from typing import Any, Callable, Dict, List, Optional

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import bench_driver as bd
import port_guard

WIFIAP_RE = re.compile(r"WIFIAP\?\s+ssid=(?P<ssid>\S+)\s+ip=\S+\s+up=(?P<up>\d)\s+sta=(?P<sta>\d)\s+onboarded=(?P<ob>\d)")
STATUS_RE = re.compile(r"STATUS .*?mode=(?P<mode>\d+)\s+wifi=(?P<wifi>\d)\s+ip=(?P<ip>\S+)")
RENDER_RE = re.compile(r"RENDER screen=(?P<screen>\d+)")
WEBTOK_RE = re.compile(r"WEBTOK ([0-9a-fA-F]{24})")
MENU_RE = re.compile(r"MENU open=(?P<open>\d)")
CALGATE_RE = re.compile(r"CALGATE active=(?P<active>\d)\s+kind=(?P<kind>\w+)")
# ScreenId order (lib/core/include/nimbus/attention.h, device.py SCREEN_NAMES).
SCREEN_MENU = 3
SCREENS_AT_REST = {0: "StatusIdle", 7: "SetupInfo", 8: "IdleArt", 13: "Screensaver"}
GEAR_TAP = (300, 10)  # the gear, top-right on the landscape panel
BACK_TAP = (20, 22)  # the header Back or Home target on every screen that draws one
DEFAULT_CEILING_CENTS = 2000  # the owner's monthly default for a new unit
FIRST_MESSAGE = "Reply with exactly one word: ok"
KEY_FIELD = {"anthropic": "antKey", "mistral": "mistKey", "openai": "oaiKey", "zai": "zaiKey"}
KEY_ENV = {"anthropic": "ANTHROPIC_API_KEY", "mistral": "MISTRAL_API_KEY", "openai": "OPENAI_API_KEY", "zai": "Z_AI_TOKEN"}
WIFI_ENV = ("NIMBUS_TEST_STA_SSID", "NIMBUS_TEST_STA_PASS")


class StepFailed(Exception):
    """A step's assertion did not hold: the finding is the message."""


class StepBlocked(Exception):
    """A step could not run: nothing was measured."""


def need(condition: Any, message: str) -> None:
    if not condition:
        raise StepFailed(message)


@dataclasses.dataclass
class Gate:
    """What the steps share. ``ask`` puts a yes-or-no question to the person at the bench."""

    args: argparse.Namespace
    ev: bd.Evidence
    api: bd.Api
    console: Any = None
    ask: Callable[[str], Optional[bool]] = lambda question: None
    open_console: Callable[[], Any] = lambda: None
    run: Callable[..., Any] = subprocess.run
    settle_s: float = 3.0

    def poll(self, check: Callable[[], Any], timeout: float, every: Optional[float] = None) -> Any:
        """Call ``check`` until it returns something true or the time is up (then its last value)."""
        end = time.time() + timeout
        value = check()
        while not value and time.time() < end:
            time.sleep(self.settle_s if every is None else every)
            value = check()
        return value

    def human(self, question: str) -> str:
        answer = self.ask(question)
        if answer is None:
            raise StepBlocked(f"needs a person at the bench: {question}")
        need(answer, f"the person answered no: {question}")
        return "yes"


# ---- the steps --------------------------------------------------------------------------------------


def step_identity(g: Gate) -> Dict[str, Any]:
    a = g.args
    port_guard.check_mac(a.mac, "onboarding_gate")
    if a.no_console:
        return {"mac": a.mac, "console": False}
    row = port_guard.check_port(a.port, "onboarding_gate")
    need(row and port_guard.guard_norm(row.serial) == port_guard.guard_norm(a.mac), f"{a.port} does not report {a.mac}")
    return {"port": a.port, "mac": a.mac, "usbSerial": row.serial}


def esptool_argvs(esptool: List[str], port: str, build: str, boot_app0: str) -> List[List[str]]:
    """The factory wipe: read the MAC, erase, full flash. Every call stays off the reset lines (the bench's form)."""
    base = esptool + ["--chip", "esp32s3", "--port", port, "--before", "no_reset", "--after", "no_reset"]
    images = ["0x0", f"{build}/bootloader.bin", "0x8000", f"{build}/partitions.bin", "0xe000", boot_app0]
    return [
        esptool + ["--chip", "esp32s3", "--port", port, "--before", "default_reset", "--after", "no_reset", "read-mac"],
        base + ["erase-flash"],
        base + ["write-flash"] + images + ["0x10000", f"{build}/firmware.bin"],
    ]


def step_factory_state(g: Gate) -> Dict[str, Any]:
    """Bring the unit to factory-fresh if asked to, then open the console."""
    a = g.args
    how = "as found"
    if a.wipe_build:
        argvs = esptool_argvs(a.esptool.split(), a.port, a.wipe_build, a.boot_app0)
        first = g.run(argvs[0], capture_output=True, text=True, timeout=60)
        port_guard.check_mac(_mac_in(first.stdout), "onboarding_gate")
        need(port_guard.guard_norm(a.mac) in port_guard.guard_norm(first.stdout), f"read-mac did not report {a.mac}")
        for argv in argvs[1:]:
            need(g.run(argv, capture_output=True, text=True, timeout=300).returncode == 0, f"failed: {argv[-1]}")
        how = "erased and flashed " + a.wipe_build
    if not a.no_console:
        g.console = g.open_console()
    if a.factory_reset and not a.no_console:
        need(g.console.cmd("FACTRESET", "FACTRESET ok", 8.0), "FACTRESET was not acknowledged")
        need(g.console.wait("READY", 60.0), "no READY after the factory reset")
        how = "FACTRESET"
    return {"how": how}


def _mac_in(text: str) -> str:
    match = re.search(r"([0-9a-fA-F]{2}(?::[0-9a-fA-F]{2}){5})", text or "")
    return match.group(1) if match else ""


def step_is_fresh(g: Gate) -> Dict[str, Any]:
    if g.args.no_console:
        return {"answer": g.human("Is the unit showing its first-run setup screen with a setup network name?")}
    need(g.poll(lambda: g.console.cmd("PING", "PONG", 3.0), 60.0), "the console never answered PING")
    reply = g.console.cmd("WIFIAP?", "WIFIAP?", 6.0)
    match = WIFIAP_RE.search(reply)
    need(match, f"no WIFIAP? answer: {reply!r}")
    need(match["ob"] == "0", "the unit says it is already onboarded, so this is not a fresh unit")
    need(match["sta"] == "0", "the unit is already on Wi-Fi, so this is not a fresh unit")
    need(match["ssid"].endswith("-setup"), f"the setup network is named {match['ssid']!r}")
    need(match["up"] == "1", "the setup network is not up on a fresh unit")
    return {"setupNetwork": match["ssid"]}


def _screen(g: Gate) -> int:
    match = RENDER_RE.search(g.console.cmd("RENDER?", "RENDER screen=", 6.0))
    need(match, "RENDER? did not answer")
    return int(match["screen"])


def step_first_screen(g: Gate) -> Dict[str, Any]:
    """The glass on first boot, through the console: not gated on a capacitive panel, resting on a known screen, the
    menu opens from the gear and closes from the header."""
    if g.args.no_console:
        return {"answer": g.human("Does a tap on the gear open the menu, and does Back close it?")}
    gate = CALGATE_RE.search(g.console.cmd("CALGATE?", "CALGATE", 6.0))
    need(gate, "CALGATE? did not answer")
    need(not (gate["active"] == "1" and gate["kind"] == "cap"), "a capacitive panel must never sit behind the touch gate")
    first = _screen(g)
    need(first in SCREENS_AT_REST, f"the first screen is id {first}, not a resting screen")
    g.console.cmd(f"TAP {GEAR_TAP[0]} {GEAR_TAP[1]}", "TAP", 4.0)
    menu = g.poll(lambda: MENU_RE.search(g.console.cmd("MENU?", "MENU open=", 4.0) or "MENU open=0").group("open") == "1", 6.0, 0.3)
    need(menu, "a tap on the gear did not open the menu")
    need(_screen(g) == SCREEN_MENU, "the menu is open but RENDER? does not show it")
    g.console.cmd(f"TAP {BACK_TAP[0]} {BACK_TAP[1]}", "TAP", 4.0)
    closed = g.poll(lambda: MENU_RE.search(g.console.cmd("MENU?", "MENU open=", 4.0) or "MENU open=1").group("open") == "0", 6.0, 0.3)
    need(closed, "Back did not close the menu")
    return {"firstScreen": SCREENS_AT_REST[first], "touch": gate["kind"]}


def step_wifi(g: Gate) -> Dict[str, Any]:
    if g.args.no_console:
        return {"answer": g.human("Did the setup wizard join Wi-Fi? (Use the phone on the setup network.)")}
    ssid = bd.read_env_value(g.args.env_file, WIFI_ENV[0])
    password = bd.read_env_value(g.args.env_file, WIFI_ENV[1])
    if not (ssid and password):
        raise StepBlocked(f"{WIFI_ENV[0]} and {WIFI_ENV[1]} are not in the env file")
    g.console.send(f"WIFI {ssid}|{password}", secret=True)

    def joined() -> Optional[str]:
        match = STATUS_RE.search(g.console.cmd("STATUS", "STATUS ", 6.0))
        return match["ip"] if match and match["wifi"] == "1" and match["ip"] != "0.0.0.0" else None

    ip = g.poll(joined, 90.0)
    need(ip, "the unit did not join Wi-Fi within 90 s")
    g.api.ip = g.args.ip or ip
    return {"ip": ip}


def step_token(g: Gate) -> Dict[str, Any]:
    if g.args.no_console:
        return {"token": "from the token file"}
    match = WEBTOK_RE.search(g.console.cmd("WEBTOK?", "WEBTOK", 8.0))
    need(match, "WEBTOK? did not answer with a token")
    g.api.set_token(match.group(1))
    if g.args.token_out:
        with open(g.args.token_out, "w", encoding="utf-8") as f:
            f.write(match.group(1))
    return {"token": "read, not recorded here"}


def _state(g: Gate) -> Dict[str, Any]:
    return g.poll(g.api.state, 60.0) or {}


def step_gate_holds(g: Gate) -> Dict[str, Any]:
    """The wizard's gate is shut on a fresh unit and says why when it is pushed."""
    state = _state(g)
    need(state, "the web API did not answer")
    need(state.get("needsOnboarding") is True, "a fresh unit must report needsOnboarding")
    need(state.get("provVerified") is False, "a fresh unit must not report a verified provider")
    pushed = g.api.post_form("/api/onboard/complete", {})
    need(pushed.code == 409 and "no verified provider" in pushed.text, f"finishing with no provider answered {pushed.code}")
    return {"pushed": pushed.code, "said": pushed.text[:80]}


def step_provider(g: Gate) -> Dict[str, Any]:
    provider = g.args.provider
    key = bd.read_env_value(g.args.env_file, KEY_ENV[provider])
    if not key:
        raise StepBlocked(f"{KEY_ENV[provider]} is not in the env file")
    saved = g.api.post_form("/api/orch", {KEY_FIELD[provider]: key}, 30.0)
    need(saved.code == 200, f"saving the key answered {saved.code}")
    fields = {"orchHost": provider}
    if g.args.model:
        fields["orchModel"] = g.args.model
    need(g.api.post_form("/api/orch", fields).code == 200, "choosing the head was refused")
    verify = g.api.post_form("/api/verify", {"provider": provider}, 60.0)
    need(verify.code in (200, 202), f"verify answered {verify.code}: {verify.text[:120]}")
    need(g.poll(lambda: g.api.state().get("provVerified") is True, 90.0), f"{provider} never verified")
    return {"provider": provider, "verified": True}


def step_complete(g: Gate) -> Dict[str, Any]:
    done = g.api.post_form("/api/onboard/complete", {})
    need(done.code == 200, f"finishing answered {done.code}: {done.text[:120]}")
    need(g.api.state().get("needsOnboarding") is False, "the wizard's gate did not clear")
    return {"finished": done.text[:80]}


def step_defaults(g: Gate) -> Dict[str, Any]:
    state = g.api.state()
    spend = state.get("spend") or {}
    if "limitCents" not in spend:
        raise StepBlocked("/api/state carries no spend ceiling to read")
    need(spend["limitCents"] == DEFAULT_CEILING_CENTS, f"the default ceiling is {spend['limitCents']} cents")
    need(not spend.get("frozen"), "a new unit is already paused")
    return {"limitCents": spend["limitCents"], "fsMounted": state.get("fsMounted")}


def step_first_answer(g: Gate) -> Dict[str, Any]:
    result = bd.chat_turn(g.api, FIRST_MESSAGE, 120.0, g.settle_s)
    need(result.ok, f"the first message was not answered: {result.error}")
    words = result.reply.strip().lower()
    need("ok" in words, f"the first answer was {result.reply[:120]!r}")
    log = g.api.get("/api/errlog", 30.0)
    g.ev.write("errlog-first-answer.txt", log.text)
    need("turn end: ok" in log.text, "the durable log has no finished turn for the first answer")
    return {"reply": result.reply[:120], "seconds": round(result.seconds, 1)}


def step_screen_after(g: Gate) -> Dict[str, Any]:
    if g.args.no_console:
        return {"answer": g.human("Is the unit on its normal idle screen now?")}
    screen = g.poll(lambda: _screen(g) in SCREENS_AT_REST, 15.0, 1.0)
    need(screen, "the screen is not at rest after the first answer")
    return {"screen": _screen(g)}


def step_restart(g: Gate) -> Dict[str, Any]:
    """Setup survives a restart: the wizard stays gone, the provider stays verified, no crash on the way."""
    before = g.api.state()
    if g.args.no_console:
        g.human("Please power-cycle the unit now, and answer yes when its screen is back.")
    else:
        need(g.console.cmd("REBOOT", "REBOOTING", 8.0), "REBOOT was not acknowledged")
        g.poll(lambda: g.console.cmd("PING", "PONG", 3.0), 60.0)
    after = g.poll(lambda: (lambda s: s if s.get("boot", {}).get("n") != before.get("boot", {}).get("n") else {})(g.api.state()), 90.0)
    need(after, "the unit did not come back with a new boot count")
    need(after.get("needsOnboarding") is False, "the wizard came back after a restart")
    need(after.get("provVerified") is True, "the provider did not stay verified across a restart")
    crash = (after.get("boot") or {}).get("crash") or {}
    need(not crash.get("thisReset"), f"the restart was a crash: {crash.get('why')}")
    return {"bootBefore": before.get("boot", {}).get("n"), "bootAfter": after["boot"]["n"]}


STEPS = (
    ("identity", "the board is the one named, and not a forbidden one", step_identity),
    ("factory-state", "the unit is brought to factory-fresh", step_factory_state),
    ("is-fresh", "the unit reads factory-fresh", step_is_fresh),
    ("first-screen", "the first screens work", step_first_screen),
    ("wifi", "the unit joins Wi-Fi", step_wifi),
    ("token", "the web sign-in token is read", step_token),
    ("gate-holds", "setup cannot finish before a provider works", step_gate_holds),
    ("provider", "a provider key is saved and verifies", step_provider),
    ("complete", "setup finishes", step_complete),
    ("defaults", "the defaults a new client gets are right", step_defaults),
    ("first-answer", "the first message gets a real answer", step_first_answer),
    ("screen-after", "the screen is at rest after it", step_screen_after),
    ("restart", "setup survives a restart", step_restart),
)


def _run_step(g: Gate, name: str, fn) -> Dict[str, Any]:
    started = time.time()
    row: Dict[str, Any] = {"step": name, "at": bd.utc_iso(started)}
    try:
        row.update(grade="PASS", detail=fn(g))
    except StepBlocked as exc:
        row.update(grade="BLOCKED", detail=str(exc))
    except (StepFailed, port_guard.ForbiddenDevice) as exc:
        row.update(grade="FAIL", detail=str(exc))
    row["seconds"] = round(time.time() - started, 1)
    return row


def run_gate(g: Gate, steps=STEPS) -> Dict[str, Any]:
    """Run the steps in order, stopping at the first that does not pass. Writes the evidence as it goes."""
    rows: List[Dict[str, Any]] = []
    for index, (name, title, fn) in enumerate(steps, 1):
        row = _run_step(g, name, fn)
        row["title"] = title
        rows.append(row)
        g.ev.write(f"step-{index:02d}-{name}.json", row)
        g.ev.note(f"{row['grade']} {name}: {title}: {row['detail']}", "gate.log")
        if row["grade"] != "PASS":
            rows += [{"step": n, "title": t, "grade": "NOT RUN"} for n, t, _ in steps[index:]]
            break
    grade = bd.worst_grade([r["grade"] for r in rows if r["grade"] != "NOT RUN"])
    verdict = {"grade": grade, "steps": rows, "at": bd.utc_iso()}
    g.ev.write("gate.json", verdict)
    return verdict


# ---- the command line --------------------------------------------------------------------------


def ask_stdin(question: str) -> Optional[bool]:
    if not sys.stdin.isatty():
        return None
    return input(f"{question} [y/n] ").strip().lower().startswith("y")


def parse_args(argv=None) -> argparse.Namespace:
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--port")
    ap.add_argument("--mac", default="")
    ap.add_argument("--out", required=True)
    ap.add_argument("--env-file", required=True, help="read by exact key name, never printed")
    ap.add_argument("--provider", default="anthropic", choices=sorted(KEY_ENV))
    ap.add_argument("--model", default="", help="the head model; the unit's default when empty")
    ap.add_argument("--no-console", action="store_true", help="a production image: the person answers the screen steps")
    ap.add_argument("--ip", default="")
    ap.add_argument("--token-file", help="the token, with --no-console")
    ap.add_argument("--token-out", help="where to save the token read from the console")
    ap.add_argument("--factory-reset", action="store_true", help="send FACTRESET first (erases the unit's settings)")
    ap.add_argument("--wipe-build", help="a build folder to erase and full-flash first")
    ap.add_argument("--boot-app0", default=os.path.expanduser("~/.platformio/packages/framework-arduinoespressif32/tools/partitions/boot_app0.bin"))
    ap.add_argument("--esptool", default=os.path.expanduser("~/.platformio/penv/bin/esptool"))
    args = ap.parse_args(argv)
    if args.no_console and not (args.ip and args.token_file):
        ap.error("--no-console needs --ip and --token-file")
    if not args.no_console and not (args.port and args.mac):
        ap.error("a console run needs --port and --mac")
    return args


def main(argv=None) -> int:
    args = parse_args(argv)
    token = bd.read_token(args.token_file) if args.token_file else "unset"
    api = bd.Api(args.ip or "0.0.0.0", token)
    ev = bd.Evidence(args.out, api.scrub)
    gate = Gate(
        args=args,
        ev=ev,
        api=api,
        ask=ask_stdin,
        open_console=lambda: bd.Console(args.port, ev.path("console.log"), "onboarding_gate"),
    )
    try:
        verdict = run_gate(gate)
    finally:
        if gate.console:
            gate.console.close()
    print(json.dumps({"grade": verdict["grade"], "steps": [(r["step"], r["grade"]) for r in verdict["steps"]]}))
    return 0 if verdict["grade"] == "PASS" else (port_guard.REFUSED_EXIT if "refused" in json.dumps(verdict["steps"][0]) else 1)


if __name__ == "__main__":
    raise SystemExit(main())
