"""L54 - PDF delivery, the web page batch, connector honesty and the archive view.

Bench legs for program-2026-10-18 lane B: one unit with an SD card, the test build,
Orchestrator mode, cloud-paired. LAN legs need ``NIMBUS_TEST_IP`` and
``NIMBUS_TEST_TOKEN``; the console legs (``hil``) need the board on its port
(``--port`` or ``NIMBUS_PORT``).

Routes and the page (no spend, ``net``):
  * the page is two responses: ``GET /`` carries a relative ``<script src="app.js">``
    and no inline script body; ``GET /app.js`` is the script, ungated, as
    text/javascript (the cloud relay buffers each body under 383 KB);
  * CUM-718: ``POST /api/connectors/oauth/start``, ``GET .../status`` and
    ``POST .../cancel`` reach their own handlers (they were answered by the generic
    connectors handler), and ``GET /api/cloud/mintkey`` answers its own shape; a
    sign-in step against a server that never answers (``NIMBUS_L54_SLOW_URL``) runs
    off the watched loop, so the device never resets (lane WDT's finding);
  * ``POST /api/connectors/check`` queues the Mistral workspace check (or says the
    Mistral key is missing); ``GET /api/connectors`` carries the honesty fields;
  * ``GET /api/orch`` names the PDF route (``pdfRoute``) the Code sandbox line shows.

The Telegram archive (``net``): a day page stays under 16 KB; Export Day is a valid
ZIP holding exactly that day's files; with ``hil`` + ``agent`` the item and ZIP routes
answer the cloud relay's loopback 503 while a turn runs and 200 after (the admission
gate, prism rows 6 and 7).

Settings store (unit L, ``hil``): ``NVSFILL`` until full shows a degraded Settings
storage row on /api/health (the Home panel turns amber); ``NVSFILL 0`` frees it.

Mistral honesty (``hil``, needs a Mistral key and a Studio connector that the
workspace lists): the confirmed state survives a restart, read before any check runs.

The PDF recipe (``agent``, real spend, one run per route): a web turn asks for a
one-page PDF; a new .pdf lands under Files, the device log shows the capture (or the
device writer) line, the settings store's failed-save count is unchanged, and the
turn's reply names the file. ``NIMBUS_L54_ROUTE`` names the route the unit is set up
for (openai, anthropic or device); the leg checks /api/orch agrees before it spends.
The Telegram document itself is a human gate (H2-8): the orchestrator's runner looks
at the bench chat.
"""

from __future__ import annotations

import io
import json
import os
import re
import time
import zipfile

import pytest

try:  # keep `pytest --collect-only` clean on a host without requests
    import requests
except ImportError:  # pragma: no cover
    requests = None

# The PdfRoute slugs (connectors_wire.h is the one frozen list; devicepaused and paused: a
# held-off sandbox provider, CUM-821).
ROUTES = {"openai", "anthropic", "device", "devicepaused", "nostore", "nocapture", "paused", "sandboxoff", "nokey"}


def _lan():
    if requests is None:
        pytest.skip("requests not installed")
    ip, tok = os.environ.get("NIMBUS_TEST_IP"), os.environ.get("NIMBUS_TEST_TOKEN")
    if not ip or not tok:
        pytest.skip("NIMBUS_TEST_IP and NIMBUS_TEST_TOKEN are required for the L54 LAN legs")
    s = requests.Session()
    s.headers["X-Nimbus-Token"] = tok
    return ip, s


def _url(ip, path):
    return f"http://{ip}{path}"


# ---- the page and its script ---------------------------------------------------------


@pytest.mark.net
def test_page_and_script_are_two_ungated_responses():
    ip, _ = _lan()
    page = requests.get(_url(ip, "/"), timeout=20)
    assert page.status_code == 200
    assert '<script src="app.js"' in page.text, "GET / no longer loads its script from app.js"
    assert "const $=id=>" not in page.text, "the script body is still inline in GET /"
    js = requests.get(_url(ip, "/app.js"), timeout=30)  # no token: a script tag sends none
    assert js.status_code == 200
    assert js.headers.get("Content-Type", "").startswith("text/javascript")
    assert js.text.startswith("\nconst $=id=>") or js.text.startswith("const $=id=>")
    assert "<script" not in js.text[:200] and not js.text.rstrip().endswith("</script>")
    for body in (page.content, js.content):
        assert len(body) + 16 * 1024 <= 383 * 1024, f"a response is {len(body)} B: over the relay buffer"


