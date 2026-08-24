"""§L3 - HIL device/serial tests (the HIL test spec).

Boot, selftest gate, render-state, notifier broker E2E.
All drive the NIMBUS_TEST serial affordances (plus the existing NIMBUS_NOTIFIER_DEBUG
``NSN ..`` lines); none use a camera.

Ring semantics pinned by lib/core/src/ring_plan.cpp:
  * Active/"Desk" posture (posture=1): ring = seg:<job count>,
  * Passive posture (posture=0):       ring = dark (at most one attention LED = single).

Posture is a device-side precondition; a test whose scenario needs a specific posture
does a LOUD skip naming the posture to set, never a green-by-default.

Gated behind --allow-hardware (see conftest.py). On a device-less box every test here
is a LOUD, REASONED skip.
"""

from __future__ import annotations

import re
import time

import pytest

from device import ExpectTimeout

POSTURE_PASSIVE = 0
POSTURE_ACTIVE = 1  # "Desk" in the plan's UX vocabulary


# ---- boot_ok (F11, F14) ----------------------------------------------------
@pytest.mark.hil
def test_boot_ok(device):
    """boot_ok (F11, F14): reset -> ``READY mode=<n> ip=<..>`` within 20 s, with NO
    ``Guru Meditation`` / ``abort()`` / ``assert failed`` and NO reboot loop
    (>=2 ``rst:`` lines) in the captured boot stream. wait_ready() FAILS on any
    panic marker or rst-loop and only succeeds on the beacon - turning "boots in the
    other mode" from a claim into an assertion.

    A clean beacon over a wedged loop is still a brick, so PING->PONG proves the loop
    is actually running."""
    device.reset()  # pulse reset + reopen across the F13 re-enumeration
    mode, _ip = device.wait_ready(timeout=20.0)
    assert mode in (0, 1, None), f"unexpected boot mode {mode!r}"
    assert device.ping(), "device beaconed READY but PING got no PONG (wedged loop)"


# ---- selftest_gate (F5) ----------------------------------------------------
@pytest.mark.hil
def test_selftest_gate(device):
    """selftest_gate (F5): ``TEST all`` -> a PASS/SKIP verdict; any SKIP must carry a
    REASON token (e.g. ``sd:absent``), never a bare SKIP - no silent aggregate."""
    m = device.selftest("all", timeout=30.0)
    verdict, rest = m.group("verdict"), m.group("rest")
    assert verdict in ("PASS", "SKIP"), f"selftest all FAILED: {m.group(0)}"
    if verdict == "SKIP":
        assert re.search(r"\b\w+:\w+", rest), f"bare SKIP with no reason token: {m.group(0)!r}"


# ---- render_state (F2, F4) -------------------------------------------------
@pytest.mark.hil
def test_render_three_jobs_desk(device, nsn):
    """render_state (F2, F4): drive 3 nsn Running jobs; in Active/"Desk" posture the
    ring must report ``seg:3``. LOUD-skip in Passive posture (which would correctly
    show ``dark`` and mask the assertion)."""
    device.ensure_mode(0)  # nsn frames need Notifier mode
    # no reboot: state comes from the frames we send; reboot churn is the wedge
    # correlate (see device.py::_open_quiet doc).

    r0 = device.render(timeout=6.0)
    if r0.posture != POSTURE_ACTIVE:
        pytest.skip(
            f"device posture={r0.posture} (Passive); set Active/Desk posture for the seg-count assertion (F2/F4)"
        )

    nsn.send_frame([nsn.running(170), nsn.running(85), nsn.running(20)], brightness=90)
    time.sleep(1.0)  # let the ring recompose (BLE write latency > serial's)

    r = device.render(timeout=6.0)
    assert r.ring == "seg:3", (
        f"ring={r.ring!r}, expected 'seg:3' for 3 jobs in Active posture (F2/F4 - ring never asserted on-device before)"
    )


