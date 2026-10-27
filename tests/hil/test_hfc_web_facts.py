"""Lane HFC (v4.6.1, CUM-821 + CUM-822) - public web facts go to web search on an open
provider, and a paused provider is shown as paused.

Production, 27 Oct 2026 (Lumi on v4.6.0): OpenAI and Anthropic were paused by the owner's
monthly limits and the head ran on Mistral. The provider block still said "openai:
available, VERIFIED" and "anthropic: available, VERIFIED" and offered "spawn on openai" and
"spawn on anthropic"; the Notion row said "always try it first", so a rail and weather lookup
spawned on Mistral searched Notion twice and reported "no rail schedule or weather data found
in Notion", and the head then told the owner Notion was its only source. Host-tested first
(test_connectors_wire, test_harness_turn, test_harness_wire_mistral); this leg proves it on a
bench unit (the test build, Orchestrator mode, OpenAI, Anthropic and Mistral keyed).

``test_hfc_web_questions_use_mistral_web_search``: OpenAI and Anthropic frozen by 1-cent monthly
limits (their spend seeded over them), the head on Mistral, a Mistral Notion card enabled with
Mistral's web_search built-in (each added for the leg when the unit has none, as on Lumi). After
a conversation reset each, the web chat asks three different public-web questions (the weather
in Cambridge, the Elizabeth line, the Fitzwilliam Museum's opening hours). Each ask passes when a
fresh Mistral web_search sub ran web search with the owner's connectors left off its request (so
no Notion call), the delivered answer names at least one source and never says the web is
unavailable or credits Notion, and the prompt's provider block shows OpenAI and Anthropic PAUSED
with no spawn line for either. The head runs on ``NIMBUS_HFC_HEAD_MODEL`` (mistral-small-latest
by default); the device estimated about 5 cents per ask on 27 Oct. The limits, the seeded spend,
the head, the priority and the connectors are put back. ``NIMBUS_HFC_ASKS`` runs the first n
questions (all three by default).
Needs ``NIMBUS_TEST_IP`` and ``NIMBUS_TEST_TOKEN`` and the console (SPENDSET). Evidence goes to
``NIMBUS_HFC_EVIDENCE`` (a directory) when set.
"""

from __future__ import annotations

import json
import os
import re
import time

import pytest

from webrig import chat_turn, ip_tok_or_skip, make_session, require_orchestrator

_S = None
SLUGS = ("cumulo", "openai", "anthropic", "mistral", "zai")
FROZEN = ("openai", "anthropic")
# Three different public-web questions, so no ask can be answered from an earlier one
# (NIMBUS_HFC_QUESTIONS, "|"-separated, replaces them: a unit that answered these today
# finds them in its own log).
QUESTIONS = tuple(
    q.strip()
    for q in os.environ.get(
        "NIMBUS_HFC_QUESTIONS",
        "What's the weather in Cambridge today?|Is the Elizabeth line running normally right now?|"
        "What are the opening hours of the Fitzwilliam Museum today?",
    ).split("|")
    if q.strip()
)
# A file ask while OpenAI and Anthropic are paused: it must not go to either (review of
# 4e9cf28f), so it runs on the device writer or the reply says plainly it is not possible.
PDF_ASK = os.environ.get("NIMBUS_HFC_PDF_ASK", "Make a one-page PDF titled Bench check with today's date in it.")
# A named source: a link, a domain, or the word itself.
SOURCE = re.compile(r"https?://|\bsources?\b|according to|\b[a-z0-9-]+\.(com|org|net|uk|gov|info)\b", re.I)
HEAD_MODEL = os.environ.get("NIMBUS_HFC_HEAD_MODEL", "mistral-small-latest")
# A delivered answer that gives up on the web, or credits Notion with public facts.
GAVE_UP = re.compile(
    r"(no|don't have|do not have|without) (live )?(web|internet) access|cannot (check|look up|access) "
    r"(the )?(web|weather|train)|only connector|notion is the only|notion was the fallback|found in notion",
    re.I,
)
TAG = re.compile(r"\[tools: (\d+)(?:, web: (\d+))?\]")


def _orch(ip: str) -> dict:
    return _S.get(f"http://{ip}/api/orch", timeout=10).json()


def _keyed(ip: str) -> list:
    provs = _orch(ip).get("providers") or {}
    return [p for p in SLUGS if (provs.get(p) or {}).get("hasKey")]