# ---- CUM-718: every route reaches its own handler -------------------------------------


@pytest.mark.net
def test_oauth_routes_reach_their_own_handlers():
    ip, s = _lan()
    r = s.post(_url(ip, "/api/connectors/oauth/start"), data={"name": "no-such-connector"}, timeout=10)
    assert "patch must be a JSON object" not in r.text, "start was answered by the generic handler"
    assert r.status_code in (200, 400) and ("error" in r.json() or r.json().get("ok"))
    st = s.get(_url(ip, "/api/connectors/oauth/status"), timeout=10).json()
    assert "configured" not in st, "status was answered by GET /api/connectors"
    assert "active" in st or "phase" in st
    c = s.post(_url(ip, "/api/connectors/oauth/cancel"), timeout=10)
    assert c.status_code == 200 and c.json().get("ok") is True


SLOW_URL = os.environ.get("NIMBUS_L54_SLOW_URL", "https://example.com:81/mcp")


@pytest.mark.net
def test_oauth_sign_in_step_against_a_slow_server_never_resets_the_device():
    """Lane WDT's finding: a sign-in step ran inline on the watched main loop with 8-10 s
    HTTPS timeouts, so a slow server reset the device (the 8 s task watchdog). The step
    now runs on a worker. A connector pointed at a server that never answers (default: a
    filtered port, every connect waits out its timeout) is signed in; the device keeps
    answering and its uptime never goes back (no reset), then the flow is cancelled."""
    ip, s = _lan()
    name = "l54slow"
    patch = {"name": name, "type": name, "prov": "any", "kind": "mcp", "url": SLOW_URL, "en": 0, "dev": 1}
    r = s.post(_url(ip, "/api/connectors"), data={"patch": json.dumps(patch)}, timeout=15)
    assert r.status_code == 200, r.text[:200]
    try:
        up0 = int(s.get(_url(ip, "/api/state"), timeout=10).json().get("uptimeMs", 0))
        st = s.post(_url(ip, "/api/connectors/oauth/start"), data={"name": name}, timeout=10)
        assert st.status_code == 200, st.text[:200]
        last, phases = up0, set()
        end = time.time() + 45
        while time.time() < end:
            time.sleep(2)
            up = int(s.get(_url(ip, "/api/state"), timeout=10).json().get("uptimeMs", 0))
            assert up > last, f"uptime went back ({last} -> {up}): the device restarted during the sign-in step"
            last = up
            phases.add(s.get(_url(ip, "/api/connectors/oauth/status"), timeout=10).json().get("phase"))
        assert phases & {"discovering", "registering", "failed"}, phases
    finally:
        s.post(_url(ip, "/api/connectors/oauth/cancel"), timeout=10)
        s.post(_url(ip, "/api/connectors"), data={"del": name}, timeout=15)


@pytest.mark.net
def test_cloud_mintkey_route_answers_its_own_shape():
    ip, s = _lan()
    m = s.get(_url(ip, "/api/cloud/mintkey"), timeout=10).json()
    assert "state" in m and m["state"] in ("idle", "pending", "done", "error"), m


# ---- Mistral connector honesty and the Code sandbox line ------------------------------


@pytest.mark.net
def test_connectors_check_and_honesty_fields():
    ip, s = _lan()
    d = s.get(_url(ip, "/api/connectors"), timeout=15).json()
    assert "mistralTools" in d and "mistralToolsAdvised" in d
    assert all(k.get("id") != "code_interpreter" or k.get("cred") for k in d.get("known", []))
    gcal = next((k for k in d.get("known", []) if k.get("id") == "gcal"), None)
    assert gcal and gcal.get("mcid") == "google_calendar", "the Mistral id is not offered for gcal"
    r = s.post(_url(ip, "/api/connectors/check"), timeout=10)
    if r.status_code == 409:
        assert r.json().get("error") == "no_key"
    else:
        assert r.status_code == 200 and "queued" in r.json()


