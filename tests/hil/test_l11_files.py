"""§L11 - E1 artifact store (SD file pipeline) LAN tests.

Exercises the token-gated ``/api/files/*`` surface on real hardware (ROADMAP E1):
upload (multipart streamed to SD) → list → download (byte-identical) → delete, plus
the security-critical properties - auth gate, path-traversal rejection, the route is
NOT prefix-swallowed, and graceful degradation under ``FAULT sd``.

These codify the hand-run device smoke that first shipped E1 (and caught the
``/api/files`` prefix-collision + the off-Lock concurrency bugs the adversarial review
found). Every test RESTORES what it created (delete uploads, clear faults) in a
``finally`` - a leaked file or fault would cascade into downstream tests.

Auth rides the ``X-Nimbus-Token`` header (CUM-45 took the token out of URLs; a
``?t=`` query param is not read by the firmware). Markers: ``@pytest.mark.net``
(LAN + serial for the token).
"""

from __future__ import annotations

import hashlib
import os

import pytest

from test_l4_network import lan_ip_or_skip
from test_l9_resilience import _webtok

try:  # keep `pytest --collect-only` clean on a host without requests
    import requests
except ImportError:  # pragma: no cover
    requests = None

PROJECT = "hiltest"


def _need_requests():
    if requests is None:
        pytest.skip("requests not installed")


def _hdr(tok: str) -> dict:
    # CUM-45 took the token out of URLs: the web fetch shim + the HIL harness send
    # it on the X-Nimbus-Token header (a form-body `t` also works on POSTs). A `?t=`
    # query param is NOT read by the firmware, so raw requests must carry the header.
    return {"X-Nimbus-Token": tok}


def _url(ip: str, path: str, tok: str, **params) -> str:
    # Token rides the header (see _hdr); only non-secret params go in the URL.
    q = "&".join(f"{k}={v}" for k, v in params.items())
    return f"http://{ip}{path}" + (f"?{q}" if q else "")


def _upload(ip, tok, project, name, data, timeout=20):
    return requests.post(
        _url(ip, "/api/files/upload", tok, project=project, name=name),
        files={"file": (name, data)},
        headers=_hdr(tok),
        timeout=timeout,
    )


def _list(net, ip, tok, project=""):
    path = "/api/files/list" + (f"?project={project}" if project else "")
    return net.get_json(path, ip=ip, timeout=8.0)


def _rm(net, ip, tok, project, name):
    return net.post("/api/files/rm", {"project": project, "name": name, "t": tok}, ip=ip)


def _state(net, ip, tok) -> dict:
    """/api/state is token-gated (since owner-batch-2 every /api GET is). The harness
    authenticates it on the X-Nimbus-Token header (net adds it)."""
    return net.get_json("/api/state", ip=ip, timeout=6.0)


# ---- auth gate -------------------------------------------------------------
@pytest.mark.net
def test_files_requires_token(device, net, secrets, require_secret):
    """File names + contents are private: every /api/files route needs the token."""
    _need_requests()
    ip = lan_ip_or_skip(device, net, secrets, require_secret)
    assert net.get("/api/files/list", ip=ip, auth=False).status_code == 401
    # upload with no token must not write a byte
    r = requests.post(
        f"http://{ip}/api/files/upload?project=evil&name=x.txt", files={"file": ("x.txt", b"hi")}, timeout=10
    )
    assert r.status_code == 401, f"unauthenticated upload -> {r.status_code}, want 401"


# ---- /api/state block ------------------------------------------------------
@pytest.mark.net
def test_files_state_block(device, net, secrets, require_secret):
    """/api/state carries a files{present,count,bytes} block."""
    ip = lan_ip_or_skip(device, net, secrets, require_secret)
    tok = _webtok(device)
    st = _state(net, ip, tok)
    assert "files" in st, "state missing files{} block"
    f = st["files"]
    assert "present" in f and "count" in f and "bytes" in f
    # CUM-7: the state block also carries the quota + card-free truths + the
    # unsupported flag (additive; present regardless of whether a card is in).
    assert "quota" in f and "cardFree" in f and "unsupported" in f


@pytest.mark.net
def test_files_list_quota_truths(device, net, secrets, require_secret):
    """CUM-7: /api/files/list exposes the four distinct storage truths and the
    unsupported-card flag. With a supported card, quota = card - 512 MB reserve,
    so cardFree/quota/used are all present and quota does not exceed cardTotal."""
    ip = lan_ip_or_skip(device, net, secrets, require_secret)
    tok = _webtok(device)
    d = _list(net, ip, tok)
    if not d.get("present"):
        # absent OR unsupported (<1GB) card: the unsupported flag disambiguates
        assert "unsupported" in d or d.get("present") is False
        pytest.skip("no supported card mounted")
    for k in ("count", "bytes", "quota", "cardTotal", "cardFree", "freeBytes", "unsupported"):
        assert k in d, f"/api/files/list missing {k}: {list(d)}"
    assert d["quota"] <= d["cardTotal"], "quota exceeds the card capacity"
    assert d["bytes"] <= d["quota"] or d["quota"] == 0, "used exceeds quota"


