"""L68 card downloads - a download never ends early while a tool holds the card.

Since v4.6.1 the web server never waits for the SD card (``nimbus/sys/web_guard.h``).
Its card downloads used to be library fillers that answered "try again" while a tool
held the card; the web library's in-flight credits ran out with nothing in flight and
AsyncTCP's 5 s acknowledgement timeout closed the connection, so the client saw the
download end early. A card download is now a card stream (``nimbus/net/card_stream.h``):
its open and every read only try the card lock, a pass that finds the card held sends
nothing, and the download waits out the hold (up to 60 s of waits) and completes.

``test_downloads_wait_out_a_card_hold`` (``hil`` + ``net``; the bench runs 10 runs):
each run holds the card for 30 s inside a tool call (the L68 recipe: ``TOOLSTALL`` with
a canned head round from ``PROVFAULT``, nothing billed) while ``/api/state`` is polled
every second, and downloads

* a 1.5 MB stored file, read slowly from before the hold, so the hold lands mid-file;
* the durable log (``/api/errlog``), 1 s into the hold;
* the same stored file, 3 s into the hold (its open waits for the card);
* an archived day as a ZIP (``/api/tg/archive/zip``), 5 s into the hold.

Every download must complete whole: the file byte for byte, the ZIP valid with the
seeded members byte for byte, the log at least the size it had when it opened and
starting with what a quiet read returned. Each must also show that the hold met it
(no vacuous pass): a download started during the hold gets its head only after the
hold, and the slow read pauses mid-file. No restart, every poll within 1 s. A busy
answer (503 + Retry-After, the device short of working memory) is retried the way the
page does and counted.

``test_past_the_budget_a_download_fails_clearly`` (one run): a 70 s hold, past the
stream's 60 s wait budget. The download started during the hold answers 503 with
Retry-After after about 60 s; the slow read already under way is cut (an error at the
client, never a clean short end); no restart, every poll within 1 s; a download after
the hold completes.

Setup seeds what it reads: the file through ``/api/files/upload`` (project
``card-dl-bench``, removed at the end) and two files in a test chat's archive day through
``/api/tg/archive/import`` (a rerun skips them). Parsers and verdicts are host-tested in
test_l68_card_downloads_host.py.
"""

from __future__ import annotations

import hashlib
import io
import os
import re
import threading
import time
import zipfile
from dataclasses import dataclass
from typing import Dict, List, Optional, Tuple

import pytest

from test_l68_web_stall import HOLD_MS

try:  # keep `pytest --collect-only` clean on a host without requests
    import requests
except ImportError:  # pragma: no cover
    requests = None

RUNS = int(os.environ.get("NIMBUS_CARD_DL_RUNS", "10"))
BIG_BYTES = 1_500_000
FILE_PROJECT, FILE_NAME = "card-dl-bench", "card-stream.bin"
ARC_CHAT, ARC_DAY = "990000820", "2026-10-27"
ARC_FILES = (("r0000000000000821.bin", 300_000), ("r0000000000000822.bin", 60_000))
FILE_PATH = f"/api/files/dl?project={FILE_PROJECT}&name={FILE_NAME}"
ZIP_PATH = f"/api/tg/archive/zip?chat={ARC_CHAT}&day={ARC_DAY}"
PATHS = {"errlog": "/api/errlog", "file": FILE_PATH, "zip": ZIP_PATH}
IN_HOLD = ((1.0, "errlog"), (3.0, "file"), (5.0, "zip"))  # seconds into the hold
SLOW_CHUNK, SLOW_PAUSE_S = 4096, 0.1  # about 40 KB/s: the slow read spans the hold
HOLD_SLACK_S = 4.0  # the hold starts a little before its console line is read
CARD_WAIT_BUDGET_S = 60.0  # nimbus::net::kCardStreamWaitMs
PAST_BUDGET_HOLD_MS = 70000  # past the budget (TOOLSTALL takes up to 120 s)
MID_GAP_S = 5.0  # the slow read's longest pause between chunks: the hold met it mid-file
READ_TIMEOUT_S = 90.0  # longer than the stream's 60 s wait budget
BUSY_TRIES = 3
WAITED_RE = re.compile(
    r"web: a card download waited (?P<ms>\d+) ms for the card \((?P<holds>\d+) holds, (?P<bytes>\d+) B sent\)"
)
GAVE_UP_RE = re.compile(r"web: a card download gave up after waiting (\d+) ms")