@pytest.mark.net
def test_orch_names_the_pdf_route():
    ip, s = _lan()
    o = s.get(_url(ip, "/api/orch"), timeout=15).json()
    assert o.get("pdfRoute") in ROUTES, o.get("pdfRoute")
    st = s.get(_url(ip, "/api/state"), timeout=15).json()
    if st.get("storeSD"):
        assert o["pdfRoute"] != "nostore"


@pytest.mark.hil
def test_confirmed_mistral_connector_survives_a_restart(device):
    ip, s = _lan()
    before = {c["name"]: c for c in s.get(_url(ip, "/api/connectors"), timeout=15).json()["configured"]}
    confirmed = [
        n
        for n, c in before.items()
        if c.get("prov") == "mistral" and c.get("kind") == "connector" and c.get("auth") == 1
    ]
    if not confirmed:
        pytest.skip("no confirmed Mistral Studio connector on this unit (needs a Mistral key and a listed connector)")
    device.send("RESTART")
    deadline = time.time() + 120
    after = None
    while time.time() < deadline:
        time.sleep(4)
        try:
            after = s.get(_url(ip, "/api/connectors"), timeout=8).json()
            break
        except Exception:  # noqa: BLE001 - still booting
            continue
    assert after, "the unit did not come back within 120 s"
    got = {c["name"]: c for c in after["configured"]}
    for n in confirmed:  # read before the 90 s boot check could have run
        assert got[n]["auth"] == 1, f"{n} dropped to {got[n].get('ws') or got[n]['auth']} across the restart"


# ---- the Telegram archive -------------------------------------------------------------


def _first_day(ip, s):
    arc = s.get(_url(ip, "/api/tg/archive"), timeout=15).json()
    if not arc.get("chats"):
        pytest.skip("the archive holds no chat on this unit")
    chat = arc["chats"][0]["chat"]
    days = s.get(_url(ip, "/api/tg/archive"), params={"chat": chat}, timeout=15).json()["days"]
    if not days:
        pytest.skip("the first archived chat has no day")
    return chat, days[0]


@pytest.mark.net
def test_archive_day_page_stays_small_and_pages_on():
    ip, s = _lan()
    chat, day = _first_day(ip, s)
    r = s.get(_url(ip, "/api/tg/archive"), params={"chat": chat, "day": day, "limit": 200}, timeout=15)
    assert r.status_code == 200 and len(r.content) < 16 * 1024, f"a day page is {len(r.content)} B"
    page = r.json()
    if page["more"]:
        nxt = s.get(
            _url(ip, "/api/tg/archive"), params={"chat": chat, "day": day, "from": page["next"]}, timeout=15
        ).json()
        assert nxt["from"] == page["next"]


@pytest.mark.net
def test_archive_export_day_is_a_valid_zip_of_that_day():
    ip, s = _lan()
    chat, day = _first_day(ip, s)
    r = s.get(_url(ip, "/api/tg/archive/zip"), params={"chat": chat, "day": day}, timeout=120)
    assert r.status_code == 200, r.text[:200]
    assert r.headers.get("Content-Type") == "application/zip"
    assert f'filename="{chat}-{day}.zip"' in r.headers.get("Content-Disposition", "")
    z = zipfile.ZipFile(io.BytesIO(r.content))
    assert z.testzip() is None, "a member's CRC does not match its bytes"
    names = set(z.namelist())
    assert "index.jsonl" in names
    rows, start = [], 0
    while True:
        pg = s.get(_url(ip, "/api/tg/archive"), params={"chat": chat, "day": day, "from": start}, timeout=15).json()
        rows += pg["rows"]
        if not pg["more"]:
            break
        start = pg["next"]
    listed = {x["file"] for x in rows if x.get("file") and x.get("st", "ok") == "ok"}
    assert listed <= names, f"files the index names but the ZIP lacks: {sorted(listed - names)}"
    assert not [n for n in names if n.endswith((".part", ".tmp"))]