# ---- upload → list → download → delete -------------------------------------
@pytest.mark.net
def test_files_text_roundtrip(device, net, secrets, require_secret):
    _need_requests()
    ip = lan_ip_or_skip(device, net, secrets, require_secret)
    tok = _webtok(device)
    if not _list(net, ip, tok).get("present"):
        pytest.skip("no SD card mounted - artifact store absent")
    name = "l11_text.txt"
    body = b"Nimbus E1 HIL: the quick brown fox.\nLine two 1234567890.\n"
    try:
        r = _upload(ip, tok, PROJECT, name, body)
        assert r.status_code == 200, f"upload -> {r.status_code}: {r.text[:120]}"
        lst = _list(net, ip, tok, PROJECT)
        names = [e["name"] for e in lst["files"]]
        assert name in names, f"uploaded file not listed: {names}"
        got = requests.get(_url(ip, "/api/files/dl", tok, project=PROJECT, name=name), headers=_hdr(tok), timeout=15)
        assert got.status_code == 200
        assert got.content == body, "download not byte-identical"
    finally:
        _rm(net, ip, tok, PROJECT, name)


# ---- multi-chunk binary (streaming path) -----------------------------------
@pytest.mark.net
def test_files_binary_multichunk_roundtrip(device, net, secrets, require_secret):
    _need_requests()
    ip = lan_ip_or_skip(device, net, secrets, require_secret)
    tok = _webtok(device)
    if not _list(net, ip, tok).get("present"):
        pytest.skip("no SD card mounted - artifact store absent")
    name = "l11_blob.bin"
    blob = os.urandom(256 * 1024)  # spans many upload chunks
    want = hashlib.md5(blob).hexdigest()
    try:
        r = _upload(ip, tok, PROJECT, name, blob, timeout=60)
        assert r.status_code == 200, f"upload -> {r.status_code}: {r.text[:120]}"
        got = requests.get(_url(ip, "/api/files/dl", tok, project=PROJECT, name=name), headers=_hdr(tok), timeout=60)
        assert got.status_code == 200
        assert hashlib.md5(got.content).hexdigest() == want, "256KB round-trip corrupted"
    finally:
        _rm(net, ip, tok, PROJECT, name)


# ---- route is NOT prefix-swallowed (regression) ----------------------------
@pytest.mark.net
def test_files_dl_route_not_swallowed_by_list(device, net, secrets, require_secret):
    """GET /api/files/dl must return the FILE, not the /api/files listing JSON -
    the ESPAsyncWebServer prefix-match bug that first shipped E1 (a bare
    ``/api/files`` GET handler swallowed ``/api/files/dl``)."""
    _need_requests()
    ip = lan_ip_or_skip(device, net, secrets, require_secret)
    tok = _webtok(device)
    if not _list(net, ip, tok).get("present"):
        pytest.skip("no SD card mounted - artifact store absent")
    name = "l11_route.txt"
    body = b"ROUTE-MARKER-9f3a not-a-json-listing"
    try:
        assert _upload(ip, tok, PROJECT, name, body).status_code == 200
        got = requests.get(_url(ip, "/api/files/dl", tok, project=PROJECT, name=name), headers=_hdr(tok), timeout=15)
        assert got.content == body
        assert b'"present"' not in got.content, "dl returned the listing JSON - route swallowed"
    finally:
        _rm(net, ip, tok, PROJECT, name)


# ---- path traversal --------------------------------------------------------
@pytest.mark.net
def test_files_path_traversal_rejected(device, net, secrets, require_secret):
    """validSegment gates every consumer: traversal names/projects are refused and
    a traversal download 404s - nothing escapes /mem/files."""
    _need_requests()
    ip = lan_ip_or_skip(device, net, secrets, require_secret)
    tok = _webtok(device)
    if not _list(net, ip, tok).get("present"):
        pytest.skip("no SD card mounted - artifact store absent")
    # name traversal
    r = _upload(ip, tok, PROJECT, "../../etc/x", b"x")
    assert r.status_code >= 400, f"traversal name accepted -> {r.status_code}"
    # project traversal
    r = _upload(ip, tok, "..", "x.txt", b"x")
    assert r.status_code >= 400, f"traversal project accepted -> {r.status_code}"
    # download traversal
    got = requests.get(
        _url(ip, "/api/files/dl", tok, project=PROJECT, name="../../../etc/passwd"), headers=_hdr(tok), timeout=10
    )
    assert got.status_code == 404, f"traversal dl -> {got.status_code}, want 404"


# ---- graceful degradation under FAULT sd -----------------------------------
@pytest.mark.net
def test_files_fault_sd_degrades_and_recovers(device, net, secrets, require_secret):
    """FAULT sd drops the store to present:false, upload fails clean (no crash), and
    clearing the fault restores it - device serving throughout."""
    _need_requests()
    ip = lan_ip_or_skip(device, net, secrets, require_secret)
    tok = _webtok(device)
    if not _list(net, ip, tok).get("present"):
        pytest.skip("no SD card mounted - artifact store absent")
    try:
        net.post("/api/fault", {"cap": "sd", "on": 1, "t": tok}, ip=ip)
        st = _state(net, ip, tok)
        assert st["files"]["present"] is False, "store still present under FAULT sd"
        r = _upload(ip, tok, PROJECT, "l11_faulted.txt", b"x")
        assert r.status_code >= 400, f"upload under FAULT sd -> {r.status_code}, want failure"
    finally:
        net.post("/api/fault", {"cap": "all", "t": tok}, ip=ip)
    # recovery: device still up + store back
    st = _state(net, ip, tok)
    assert st["files"]["present"] is True, "store did not recover after clearing FAULT sd"
