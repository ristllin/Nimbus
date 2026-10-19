"""L58 - quiet, honest nightly dream (program-2026-10-18, lane M).

The nightly dream used to report "ok pruned=0 deduped=0" whatever happened, and a
failed dream sent "That didn't finish ... ask again to retry" to the owner's
Telegram at about 04:30 (canonical rows 27 and 28). This leg drives the console
``DREAM`` drill on a bench unit and reads the result where the owner would: the
``ev:dream`` timeline row (``EPIQ Nightly dream``) and the absence of any Telegram
send while the quiet turn failed.

Legs:
  1. Every provider unreachable (``FAULT provider on``): the row says FAILED with
     its cause and the write counts, a retry is scheduled for the same night, and
     the device sends NOTHING to Telegram (the failure notice is held).
  2. Memory refused (``FAULT memory on``) after an owner turn that states durable
     facts: the reflection's writes fail, and the row says degraded with the
     failed count - never ok.
  3. Storage: the dream path writes no NVS (STATUS: used bytes and failed NVS
     writes are the same before and after the drill; run it on the NVSFILL'd unit).

Conditions: unit L (the fill unit), Orchestrator mode, a cheap head
(claude-haiku-5-5), the owner's Telegram chat allowlisted. Leg 1 costs nothing
(the requests never leave the device); leg 2 costs one owner turn and one
reflection turn on the cheap head (well under $0.01).
"""

from __future__ import annotations

import re
import time

import pytest

from device import Device, ExpectTimeout

pytestmark = [pytest.mark.hil, pytest.mark.agent]

_DREAM_ROW = re.compile(r"Nightly dream [^\"]*")


def _dream_rows(dev: Device) -> list[str]:
    """Every ev:dream row text the episodic store returns for the query."""
    dev.drain(quiet=0.3)
    dev.send("EPIQ Nightly dream")
    rows: list[str] = []
    deadline = time.time() + 8.0
    while time.time() < deadline:
        try:
            line = dev.expect_re(r"Nightly dream|EPIQ|ERR", timeout=max(0.5, deadline - time.time()))
        except ExpectTimeout:
            break
        for m in _DREAM_ROW.finditer(line.string):
            rows.append(m.group(0))
        if "EPIQ" in line.string and "Nightly dream" not in line.string:
            break
    return rows


def _drill(dev: Device, timeout: float = 240.0) -> str:
    """Run the DREAM drill; return the transcript lines it produced."""
    dev.drain(quiet=0.5)
    start = len(dev._transcript)
    dev.send("DREAM")
    dev.expect("DREAM fired", timeout=timeout)
    # Telegram sends are queued; give a held-or-sent notice time to surface.
    try:
        dev.expect("telegram: send", timeout=6.0)
    except ExpectTimeout:
        pass
    return "\n".join(dev._transcript[start:])


def _nvs(dev: Device) -> tuple[int, int]:
    """(used bytes, failed writes) from STATUS - read only."""
    m = dev.cmd_re("STATUS", r"nvs=(\d+)/\d+ nvsfree=\d+ nvsfail=(\d+)", timeout=8.0)
    return int(m.group(1)), int(m.group(2))


@pytest.fixture(scope="module")
def orch(device: Device, secrets) -> Device:
    if not secrets.get("ANTHROPIC_API_KEY"):
        pytest.skip("no ANTHROPIC_API_KEY for the dream drill")
    if not device.ping():
        pytest.skip("console not answering")
    return device


def test_failed_quiet_dream_sends_nothing_and_says_failed(orch: Device):
    orch.cmd("FAULT provider on", "FAULT provider=1", timeout=5.0)
    try:
        window = _drill(orch)
    finally:
        orch.cmd("FAULT clear", "FAULT cleared", timeout=5.0)
    assert "quiet turn: notice held" in window, "the failure notice was not held"
    assert "telegram: send" not in window, "a failed quiet dream sent a Telegram message"
    rows = _dream_rows(orch)
    assert rows, "no ev:dream row was written"
    newest = rows[-1]
    assert newest.startswith("Nightly dream FAILED ("), newest
    assert "memories stored" in newest and "failed)" in newest, newest  # the counts
    assert "retry 2 of 3 at" in newest or "no retry tonight" in newest, newest


def test_failed_writes_make_the_night_degraded_with_counts(orch: Device):
    orch.turn(
        "Please remember for later: my sister Anna's birthday is on 3 March, and I work from home on Fridays now."
    )
    orch.expect("orchestrator: turn", timeout=120.0)
    orch.drain(quiet=2.0)
    orch.cmd("FAULT memory on", "FAULT memory=1", timeout=5.0)
    try:
        window = _drill(orch)
    finally:
        orch.cmd("FAULT clear", "FAULT cleared", timeout=5.0)
    assert "telegram: send" not in window, "the dream messaged the owner"
    newest = _dream_rows(orch)[-1]
    assert not newest.startswith("Nightly dream ok"), newest
    if newest.startswith("Nightly dream degraded"):
        m = re.search(r"(\d+) failed\)", newest)
        assert m and int(m.group(1)) >= 1, newest
    else:
        # The model chose not to write (allowed on a short day): the row still
        # reports the counts and is not "ok" with writes that failed.
        assert "memories stored" in newest, newest


def test_dream_writes_no_nvs(orch: Device):
    before = _nvs(orch)
    orch.cmd("FAULT provider on", "FAULT provider=1", timeout=5.0)
    try:
        _drill(orch)
    finally:
        orch.cmd("FAULT clear", "FAULT cleared", timeout=5.0)
    assert _nvs(orch) == before, "the dream path wrote NVS (used bytes or failed writes moved)"