@pytest.mark.hil
@pytest.mark.agent
def test_archive_routes_answer_busy_through_the_relay_during_a_turn(device):
    """Prism rows 6 and 7: the item (a media file over 16 KB) and the ZIP go through
    the admission gate - the relay's loopback replay is refused (503) while a turn is
    in flight and served after it."""
    from webrig import chat_turn, make_session

    ip, s = _lan()
    chat, day = _first_day(ip, s)
    page = s.get(_url(ip, "/api/tg/archive"), params={"chat": chat, "day": day}, timeout=15).json()
    media = next((x["file"] for x in page["rows"] if x.get("file") and not x["file"].endswith(".json")), None)
    if not media:
        pytest.skip("the day holds no media file")
    item = f"/api/tg/archive/item?chat={chat}&day={day}&name={media}"
    paths = [item, f"/api/tg/archive/zip?chat={chat}&day={day}"]
    statuses = {p: [] for p in paths}
    import threading

    t = threading.Thread(
        target=chat_turn,
        args=(
            make_session(s.headers["X-Nimbus-Token"]),
            ip,
            "Search memory for 'tea' three times, then answer in one sentence.",
        ),
        daemon=True,
    )
    t.start()
    time.sleep(6)
    for p in paths:
        out = device.cmd(f"CLOUDLOOP {p}", expect="CLOUDLOOP", timeout=60.0)
        m = re.search(r"CLOUDLOOP \S+ -> (-?\d+)", out)
        statuses[p].append(int(m.group(1)) if m else None)
    t.join(timeout=240)
    out = device.cmd(f"CLOUDLOOP {item}", expect="CLOUDLOOP", timeout=60.0)
    m = re.search(r"CLOUDLOOP \S+ -> (-?\d+)", out)
    statuses[item].append(int(m.group(1)) if m else None)
    assert statuses[item][0] == 503 and statuses[paths[1]][0] == 503, statuses
    assert statuses[item][-1] == 200, statuses


# ---- the settings store (unit L) ------------------------------------------------------


# Filled the way test_l43_nvs_reserve.py fills it: erase the filler keys, then ONE
# NVSFILL sized from /api/state. NVSFILL n rewrites the keys k0..k{n-1} in place, so
# repeating a small fill never takes more space (the first bench run looped
# "NVSFILL 64" forty times and never got past the first 64 entries).
LEAVE_FREE = 10  # entries left free: under the reserve, as on 19 Oct


def _health_row(s, ip, key):
    h = s.get(_url(ip, "/api/health"), timeout=15).json()
    return h, next((c for c in h["components"] if c["key"] == key), None)


@pytest.mark.hil
def test_full_settings_store_shows_the_row_then_clears(device):
    from test_l43_nvs_reserve import RESERVE, free_entries, nvs_fill, nvs_free

    ip, s = _lan()
    try:
        nvs_free(device)
        st = s.get(_url(ip, "/api/state"), timeout=15).json()
        nvs_fill(device, max(0, free_entries(st) - LEAVE_FREE))
        free = free_entries(s.get(_url(ip, "/api/state"), timeout=15).json())
        assert free < RESERVE, f"the fill left {free} free entries, not under the reserve of {RESERVE}"
        h, row = _health_row(s, ip, "nvs")
        assert row and row["state"] == "degraded", h["components"]
        assert f"under the reserve of {RESERVE}" in row["detail"], row["detail"]
        assert "entries free" in row["detail"]
        assert h["degraded"] >= 1  # the Home panel is amber
    finally:
        nvs_free(device)
    h, row = _health_row(s, ip, "nvs")
    # Freed: the row is gone, or (a save failed while full) it no longer says "under the reserve".
    assert row is None or "under the reserve" not in row["detail"], row


# ---- the PDF recipe (real spend) ------------------------------------------------------


def _pdfs(s, ip):
    files = s.get(_url(ip, "/api/files/list"), timeout=20).json()
    rows = files.get("files", files) if isinstance(files, dict) else files
    return {(f.get("project"), f.get("name")) for f in rows if str(f.get("name", "")).lower().endswith(".pdf")}