@pytest.mark.hil
def test_render_passive_dark(device, nsn):
    """render_state (F2, F4): in Passive posture with NO attention jobs the ring is
    dark - the exact "looks dead but is fine" state behind F2. Clear all jobs, then
    assert ``ring=dark``. LOUD-skip in Active posture (which shows ``seg:0``)."""
    device.ensure_mode(0)  # nsn frames need Notifier mode
    # no reboot: state comes from the frames we send; reboot churn is the wedge
    # correlate (see device.py::_open_quiet doc).

    r0 = device.render(timeout=6.0)
    if r0.posture != POSTURE_PASSIVE:
        pytest.skip(f"device posture={r0.posture} (Active); set Passive posture for the dark-ring assertion (F2/F4)")

    nsn.send_frame([], brightness=0)  # clear all
    time.sleep(1.0)  # BLE write latency > serial's

    r = device.render(timeout=6.0)
    assert r.ring == "dark", (
        f"ring={r.ring!r}, expected 'dark' for Passive + no attention (F2 - the state that looked dead)"
    )


# ---- notifier_broker_e2e (F2, F4) ------------------------------------------
@pytest.mark.hil
def test_notifier_broker_e2e(device, nsn):
    """notifier_broker_e2e: the three tools/nsn_send.py scenarios drive distinct,
    asserted device state via the REAL broker encoder:

      scenario       frame                              jobs  attn
      -------------  ---------------------------------  ----  ----
      two running    2x Running                          2     0
      + approval     2x Running + 1 AwaitingApproval     3     1
      clear all      empty                               0     0

    For each, assert the ``NSN jobs=<n> attn=<a>`` echo matches; and in Active posture
    the ``RENDER? ring=seg:N`` count matches the job count (attention adds no
    segment; it lights ``single`` in Passive)."""
    device.ensure_mode(0)  # nsn frames need Notifier mode
    # no reboot: state comes from the frames we send; reboot churn is the wedge
    # correlate (see device.py::_open_quiet doc).

    try:
        probe = device.render(timeout=6.0)
    except ExpectTimeout:
        pytest.skip("RENDER? not answering - needs the NIMBUS_TEST/notifierdbg build")
    active = probe.posture == POSTURE_ACTIVE

    scenarios = [
        ("two running", [nsn.running(170), nsn.running(85)], 60, 2, 0),
        ("add an approval", [nsn.running(170), nsn.running(85), nsn.awaiting_approval(32)], 90, 3, 1),
        ("clear all", [], 0, 0, 0),
    ]
    for name, segs, bright, want_jobs, want_attn in scenarios:
        nsn.send_frame(segs, bright)
        try:
            # BLE-tagged echo (net::ble::drain's debug print) - serial no longer
            # carries frames, so the untagged "NSN jobs=.." line is gone with it.
            # A longer timeout than the old serial version: BLE has real
            # scan/connect/write latency a synchronous serial write never had.
            m = device.expect_re(
                r"NSN\(ble\)\s+jobs=(?P<jobs>\d+)\s+attn=(?P<attn>\d+)\s+bright=(?P<b>\d+)", timeout=8.0
            )
        except ExpectTimeout:
            pytest.fail(
                f"[{name}] no 'NSN(ble) jobs=..' echo - frame dropped, BLE "
                "not connected, or not a notifierdbg/NIMBUS_TEST build"
            )
        assert int(m.group("jobs")) == want_jobs, f"[{name}] jobs={m.group('jobs')}, expected {want_jobs}"
        assert int(m.group("attn")) == want_attn, f"[{name}] attn={m.group('attn')}, expected {want_attn}"

        if active:
            time.sleep(0.3)
            r = device.render(timeout=6.0)
            if want_jobs == 0:
                assert r.ring in ("dark", "seg:0"), f"[{name}] ring={r.ring!r}, expected dark/seg:0"
            else:
                assert r.ring == f"seg:{want_jobs}", f"[{name}] ring={r.ring!r}, expected seg:{want_jobs}"
