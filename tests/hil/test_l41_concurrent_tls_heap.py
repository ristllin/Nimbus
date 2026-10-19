"""L41 - a cloud page load beside a turn never breaks the turn's TLS (bench legs).

The field failure (14 Oct, Lumi): a 375,598 B config page streamed through the cloud
relay for 26 s, outside the TLS work slot, while a head turn wrote 71 KB bodies; both
body writes failed ("0/71508"), the turn was failed over to another provider and the
relay redialed twice into the next rounds. With the relay inside the slot, the page
waits for the turn's exchanges (8 KB chunks, the slot released between them) or gets
a busy answer the cloud retries.

Device-side evidence, read off the serial console (every log line, healthy ones
included) while the legs run:
  * ``transport: heaptrace ... fail= af= min= at= ms=`` - one per exchange;
  * ``relay: bulk res ... at= ms= waited=`` - one per big tunneled frame;
  * ``tls: overlap <who>`` and ``/api/state.mem.tlsOverlaps`` - the concurrent
    work-TLS detector (the transport, the relay dial and every big-frame chunk mark
    their TLS I/O; a mark while another is live counts);
  * ``fabric: switching`` - must not appear.

Legs (unit A, test build, cloud-paired, Lumi-shaped corpus so round bodies reach
65-75 KB; ``hil`` + ``net`` + ``agent``):
  (a) a 4-round tool turn while the config page is fetched through the tunnel 3
      times. The CLOUD tunnel needs the owner's signed-in browser: the orchestrator
      runs lane X's probe (cumulo-nimbus tools/tunnel-bench/cloud-page-probe.js,
      ``cumuloCloudPageProbeStart({rounds: 24, everyMs: 5000})``) in the device's
      cloud tab and sets ``NIMBUS_L41_CLOUD_PROBE=1``. The test also replays
      ``GET /`` through the relay's loopback (console ``CLOUDLOOP /``) 3 times: a
      503 during the turn (the busy answer) and a 200 after it.
      Pass: no write failure (every trace fail=none, af=0), no switch, no overlap
      (tlsOverlaps unchanged, no bulk frame spanning an exchange it did not wait
      for), the page served or busy-then-served.
  (b) the counter-leg: ``FAULT relay_noslot on`` (the relay serves like v4.5.13)
      and the same cloud page loads must reproduce the overlap (tlsOverlaps moves),
      or the test could not fail.
  (c) fresh boot: after a restart the relay's first dial comes after Telegram's
      first successful poll, or 20 s after Wi-Fi (the two boot handshakes used to
      start within 30 ms of each other).
"""

from __future__ import annotations

import os
import re
import threading
import time

import pytest

import l41_evidence as ev

PROMPT = (
    "Search memory three times, once each for 'breakfast', 'car seat' and 'tea' "
    "(memory_search), then answer with one short sentence."
)
CLOUD_PROBE = os.environ.get("NIMBUS_L41_CLOUD_PROBE") == "1"


def _state(s, ip):
    r = s.get(f"http://{ip}/api/state", timeout=10)
    r.raise_for_status()
    return r.json()


def _start_turn(s, ip, text):
    r = s.post(f"http://{ip}/api/chat", data={"text": text}, timeout=10)
    assert r.status_code == 200, f"/api/chat -> {r.status_code}: {r.text[:160]}"
    return r.json().get("turn", "")


def _wait_turn(s, ip, turn, timeout_s=240):
    deadline = time.time() + timeout_s
    while time.time() < deadline:
        time.sleep(2)
        j = s.get(f"http://{ip}/api/chat", params={"turn": turn} if turn else None, timeout=10).json()
        if not j.get("pending", True) and (j.get("reply") or j.get("error")):
            return j
    pytest.fail(f"turn did not finish within {timeout_s}s")


class SerialTap:
    """Collect every console line in the background while a leg runs."""

    def __init__(self, device):
        self.device, self.lines, self._stop = device, [], threading.Event()
        self._t = threading.Thread(target=self._run, daemon=True)

    def _run(self):
        while not self._stop.is_set():
            ln = self.device._readline(time.time() + 0.5)
            if ln:
                self.lines.append(ln)

    def __enter__(self):
        self._t.start()
        return self

    def __exit__(self, *exc):
        self._stop.set()
        self._t.join(timeout=5)


def _run_turn_with_page_loads(device, s, ip):
    """One 4-round turn; 3 loopback page replays (CLOUDLOOP) during it, one after."""
    statuses = []
    with SerialTap(device) as tap:
        turn = _start_turn(s, ip, f"[l41] {PROMPT}")
        for _ in range(3):
            time.sleep(4)
            device.send("CLOUDLOOP /")
            time.sleep(6)
        reply = _wait_turn(s, ip, turn)
        device.send("CLOUDLOOP /")
        time.sleep(8)
    for ln in tap.lines:
        m = re.search(r"CLOUDLOOP / -> (-?\d+)", ln)
        if m:
            statuses.append(int(m.group(1)))
    return reply, statuses, ev.collect(tap.lines)