def _astool(s, ip, tool, args):
    """Call a registry tool as the owner's web chat (test build route)."""
    body = json.dumps({"jsonrpc": "2.0", "id": 1, "method": "tools/call", "params": {"name": tool, "arguments": args}})
    r = s.post(_url(ip, "/api/test/astool"), data={"chat": "web", "body": body}, timeout=60)
    assert r.status_code == 200, r.text[:300]
    return r.text


@pytest.mark.net
def test_device_pdf_writer_saves_a_real_pdf_and_refuses_oversize():
    """The device's own PDF writer (files.pdf) on the real card, no spend: a small
    document saves a valid PDF under Files; an oversize one is refused, not cut."""
    ip, s = _lan()
    st = s.get(_url(ip, "/api/state"), timeout=15).json()
    if not st.get("storeSD"):
        pytest.skip("files.pdf needs an SD card")
    out = _astool(
        s,
        ip,
        "files.pdf",
        {
            "project": "l54",
            "name": "hil-check",
            "title": "Bench check",
            "text": "# Moon\nTwo short lines.\n\n| a | b |\n|---|---|\n| 1 | 2 |\n",
        },
    )
    try:
        assert "[file saved: l54/hil-check.pdf]" in out, out[:300]
        pdf = s.get(_url(ip, "/api/files/dl"), params={"project": "l54", "name": "hil-check.pdf"}, timeout=20)
        assert pdf.status_code == 200 and pdf.content.startswith(b"%PDF-1.4"), pdf.content[:16]
        assert pdf.content.rstrip().endswith(b"%%EOF")
        big = _astool(s, ip, "files.pdf", {"project": "l54", "name": "too-big", "text": "x " * 25000})
        assert "longer than 48 KB" in big, big[:300]
    finally:
        for name in ("hil-check.pdf", "too-big.pdf"):
            s.post(_url(ip, "/api/files/rm"), data={"project": "l54", "name": name}, timeout=10)


# Each route asks for its own file name and deletes it first, so a file left by an
# earlier run (or by another route: the first bench run reused one name for every
# route) can never pass for this run's result. The prompt names the route's
# mechanism, so the leg exercises that route rather than whichever the head prefers.
RECIPE_PROJECT = "l54"
ROUTE_HOW = {
    "openai": "Make it in an openai sub-agent's code sandbox (code_interpreter), not with the device's own writer.",
    "anthropic": "Make it in an anthropic sub-agent's sandbox, not with the device's own writer.",
    "device": "Make it with the device's own PDF writer (files.pdf).",
}
MADE_BY = {  # the log line each route writes for a PDF, naming "<project>/<name>"
    "openai": r"files: captured (\S+/\S+\.pdf) \(\d+ B\) from openai",
    "anthropic": r"files: captured (\S+/\S+\.pdf) \(\d+ B\) from anthropic",
    "device": r"files: wrote (\S+/\S+\.pdf) \(\d+ B, \d+ pages?\) with the device PDF writer",
}


def recipe_prompt(route: str, name: str) -> str:
    return (
        "Make a one-page PDF titled 'Bench check' with two short paragraphs about the moon and a two-row "
        f"table of its distance and diameter. Save it in project {RECIPE_PROJECT} as {name}, and tell me the "
        f"file name. {ROUTE_HOW[route]}"
    )


def _pdf_versions(s, ip):
    """Every PDF under Files as "<project>/<name>" -> (bytes, createdAt): a rewrite
    of an existing name shows as a changed value, not as nothing new."""
    files = s.get(_url(ip, "/api/files/list"), timeout=20).json()
    rows = files.get("files", files) if isinstance(files, dict) else files
    return {
        f"{f.get('project')}/{f.get('name')}": (f.get("bytes"), f.get("createdAt"))
        for f in rows
        if str(f.get("name", "")).lower().endswith(".pdf")
    }