def _log(ip: str) -> str:
    r = _S.get(f"http://{ip}/api/errlog/recent", params={"bytes": 16384}, timeout=15)
    return r.text if r.status_code == 200 else ""


def _ram_log(ip: str) -> str:
    r = _S.get(f"http://{ip}/api/log", timeout=15)
    return r.text if r.status_code == 200 else ""


def _new_lines(before: str, after: str) -> list:
    """The lines of ``after`` past the last lines ``before`` ended with (the window rolls)."""
    old = [ln for ln in before.splitlines() if ln.strip()]
    new = [ln for ln in after.splitlines() if ln.strip()]
    for k in (3, 2, 1):
        if len(old) < k:
            continue
        anchor = old[-k:]
        for i in range(len(new) - k, -1, -1):
            if new[i : i + k] == anchor:
                return new[i + k :]
    return new


def _jobs_live(ip: str) -> int:
    try:
        return int(_S.get(f"http://{ip}/api/state", timeout=10).json().get("jobs") or 0)
    except Exception:  # noqa: BLE001 - a busy page: count it as work
        return 1


def _epi_id(row: dict) -> int:
    try:
        return int(str(row.get("id", "m0"))[1:], 16)
    except ValueError:
        return 0


def _assistant_rows(ip: str, floor: int):
    """The web chat's assistant messages after episodic id `floor`, oldest first (a sub-agent's
    synthesis has no turn id of its own: it is read from the log, never the legacy slot).
    None when the log could not be read (a 503 while the card is busy is not "no rows")."""
    try:
        r = _S.get(
            f"http://{ip}/api/mem/episodic",
            params={"session": "web", "kind": "message", "limit": 20},
            timeout=15,
        )
        if r.status_code != 200:
            return None
        rows = r.json().get("messages") or []
    except Exception:  # noqa: BLE001 - unreadable this poll
        return None
    return sorted((x for x in rows if x.get("role") == "assistant" and _epi_id(x) > floor), key=_epi_id)


def _rows_or_fail(ip: str, floor: int, what: str, tries: int = 10) -> list:
    """`_assistant_rows`, retried while the log is unreadable; the ask fails if it never reads."""
    for _ in range(tries):
        rows = _assistant_rows(ip, floor)
        if rows is not None:
            return rows
        time.sleep(3)
    pytest.fail(f"the episodic log stayed unreadable ({what})")


def _ask(ip: str, text: str, cap_s: int = 420) -> list:
    """One web-chat ask: its own reply (by turn id), then every later delivery (the synthesis of
    the sub-agent it started) until no job runs and nothing new arrives for 25 s."""
    floor = max((_epi_id(r) for r in _rows_or_fail(ip, 0, "the floor read")), default=0)
    first = chat_turn(_S, ip, text, timeout_s=cap_s)
    deadline = time.time() + cap_s
    rows, quiet_since = [], time.time()
    while time.time() < deadline:
        now = _assistant_rows(ip, floor)
        if now is None:  # a busy card: keep the last good rows, count it as activity
            quiet_since = time.time()
        elif len(now) != len(rows) or _jobs_live(ip):
            rows, quiet_since = now, time.time()
        elif time.time() - quiet_since >= 25:
            break
        time.sleep(4)
    rows = _rows_or_fail(ip, floor, "the final read")
    texts = [str(r.get("text") or "") for r in rows]
    return texts if texts else [first]


def _spend_state(device) -> dict:
    out = device.cmd("SPENDSET?", "SPENDSET {", timeout=8.0)
    j = json.loads(out[out.index("{") : out.rindex("}") + 1])
    return {r["p"]: r for r in j.get("providers", [])}


def _budget_row(ip: str, prov: str) -> dict:
    for row in (_orch(ip).get("usage") or {}).get("byProvider") or []:
        if row.get("prov") == prov:
            return row
    return {}


def _connectors(ip: str) -> list:
    return _S.get(f"http://{ip}/api/connectors", timeout=10).json().get("configured", [])


def _patch_connector(ip: str, obj: dict) -> None:
    r = _S.post(f"http://{ip}/api/connectors", data={"patch": json.dumps(obj)}, timeout=10)
    assert r.status_code == 200, f"connector patch -> {r.status_code}: {r.text[:160]}"


