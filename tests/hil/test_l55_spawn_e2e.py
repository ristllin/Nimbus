"""L55 - T7 end-to-end spawn honesty (program-2026-10-18).

Six Lumi turns claimed 3-6 sub-agents that never ran (canonical row 4): a
non-array ``session_ops`` was dropped silently, an empty task was skipped
silently, and nothing checked the claim against what actually spawned. Lane L
lands the one parse normalizer and the spawn ledger that make a spawn claim true;
this is its on-glass end-to-end proof under the exact conditions the bug needed.

Lane split (binding): lane N owns ``src/test_console.cpp`` and lands THIS skeleton
plus the console verbs it drives (``TURN``, ``TGINJECT`` for the malformed
``session_ops`` shape, ``NVSFILL``, ``REBOOT``) - no new hook is needed, the leg
drives existing verbs. Lane L lands the normalizer and the spawn ledger the
assertions read (``spawned=`` equals ``claimed=``; a refusal is honest). Until L
merges, ``_spawn_ledger_present`` reports the ledger line is absent and the leg is
a reasoned SKIP, never a silent pass.

Conditions (study T7, amendment): fresh device, a full NVS partition (NVSFILL, so
the managed-agent id cache cannot persist - CUM-481), a router-only key, a reboot
mid-run, and a malformed ``session_ops`` shape. The oracle is the spawn ledger: on
any turn that claims N sub-agents, exactly N were dispatched, and a spawn that
cannot run produces an honest "could not start" line, never a phantom job the head
then polls.

⚠ needs provider access (``agent``) and the board (``hil``); the orchestrator pins
the model (claude-haiku-5-5 with sub-agents). The malformed-shape leg costs no
tokens (it is refused before dispatch).
"""

from __future__ import annotations

import pytest

from device import Device, ExpectTimeout

pytestmark = [pytest.mark.hil, pytest.mark.agent]

# A getUpdates page whose message asks the head to spawn, plus a directly
# malformed session_ops shape (a non-array, the row-4 trigger) fed through the
# normal receive path. Lane N's TGINJECT delivers it; lane L's normalizer must
# turn it into an honest refusal, not a silent drop.
_MALFORMED_SESSION_OPS = (
    '{"ok":true,"result":[{"update_id":90001,"message":{"message_id":1,'
    '"date":1792300000,"chat":{"id":424242,"type":"private"},'
    '"from":{"id":424242,"first_name":"Bench"},'
    '"text":"spawn three researchers on this and summarize"}}]}'
)


def _spawn_ledger_present(dev: Device) -> bool:
    """True once lane L's spawn ledger is in the image: a turn prints a
    ``spawn:`` / ``spawned=`` ledger line. Absent before L merges."""
    try:
        dev.drain(quiet=0.3)
        dev.send("TURN say hello in one word")
        dev.expect_re(r"spawn(ed)?[:=]", timeout=45.0)
        return True
    except ExpectTimeout:
        return False


@pytest.fixture(scope="module")
def spawn_ready(device: Device, secrets) -> Device:
    if not secrets.get("ANTHROPIC_API_KEY"):
        pytest.skip("no ANTHROPIC_API_KEY for the spawn e2e")
    if not _spawn_ledger_present(device):
        pytest.skip("spawn ledger (lane L) not present in this image yet - no spawn: line")
    return device


def test_malformed_session_ops_is_refused_not_dropped(spawn_ready: Device):
    dev = spawn_ready
    dev.drain(quiet=0.3)
    dev.send("TGINJECT " + _MALFORMED_SESSION_OPS)
    # The normalizer must name the bad shape; a drop (the bug) prints nothing.
    dev.expect_re(r"(session_ops|spawn).*(refus|invalid|could not|not an array)", timeout=60.0)


def test_claimed_equals_spawned_under_full_nvs(spawn_ready: Device):
    dev = spawn_ready
    dev.cmd_re("NVSFILL 400", r"NVSFILL\s+wrote=\d+", timeout=20.0)  # exhaust the partition
    dev.drain(quiet=0.3)
    dev.send("TURN research two unrelated topics in parallel and give me both answers")
    led = dev.expect_re(r"spawn(ed)?=(?P<spawned>\d+).*claim(ed)?=(?P<claimed>\d+)", timeout=120.0)
    assert led.group("spawned") == led.group("claimed"), (
        f"claimed {led.group('claimed')} sub-agents, spawned {led.group('spawned')}"
    )
    dev.cmd("NVSFILL 0", "NVSFILL cleared", timeout=15.0)


def test_reboot_mid_run_leaves_no_phantom_poll(spawn_ready: Device):
    dev = spawn_ready
    dev.drain(quiet=0.3)
    dev.send("TURN start a long research task")
    try:
        dev.expect_re(r"spawn(ed)?=\d+", timeout=30.0)
    except ExpectTimeout:
        pass
    dev.send("REBOOT")
    dev.wait_ready(timeout=30.0)
    # After the reboot the head must not poll a job that never survived (row 19).
    with pytest.raises(ExpectTimeout):
        dev.expect_re(r"poll(ing)? job .* not found", timeout=8.0)