@pytest.mark.agent
@pytest.mark.net
def test_pdf_recipe_end_to_end():
    from webrig import chat_turn

    want = os.environ.get("NIMBUS_L54_ROUTE")
    if want not in ROUTE_HOW:
        pytest.skip("set NIMBUS_L54_ROUTE to the route this unit is set up for (openai, anthropic or device)")
    ip, s = _lan()
    o = s.get(_url(ip, "/api/orch"), timeout=15).json()
    assert o.get("pdfRoute") == want, f"the unit's PDF route is {o.get('pdfRoute')}, not {want}"
    name = f"bench-{want}-{int(time.time())}.pdf"
    s.post(_url(ip, "/api/files/rm"), data={"project": RECIPE_PROJECT, "name": name}, timeout=10)
    before = _pdf_versions(s, ip)
    asked = f"{RECIPE_PROJECT}/{name}"
    assert asked not in before, f"{asked} is still under Files after the delete"
    st0 = s.get(_url(ip, "/api/state"), timeout=15).json()
    made = re.compile(MADE_BY[want])
    seen = set(made.findall(s.get(_url(ip, "/api/log"), timeout=20).text))  # lines from before this run
    reply = chat_turn(s, ip, recipe_prompt(want, name), timeout_s=420.0)
    # A sub-agent route lands its file after the turn (the job finishes later). The
    # log is a ring, so it is read on every pass: a capture line is kept even when
    # the ring rolls past it before the wait ends.
    deadline = time.time() + 600
    new, made_now = {}, set()
    while time.time() < deadline:
        made_now |= set(made.findall(s.get(_url(ip, "/api/log"), timeout=20).text)) - seen
        now = _pdf_versions(s, ip)
        new = {n: v for n, v in now.items() if before.get(n) != v}  # new, or rewritten since
        if made_now & set(new):  # this route's line names a file that is new
            break
        time.sleep(10)
    assert new, f"no new PDF under Files; reply: {reply[:300]!r}"
    assert made_now, f"no {want} line for a PDF in the log ({MADE_BY[want]}); new files: {sorted(new)}"
    assert made_now & set(new), f"the {want} line names {sorted(made_now)}, the new files are {sorted(new)}"
    got = sorted(made_now & set(new))[0]
    project, fname = got.split("/", 1)
    pdf = s.get(_url(ip, "/api/files/dl"), params={"project": project, "name": fname}, timeout=30)
    assert pdf.status_code == 200 and pdf.content.startswith(b"%PDF"), pdf.content[:16]
    if got != asked:
        print(f"[l54] the PDF was saved as {got}, not the asked {asked}")
    st1 = s.get(_url(ip, "/api/state"), timeout=15).json()
    assert st1["nvs"]["fails"] == st0["nvs"]["fails"], "a settings save failed during the PDF run"


@pytest.mark.agent
@pytest.mark.net
def test_pdf_request_without_an_sd_card_says_so():
    """No SD card: no route can keep the file, so the turn says why and makes nothing."""
    if os.environ.get("NIMBUS_L54_ROUTE") != "nostore":
        pytest.skip("set NIMBUS_L54_ROUTE=nostore on a unit with its SD card out")
    from webrig import chat_turn

    ip, s = _lan()
    st0 = s.get(_url(ip, "/api/state"), timeout=15).json()
    assert not st0.get("storeSD"), "the unit still reports an SD card"
    o = s.get(_url(ip, "/api/orch"), timeout=15).json()
    assert o.get("pdfRoute") == "nostore", o.get("pdfRoute")
    made = re.compile(r"files: captured \S+\.pdf|with the device PDF writer")
    made0 = len(made.findall(s.get(_url(ip, "/api/log"), timeout=20).text))
    reply = chat_turn(s, ip, recipe_prompt("device", f"bench-nostore-{int(time.time())}.pdf"), timeout_s=420.0)
    assert re.search(r"\bSD\b|card", reply, re.I), f"the reply does not say why: {reply[:300]!r}"
    made1 = len(made.findall(s.get(_url(ip, "/api/log"), timeout=20).text))
    assert made1 <= made0, "a PDF was made with no SD card"
    st1 = s.get(_url(ip, "/api/state"), timeout=15).json()
    assert st1["nvs"]["fails"] == st0["nvs"]["fails"], "a settings save failed during the run"