def _evidence(name: str, text: str) -> None:
    d = os.environ.get("NIMBUS_HFC_EVIDENCE")
    if not d:
        return
    os.makedirs(d, exist_ok=True)
    with open(os.path.join(d, name), "w", encoding="utf-8") as f:
        f.write(text)


@pytest.fixture(scope="module")
def board(device, net):
    global _S
    ip, tok = ip_tok_or_skip("lane HFC web facts")
    _S = make_session(tok)
    deadline = time.time() + 90
    while time.time() < deadline:
        try:
            if _S.get(f"http://{ip}/api/state", timeout=5).status_code in (200, 401):
                break
        except Exception:  # noqa: BLE001 - not up yet
            pass
        time.sleep(3)
    require_orchestrator(_S, ip, "lane HFC web facts")
    return ip


def _check_block(i: int, lastturn: str) -> list:
    """The provider block shows the frozen providers PAUSED and offers no spawn on them."""
    at = lastturn.find("[PROVIDERS & CONNECTORS]")
    block = lastturn[at:] if at >= 0 else ""
    fails = [f"ask {i}: the provider block does not show {p} PAUSED" for p in FROZEN if f"- {p}: PAUSED." not in block]
    fails += [f"ask {i}: the provider block still offers spawn on {p}" for p in FROZEN if f"spawn on {p}" in block]
    return fails


def _check_tags(i: int, tags: list) -> list:
    """Each fresh result's provider record shows at least one web search."""
    if not tags:
        return [f"ask {i}: no [tools: n, web: m] tag on a fresh result in the synthesis turn"]
    return [f"ask {i}: the sub ran no web search (tools {t}, web {w or 0})" for t, w in tags if not w or int(w) == 0]


def _check_sub(i: int, lastturn: str, lines: list) -> list:
    """A mistral sub ran web search, as a web_search spawn with the connectors left off."""
    fails = []
    # The synthesis turn names each finished sub "(jobNNNN, <provider>/<model>)" (the durable
    # log can drop its "spawned" line under card-lock contention).
    subs = re.findall(r"\((job\d+), ([a-z]+)/[^)]*\): finished just now", lastturn)
    if not subs or any(p != "mistral" for _, p in subs):
        fails.append(f"ask {i}: no sub on mistral alone ({subs})")
    fails += _check_tags(i, TAG.findall(lastturn))
    if any("mistral sub: connectors attached=" in ln and "calls=0" not in ln for ln in lines):
        fails.append(f"ask {i}: a connector call is logged")
    fails += [
        f"ask {i}: {job} was not a web_search spawn with the connectors left off"
        for job, _ in subs
        if not _left_off(job, lines)
    ]
    return fails


def _left_off(job: str, lines: list) -> bool:
    """The dispatch of ``job`` logged "web_search spawn - n Studio connector(s) left off" just before
    its "spawned <job>" line. The durable log can drop that line under card-lock contention; then
    a "left off" line among this ask's own lines stands for it."""
    left = [k for k, ln in enumerate(lines) if "web_search spawn - " in ln and "left off" in ln]
    at = next((k for k, ln in enumerate(lines) if f"spawned {job} " in ln), -1)
    if at < 0:
        return bool(left)
    # The durable log dropped the line just before (card-lock contention): not a failure.
    dropped = any("durable log dropped" in ln for ln in lines[max(0, at - 2) : at])
    return dropped or any(at - 4 <= k < at for k in left)


def _check_answer(i: int, replies: list) -> list:
    """The delivered answer never gives up on the web or credits Notion, and names a source."""
    answer = "\n".join(replies)
    fails = []
    if GAVE_UP.search(answer):
        fails.append(f"ask {i}: the answer gives up on the web or credits Notion: {answer[:300]}")
    final = replies[-1] if replies else ""  # the synthesis reply (earlier ones may be stale slot text)
    if not SOURCE.search(final):
        fails.append(f"ask {i}: the answer names no source: {final[:300]}")
    return fails


def _state_before(board: str, device) -> dict:
    o0 = _orch(board)
    cards = [c for c in _connectors(board) if c.get("prov") == "mistral"]
    return {
        "host": o0.get("orchHost") or "",
        "model": ((o0.get("providers") or {}).get("mistral") or {}).get("orchModel") or "",
        "prio": o0.get("provPrio") or "",
        "caps": _spend_state(device),
        "budgets": {p: _budget_row(board, p) for p in FROZEN},
        "cards": {c.get("name"): c for c in cards},
        "added": [],
    }