@pytest.mark.hil
@pytest.mark.net
@pytest.mark.agent
def test_l41_turn_beside_page_loads_no_overlap(device):
    from webrig import ip_tok_or_skip, make_session, require_orchestrator

    ip, tok = ip_tok_or_skip("L41a")
    s = make_session(tok)
    st0 = require_orchestrator(s, ip, "L41a")
    if st0.get("faults", {}).get("relay_noslot"):
        device.cmd("FAULT relay_noslot off", "FAULT relay_noslot=0", timeout=5)
    overlaps0 = int(st0["mem"]["tlsOverlaps"])

    reply, statuses, c = _run_turn_with_page_loads(device, s, ip)
    st1 = _state(s, ip)

    assert not reply.get("error"), f"the turn failed: {reply}"
    assert c["traces"], "no transport heaptrace line was captured (console tap or build mismatch)"
    bad = [t for t in c["traces"] if t.fail != "none" or t.af]
    assert not bad, f"exchanges that failed or saw a failed allocation: {bad}"
    assert not c["switches"], f"the fabric switched provider: {c['switches']}"
    assert int(st1["mem"]["tlsOverlaps"]) == overlaps0, f"concurrent work-TLS detected: {c['overlaps']}"
    assert not ev.bulk_overlaps(c["traces"], c["bulks"]), "a big relay frame overlapped a turn exchange"
    # The loopback replay: busy (503) while the turn ran, the page (200) after it.
    assert statuses and statuses[-1] == 200, f"the page was not served after the turn: {statuses}"
    assert all(x in (200, 503) for x in statuses), f"unexpected loopback statuses: {statuses}"
    big = [t.body for t in c["traces"] if t.body]
    print(f"L41a: bodies={big} statuses={statuses} bulks={len(c['bulks'])} busy={len(c['busy'])}")
    if not CLOUD_PROBE:
        pytest.skip(
            "device-side checks passed; the cloud page loads need the owner's browser probe "
            "(set NIMBUS_L41_CLOUD_PROBE=1 with lane X's cloud-page-probe running)"
        )
    assert c["bulks"] or c["busy"], "the cloud probe ran but no big tunneled frame or busy answer was seen"


@pytest.mark.hil
@pytest.mark.net
@pytest.mark.agent
def test_l41_counter_leg_without_the_slot_reproduces_the_overlap(device):
    if not CLOUD_PROBE:
        pytest.skip("needs the cloud page probe (NIMBUS_L41_CLOUD_PROBE=1): the overlap is a WebSocket write")
    from webrig import ip_tok_or_skip, make_session, require_orchestrator

    ip, tok = ip_tok_or_skip("L41b")
    s = make_session(tok)
    st0 = require_orchestrator(s, ip, "L41b")
    overlaps0 = int(st0["mem"]["tlsOverlaps"])
    device.cmd("FAULT relay_noslot on", "FAULT relay_noslot=1", timeout=5)
    try:
        _run_turn_with_page_loads(device, s, ip)
    finally:
        device.cmd("FAULT relay_noslot off", "FAULT relay_noslot=0", timeout=5)
    st1 = _state(s, ip)
    assert int(st1["mem"]["tlsOverlaps"]) > overlaps0, (
        "with the slot disabled the relay's page writes should overlap the turn's exchanges; "
        "no overlap means the probe did not load the page during the turn (rerun) or the detector is blind"
    )


@pytest.mark.hil
@pytest.mark.net
def test_l41_fresh_boot_first_dial_after_first_poll(device):
    """After a restart: 'relay: dial host=' comes after 'telegram: poll ok', or at
    least 20 s after Wi-Fi came up when no poll succeeded first."""
    from webrig import ip_tok_or_skip, make_session, require_orchestrator

    ip, tok = ip_tok_or_skip("L41c")
    s = make_session(tok)
    st = require_orchestrator(s, ip, "L41c")
    if not st.get("cloud", {}).get("paired"):
        pytest.skip("the unit is not paired to the cloud (the relay never dials)")
    # The console re-enumerates on a restart: reboot_and_confirm reopens it and reads
    # the boot stream up to READY; the Telegram poll and the relay dial come after.
    device.reboot_and_confirm()
    with SerialTap(device) as tap:
        deadline = time.time() + 90
        while time.time() < deadline and not any("relay: dial host=" in ln for ln in tap.lines):
            time.sleep(1)
    lines = tap.lines
    t0 = next((i for i, ln in enumerate(lines) if "telegram: poll start" in ln), None)
    ok = next((i for i, ln in enumerate(lines) if "telegram: poll ok" in ln), None)
    dial = next((i for i, ln in enumerate(lines) if "relay: dial host=" in ln), None)
    assert dial is not None, "the relay never dialed within 90 s of the restart"
    assert t0 is None or ok is None or ok < dial, (
        "the relay dialed before Telegram's first successful poll: the boot handshakes overlap"
    )