# ---- pure helpers and verdicts (host-tested) ---------------------------------------
def payload(n: int, seed: int) -> bytes:
    """``n`` deterministic, incompressible bytes (a SHA-256 counter stream)."""
    out = bytearray()
    i = 0
    while len(out) < n:
        out += hashlib.sha256(f"{seed}:{i}".encode()).digest()
        i += 1
    return bytes(out[:n])


def sha(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def transport_failure(rec: dict) -> Optional[str]:
    """Why a download did not arrive as a 200 read to its end, or None."""
    if rec.get("error"):
        return f"ended early: {rec['error']} after {rec.get('bytes', 0)} B"
    if rec.get("status") != 200:
        return f"status {rec.get('status')}"
    return None


def body_verdict(rec: dict, want_sha: str, want_len: int) -> Tuple[bool, str]:
    """A stored file: 200, whole, byte for byte."""
    bad = transport_failure(rec)
    if bad:
        return False, bad
    if rec.get("bytes") != want_len or rec.get("sha") != want_sha:
        return False, f"{rec.get('bytes')} B (want {want_len}), sha {str(rec.get('sha'))[:12]}"
    return True, f"whole in {rec.get('ms')} ms"


def zip_verdict(rec: dict, body: bytes, members: Dict[str, str]) -> Tuple[bool, str]:
    """An archived day: 200, a valid ZIP whose seeded members match byte for byte (the
    day's index file rides along)."""
    bad = transport_failure(rec)
    if bad:
        return False, bad
    try:
        z = zipfile.ZipFile(io.BytesIO(body))
        bad = z.testzip()
    except zipfile.BadZipFile as exc:
        return False, f"not a ZIP: {exc}"
    if bad:
        return False, f"member {bad} fails its CRC"
    for name, digest in members.items():
        if name not in z.namelist():
            return False, f"member {name} missing"
        if sha(z.read(name)) != digest:
            return False, f"member {name} differs"
    return True, f"{len(z.namelist())} members in {rec.get('ms')} ms"


def errlog_verdict(rec: dict, body: bytes, ref: bytes) -> Tuple[bool, str]:
    """The active log grows while it streams: at least the size it opened with
    (X-File-Bytes), and the same start as a quiet read (unless it rotated meanwhile)."""
    bad = transport_failure(rec)
    if bad:
        return False, bad
    opened = int(rec.get("fileBytes") or 0)
    if len(body) < opened:
        return False, f"{len(body)} B, less than the {opened} B it opened with"
    head = ref[: min(len(ref), 4096)]
    if opened >= len(ref) and not body.startswith(head):
        return False, "does not start like the quiet read"
    return True, f"{len(body)} B (opened at {opened}) in {rec.get('ms')} ms"


def waited_verdict(kind: str, rec: dict, started_in_hold_s: Optional[float]) -> Tuple[bool, str]:
    """Proof that the hold met the download (no vacuous pass): a download started
    ``started_in_hold_s`` into the hold gets its head only after the hold (its open
    waited); the slow read started before the hold pauses mid-file."""
    if started_in_hold_s is None:
        ok = rec.get("gapMs", 0) >= MID_GAP_S * 1000
        return ok, f"{kind}: longest pause {rec.get('gapMs')} ms (want {MID_GAP_S:.0f} s or more)"
    want = (HOLD_MS / 1000.0 - started_in_hold_s - HOLD_SLACK_S) * 1000
    ok = rec.get("headMs", 0) >= want
    return ok, f"{kind}: head after {rec.get('headMs')} ms (want {want:.0f} ms or more)"


def past_budget_verdict(door: dict, mid: dict, budget_s: float, full_len: int) -> Tuple[bool, str]:
    """Past the stream's wait budget the failure is clear: a download that had sent
    nothing gets 503 with Retry-After after about the budget, and one under way is cut
    (an error at the client, never a clean end that is short)."""
    if door.get("status") != 503 or not door.get("retryAfter"):
        return False, f"door download: status {door.get('status')} retryAfter {door.get('retryAfter')}"
    if door.get("headMs", 0) < (budget_s - HOLD_SLACK_S) * 1000:
        return False, f"door download answered after {door.get('headMs')} ms, before the budget"
    if not mid.get("error") and mid.get("bytes") != full_len:
        return False, f"mid-file download ended clean but short: {mid.get('bytes')} of {full_len} B"
    if not mid.get("error"):
        return False, "mid-file download was not cut (did the hold outlast the budget?)"
    return True, f"503 after {door.get('headMs')} ms; mid-file cut ({mid.get('error')}) after {mid.get('bytes')} B"


def waited_lines(log_text: str) -> List[dict]:
    """The device's lines for downloads that waited for the card (GET /api/log)."""
    return [{k: int(v) for k, v in m.groupdict().items()} for m in WAITED_RE.finditer(log_text or "")]


def run_verdict(checks: List[Tuple[str, bool, str]]) -> Tuple[bool, str]:
    bad = [f"{k}: {why}" for k, ok, why in checks if not ok]
    return (not bad), ("; ".join(bad) if bad else f"{len(checks)} checks passed")


# ---- device helpers ------------------------------------------------------------------
def _headers(net) -> dict:
    tok = net.token()
    return {"X-Nimbus-Token": str(tok)} if tok else {}


def _url(ip: str, path: str) -> str:
    return f"http://{ip}{path}"


def _fetch(net, ip: str, path: str, slow: bool, busy_tries: int = BUSY_TRIES) -> Tuple[dict, bytes]:
    """One streamed GET, waiting out busy answers (503 + Retry-After) like the page."""
    rec: dict = {"path": path, "busy": 0}
    t0 = time.time()
    for _ in range(busy_tries + 1):
        r = requests.get(_url(ip, path), headers=_headers(net), stream=True, timeout=(10.0, READ_TIMEOUT_S))
        rec["retryAfter"] = r.headers.get("Retry-After")
        if r.status_code != 503 or not rec["retryAfter"] or _ == busy_tries:
            break
        rec["busy"] += 1
        r.close()
        time.sleep(min(30.0, float(r.headers.get("Retry-After") or 5)))
    rec["status"] = r.status_code
    rec["fileBytes"] = r.headers.get("X-File-Bytes")
    rec["headMs"] = int((time.time() - t0) * 1000)  # the head comes once the file opened
    body = bytearray()
    last, gap = time.time(), 0.0
    try:
        for chunk in r.iter_content(SLOW_CHUNK if slow else 65536):
            body += chunk
            now = time.time()
            gap, last = max(gap, now - last), now
            if slow and gap < MID_GAP_S:  # slow until the hold has met it mid-file
                time.sleep(SLOW_PAUSE_S)
                last = time.time()
    except Exception as exc:  # noqa: BLE001 - an early end is the verdict, recorded
        rec["error"] = type(exc).__name__
    finally:
        r.close()
    rec["ms"] = int((time.time() - t0) * 1000)
    rec["gapMs"] = int(gap * 1000)
    rec["bytes"] = len(body)
    rec["sha"] = sha(bytes(body))
    return rec, bytes(body)


class Download(threading.Thread):
    """One download on its own thread; ``slow`` and ``busy_tries`` go to _fetch."""

    def __init__(self, net, ip: str, kind: str, path: str, **opts):
        super().__init__(daemon=True)
        self.net, self.ip, self.kind, self.path = net, ip, kind, path
        self.slow, self.busy_tries = bool(opts.get("slow")), int(opts.get("busy_tries", BUSY_TRIES))
        self.rec: dict = {"kind": kind, "error": "never ran"}
        self.body = b""

    def run(self) -> None:
        try:
            self.rec, self.body = _fetch(self.net, self.ip, self.path, self.slow, self.busy_tries)
        except Exception as exc:  # noqa: BLE001 - a connect failure is a verdict too
            self.rec = {"path": self.path, "error": f"{type(exc).__name__}: {exc}"}
        self.rec["kind"] = self.kind


def _seed_file(net, ip: str, data: bytes) -> None:
    for _ in range(5):
        r = requests.post(
            _url(ip, f"/api/files/upload?project={FILE_PROJECT}&name={FILE_NAME}"),
            headers=_headers(net),
            files={"file": (FILE_NAME, data, "application/octet-stream")},
            timeout=120.0,
        )
        if r.status_code == 200:
            return
        time.sleep(5.0)
    pytest.fail(f"could not seed {FILE_NAME}: {r.status_code} {r.text[:160]}")


def _seed_archive(net, ip: str, files: Dict[str, bytes]) -> None:
    for name, data in files.items():
        q = f"chat={ARC_CHAT}&day={ARC_DAY}&name={name}&kind=document&role=in&date=1793102400&note=carddl"
        for _ in range(5):
            h = dict(_headers(net), **{"Content-Type": "application/octet-stream"})
            r = requests.post(_url(ip, "/api/tg/archive/import?" + q), headers=h, data=data, timeout=60.0)
            if r.status_code == 200:
                break
            time.sleep(5.0)
        else:
            pytest.fail(f"could not seed the archive file {name}: {r.status_code} {r.text[:160]}")


def _quiet_get(net, ip: str, path: str) -> bytes:
    rec, body = _fetch(net, ip, path, slow=False)
    assert rec["status"] == 200 and not rec.get("error"), f"quiet read of {path}: {rec}"
    return body


def _ram_log(net, ip: str) -> str:
    try:
        return net.get("/api/log", ip=ip, timeout=10.0).text
    except Exception:  # noqa: BLE001 - the verdict says the lines are missing
        return ""


@dataclass
class HoldPlan:
    """One hold: its length, the downloads started into it, the busy answers each retries."""

    hold_ms: int = HOLD_MS
    in_hold: tuple = IN_HOLD
    busy_tries: int = BUSY_TRIES


def _hold_and_download(bench: tuple, poll_samples: list, plan: HoldPlan = HoldPlan()) -> "tuple[List[Download], bool]":
    """``bench`` is (device, net, ip, prio)."""
    from test_l68_web_stall import STALL_TOOL, StatePoller, _cleanup, _console_until, _start_turn, _wait_turn_end
    from test_l68_web_stall import tool_round

    device, net, ip, prio = bench
    turn, ended, downloads = "", None, []
    try:
        with StatePoller(net, ip) as poll:
            downloads.append(Download(net, ip, "file-mid", FILE_PATH, slow=True))
            downloads[0].start()
            time.sleep(1.0)  # the slow read is under way before the hold
            stall = (plan.hold_ms, tool_round(STALL_TOOL, {"action": "view"}))
            turn = _start_turn(device, net, ip, "anthropic", stall, "L68 card download probe: look at your scratchpad.")
            lines = _console_until(device, r"TOOLSTALL:", 60.0)
            assert any("TOOLSTALL:" in ln for ln in lines), "the stall never started (did the tool run?)"
            t_hold = time.time()
            poll_samples.append({"holdAt": t_hold})  # a marker for the poll report
            for at, kind in plan.in_hold:
                time.sleep(max(0.0, t_hold + at - time.time()))
                d = Download(net, ip, kind, PATHS[kind], busy_tries=plan.busy_tries)
                d.start()
                downloads.append(d)
            for d in downloads:
                d.join(timeout=READ_TIMEOUT_S + 60.0)
            time.sleep(2.0)
        poll_samples.extend(poll.samples)
    finally:
        ended = _wait_turn_end(net, ip, turn)
        _cleanup(device, net, ip, prio, provfault_off=ended)
    return downloads, bool(ended)


def _report_slow_polls(samples: list, over_ms: int = 500) -> None:
    """Every /api/state poll slower than ``over_ms``, placed against the hold it met."""
    hold = None
    for x in sorted(samples, key=lambda v: v.get("holdAt") or v.get("t") or 0):
        if "holdAt" in x:
            hold = x["holdAt"]
        elif x.get("ms", 0) > over_ms or x.get("status") != 200:
            at = f"{x['t'] - hold:+.1f} s from the hold's start" if hold else "before any hold"
            print(f"[L68dl] slow poll {at}: {x}")


def _run_checks(downloads: List[Download], want: dict, ref: bytes, log_text: str) -> List[Tuple[str, bool, str]]:
    checks = []
    for d in downloads:
        if d.kind in ("file", "file-mid"):
            ok, why = body_verdict(d.rec, want["file_sha"], want["file_len"])
        elif d.kind == "zip":
            ok, why = zip_verdict(d.rec, d.body, want["members"])
        else:
            ok, why = errlog_verdict(d.rec, d.body, ref)
        checks.append((d.kind, ok, why + (f" ({d.rec.get('busy')} busy answers)" if d.rec.get("busy") else "")))
    starts = {kind: at for at, kind in IN_HOLD}
    for d in downloads:
        ok, why = waited_verdict(d.kind, d.rec, starts.get(d.kind))
        checks.append(("waited", ok, why))
    # The RAM log ring is 1,280 B, so the device's own lines are reported, not required.
    print(
        f"[L68dl] device lines: {len(waited_lines(log_text))} waited, gave up: {bool(GAVE_UP_RE.search(log_text or ''))}"
    )
    checks.append(("gave up", not GAVE_UP_RE.search(log_text or ""), "a download gave up"))
    return checks


# ---- HIL leg -----------------------------------------------------------------------
@pytest.mark.hil
@pytest.mark.net
def test_downloads_wait_out_a_card_hold(device, net):
    from test_l39_crash_forensics import _boot_n, _lan_ip
    from test_l68_web_stall import _before, _orch_prio, _webguard, state_verdict, web_guard_verdict

    if requests is None:
        pytest.skip("requests not installed")
    if _before():
        pytest.skip("NIMBUS_L68_BEFORE=1: the v4.6.0 image has no card streams")
    ip = _lan_ip(None)
    big = payload(BIG_BYTES, 820)
    arc = {name: payload(n, 821 + i) for i, (name, n) in enumerate(ARC_FILES)}
    _seed_file(net, ip, big)
    _seed_archive(net, ip, arc)
    want = {"file_sha": sha(big), "file_len": len(big), "members": {k: sha(v) for k, v in arc.items()}}
    assert sha(_quiet_get(net, ip, FILE_PATH)) == want["file_sha"], "the seeded file does not read back"
    before_n = _boot_n(net, ip)
    wg0 = _webguard(device)
    prio = _orch_prio(net, ip)
    failures, samples = [], []
    try:
        for run in range(RUNS):
            ref = _quiet_get(net, ip, "/api/errlog")
            downloads, ended = _hold_and_download((device, net, ip, prio), samples)
            checks = _run_checks(downloads, want, ref, _ram_log(net, ip))
            ok, why = run_verdict(checks)
            for d in downloads:
                print(f"[L68dl] run {run + 1} {d.kind}: {d.rec}")
            print(f"[L68dl] run {run + 1}/{RUNS}: {'PASS' if ok else 'FAIL'} {why}")
            if not ok:
                failures.append(f"run {run + 1}: {why}")
            assert ended, "the probe turn was still running; PROVFAULT is left armed (restart the unit to clear it)"
            assert _boot_n(net, ip) == before_n, f"the board restarted during run {run + 1}"
        wg1 = _webguard(device)
        print(f"[L68dl] webguard before={wg0} after={wg1} polls={len(samples)}")
        polls = [x for x in samples if "holdAt" not in x]
        _report_slow_polls(samples)
        ok, why = state_verdict(polls, expect_busy=True)
        print(f"[L68dl] /api/state: {why}")
        assert ok, why
        ok, why = web_guard_verdict(wg0, wg1, expect_busy=True)
        assert ok, why
        assert not failures, "; ".join(failures)
    finally:
        try:
            net.post("/api/files/rm", {"project": FILE_PROJECT, "name": FILE_NAME}, ip=ip, timeout=10.0)
        except Exception as exc:  # noqa: BLE001 - reported; the next run re-seeds it
            print(f"[L68dl] cleanup: {exc}")


@pytest.mark.hil
@pytest.mark.net
def test_past_the_budget_a_download_fails_clearly(device, net):
    """One 70 s hold, past the 60 s wait budget: the download started during the hold
    answers 503 + Retry-After, the slow one already under way is cut, the board does not
    restart and /api/state answers within 1 s throughout. A download after it completes."""
    from test_l39_crash_forensics import _boot_n, _lan_ip
    from test_l68_web_stall import _before, _orch_prio, state_verdict

    if requests is None:
        pytest.skip("requests not installed")
    if _before():
        pytest.skip("NIMBUS_L68_BEFORE=1: the v4.6.0 image has no card streams")
    ip = _lan_ip(None)
    big = payload(BIG_BYTES, 820)
    _seed_file(net, ip, big)
    before_n = _boot_n(net, ip)
    prio = _orch_prio(net, ip)
    samples: list = []
    try:
        plan = HoldPlan(hold_ms=PAST_BUDGET_HOLD_MS, in_hold=((2.0, "file"),), busy_tries=0)
        downloads, ended = _hold_and_download((device, net, ip, prio), samples, plan)
        for d in downloads:
            print(f"[L68dl] past budget {d.kind}: {d.rec}")
        mid = next(d.rec for d in downloads if d.kind == "file-mid")
        door = next(d.rec for d in downloads if d.kind == "file")
        ok, why = past_budget_verdict(door, mid, CARD_WAIT_BUDGET_S - 2.0, len(big))
        print(f"[L68dl] past budget: {'PASS' if ok else 'FAIL'} {why}")
        assert ended, "the probe turn was still running; PROVFAULT is left armed (restart the unit to clear it)"
        assert _boot_n(net, ip) == before_n, "the board restarted during the long hold"
        _report_slow_polls(samples)
        ok_state, why_state = state_verdict([x for x in samples if "holdAt" not in x], expect_busy=True)
        print(f"[L68dl] /api/state: {why_state}")
        assert ok, why
        assert ok_state, why_state
        rec, body = _fetch(net, ip, FILE_PATH, slow=False)
        assert body_verdict(rec, sha(big), len(big))[0], f"the download after the hold: {rec}"
    finally:
        try:
            net.post("/api/files/rm", {"project": FILE_PROJECT, "name": FILE_NAME}, ip=ip, timeout=10.0)
        except Exception as exc:  # noqa: BLE001 - reported; the next run re-seeds it
            print(f"[L68dl] cleanup: {exc}")