def _unfreeze_ceiling(board: str) -> None:
    ceiling = (_orch(board).get("usage") or {}).get("ceiling") or {}
    if ceiling.get("frozen"):
        _S.post(f"http://{board}/api/orch", data={"spendCeil": "10000"}, timeout=10)  # the bench setting
        ceiling = (_orch(board).get("usage") or {}).get("ceiling") or {}
    assert not ceiling.get("frozen"), f"the overall ceiling is frozen: {ceiling}"


def _set_up(board: str, device, keyed: list, st: dict) -> None:
    """Head on Mistral, OpenAI and Anthropic frozen by 1-cent limits, Lumi's Mistral cards on."""
    _S.post(f"http://{board}/api/orch", data={"orchM_mistral": HEAD_MODEL}, timeout=10)
    _S.post(f"http://{board}/api/orch", data={"orchHost": "mistral"}, timeout=10)
    frozen = [p for p in FROZEN if p in keyed]
    for p in frozen:
        _S.post(f"http://{board}/api/orch", data={"budget": f"{p}:0:0:1:1"}, timeout=10)
        spent = int((st["caps"].get(p) or {}).get("spentCents", 0))
        device.cmd(f"SPENDSET prov {p} {max(spent, 100)}", "SPENDSET", timeout=8.0)
    caps = _spend_state(device)
    for p in frozen:
        assert (caps.get(p) or {}).get("frozen"), f"{p} is not frozen by the micro cap: {caps.get(p)}"
    # Lumi's set: Mistral's web_search built-in and the Notion Studio connector.
    want = {
        "web_search": {"name": "web_search", "prov": "mistral", "kind": "builtin", "en": 1},
        "notion": {"name": "notion", "prov": "mistral", "kind": "connector", "type": "notion", "en": 1},
    }
    for name, card in want.items():
        had = st["cards"].get(name)
        if not had:
            _patch_connector(board, card)
            st["added"].append(name)
        elif not had.get("en"):
            _patch_connector(board, {"name": name, "en": 1})
    time.sleep(24)  # the staged config, then the Mistral workspace check a connector save queues
    now = _connectors(board)
    print(f"[HFC] connectors now: {json.dumps(now)}")
    _evidence("connectors-during.json", json.dumps(now, indent=1))


def _restore_caps(board: str, device, st: dict) -> None:
    for p in FROZEN:
        b = st["budgets"].get(p) or {}
        restore = "%s:%d:%d:%d:%d" % (
            p,
            int(b.get("tokenLimit", 0)),
            int(b.get("callLimit", 0)),
            int(b.get("resetDay", 1)) or 1,
            int(b.get("centsLimit", 0)),
        )
        _S.post(f"http://{board}/api/orch", data={"budget": restore}, timeout=10)
        device.cmd(f"SPENDSET prov {p} {int((st['caps'].get(p) or {}).get('spentCents', 0))}", "SPENDSET", timeout=8.0)


def _restore(board: str, device, st: dict) -> None:
    _restore_caps(board, device, st)
    for name in st["added"]:
        _S.post(f"http://{board}/api/connectors", data={"del": name}, timeout=10)
    for name, had in st["cards"].items():
        if name in ("web_search", "notion") and not had.get("en"):
            _patch_connector(board, {"name": name, "en": 0})
    for key, field in (("model", "orchM_mistral"), ("host", "orchHost"), ("prio", "provPrio")):
        if st[key]:
            _S.post(f"http://{board}/api/orch", data={field: st[key]}, timeout=10)
    o1 = _orch(board)
    caps1 = _spend_state(device)
    model1 = ((o1.get("providers") or {}).get("mistral") or {}).get("orchModel")
    print(
        f"[HFC] restored: head {o1.get('orchHost')}/{model1}, caps {json.dumps({p: caps1.get(p) for p in FROZEN})}, "
        f"mistral cards {[c.get('name') for c in _connectors(board) if c.get('prov') == 'mistral']}"
    )


def _one_ask(board: str, i: int, question: str) -> list:
    """One fresh ask and its failures ([] = pass)."""
    _S.post(f"http://{board}/api/orch", data={"convReset": "1"}, timeout=10)
    time.sleep(3)
    log0, ram0 = _log(board), _ram_log(board)
    replies = _ask(board, question)
    lastturn = _S.get(f"http://{board}/api/lastturn", timeout=20).text
    # Each log anchored on its own (the RAM ring rolls faster than the durable log).
    lines = _new_lines(log0, _log(board)) + _new_lines(ram0, _ram_log(board))
    _evidence(f"ask{i}-replies.txt", "\n\n----- delivery -----\n\n".join(replies))
    _evidence(f"ask{i}-lastturn.txt", lastturn)
    _evidence(f"ask{i}-log.txt", "\n".join(lines))
    got = _check_block(i, lastturn) + _check_sub(i, lastturn, lines) + _check_answer(i, replies)
    verdict = "PASS" if not got else "FAIL " + json.dumps(got)
    print(f"[HFC] ask {i}: {len(replies)} deliveries; tags {TAG.findall(lastturn)}; {verdict}")
    print(f"[HFC] ask {i} answer: {replies[-1][:400] if replies else '(none)'}")
    return got


def _pdf_files(board: str) -> set:
    """The "<project>/<name>" of each .pdf in the Files listing (empty when unreadable)."""
    try:
        rows = _S.get(f"http://{board}/api/files/list", timeout=15).json().get("files") or []
    except Exception:  # noqa: BLE001 - a busy page
        return set()
    return {f"{r.get('project')}/{r.get('name')}" for r in rows if str(r.get("name") or "").endswith(".pdf")}


def _pdf_ask(board: str) -> list:
    """The file ask: no spawn on a paused OpenAI or Anthropic. On the device/devicepaused route a
    file must land (the writer's log line or a new .pdf in Files); a refusal counts only on the
    paused/nostore routes and must say it is not possible."""
    _S.post(f"http://{board}/api/orch", data={"convReset": "1"}, timeout=10)
    time.sleep(3)
    route = _S.get(f"http://{board}/api/orch", timeout=10).json().get("pdfRoute")
    log0, ram0, files0 = _log(board), _ram_log(board), _pdf_files(board)
    replies = _ask(board, PDF_ASK)
    lines = _new_lines(log0, _log(board)) + _new_lines(ram0, _ram_log(board))
    new_files = sorted(_pdf_files(board) - files0)
    _evidence("pdf-replies.txt", "\n\n----- delivery -----\n\n".join(replies))
    _evidence("pdf-log.txt", "\n".join(lines))
    _evidence("pdf-route.txt", f"pdfRoute={route}\nnew files={new_files}\n")
    answer = "\n".join(replies)
    fails = [
        f"pdf: a sub-agent went to paused {p}"
        for p in FROZEN
        if any("spawned " in ln and f"({p}/" in ln for ln in lines)
    ]
    if route in FROZEN:
        fails.append(f"pdf: the route names paused {route}")
    wrote = any("with the device PDF writer" in ln for ln in lines) or bool(new_files)
    if route in ("device", "devicepaused") and not wrote:
        fails.append(f"pdf: route {route} but no file landed (no writer line, no new .pdf): {answer[:300]}")
    if route in ("paused", "nostore") and not re.search(r"not possible", answer, re.I):
        fails.append(f"pdf: route {route} but the reply does not say it is not possible: {answer[:300]}")
    print(f"[HFC] pdf: route {route}; new files {new_files}; {'PASS' if not fails else 'FAIL ' + json.dumps(fails)}")
    print(f"[HFC] pdf answer: {replies[-1][:400] if replies else '(none)'}")
    return fails


@pytest.mark.hil
@pytest.mark.net
@pytest.mark.agent
def test_hfc_web_questions_use_mistral_web_search(board, device):
    keyed = _keyed(board)
    missing = [p for p in ("mistral",) + FROZEN if p not in keyed]
    if missing:
        pytest.skip(f"needs OpenAI, Anthropic and Mistral keyed (missing: {missing})")
    _unfreeze_ceiling(board)
    st = _state_before(board, device)
    print(f"\n[HFC] before: head {st['host']}/{st['model']}, prio {st['prio']}, mistral cards {list(st['cards'])}")
    fails = []
    try:
        _set_up(board, device, keyed, st)
        for i, question in enumerate(QUESTIONS[: int(os.environ.get("NIMBUS_HFC_ASKS", "3"))], start=1):
            fails += _one_ask(board, i, question)
        if PDF_ASK:
            fails += _pdf_ask(board)
    finally:
        _restore(board, device, st)
    assert not fails, "\n".join(fails)
