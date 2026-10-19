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

The ledger line (lane L, ``lib/harness/src/apply.cpp``) reaches serial as
``[agent] orchestrator: spawn ledger spawned=N claimed=M``, and only on a turn that
meant to start sub-agents. So the presence probe sends a turn that asks for one
sub-agent and reads until either that line or the turn's ``turn end:`` line: a
plain "say hello" turn never prints it and made the leg skip on an image that has
the ledger.

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

import re
import time
from typing import Optional, Tuple

import pytest

from device import Device, ExpectTimeout


def _hil_agent(fn):
    return pytest.mark.hil(pytest.mark.agent(fn))


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


# The ledger line, in lane L's shape (``spawn ledger spawned=N claimed=M``) and the
# short one the legs also accept (``spawn=N claim=M``).
LEDGER_RE = r"spawn(?:ed)?=(?P<spawned>\d+)\b.*?\bclaim(?:ed)?=(?P<claimed>\d+)"
TURN_END_RE = r"turn end: (?:ok|FAIL)"
PROBE_TURN = "Start one research sub-agent to answer in one sentence: why is the sky blue? Do not answer it yourself."
PROBE_TIMEOUT_S = 120.0
SETTLE_QUIET_S = 10.0  # no turn end for this long: the probe's sub-agent wave is done
SETTLE_CAP_S = 120.0


def parse_ledger(line: str) -> Optional[Tuple[int, int]]:
    """``(spawned, claimed)`` from a spawn ledger line, else None."""
    m = re.search(LEDGER_RE, line)
    return (int(m.group("spawned")), int(m.group("claimed"))) if m else None


def probe_verdict(line: str) -> Optional[bool]:
    """One serial line during the probe turn: True on a ledger line (the image has
    the ledger), False on the turn's end with no ledger line before it (it does
    not, or the head did not try to spawn), None for any other line."""
    if parse_ledger(line) is not None:
        return True
    if re.search(TURN_END_RE, line):
        return False
    return None


def _settle(dev: Device) -> None:
    """Let the probe's sub-agent and its synthesis turn finish, so their lines never
    answer a later leg: read until no ``turn end:`` for SETTLE_QUIET_S, at most
    SETTLE_CAP_S."""
    end_by = time.time() + SETTLE_CAP_S
    while time.time() < end_by:
        try:
            dev.expect_re(TURN_END_RE, timeout=min(SETTLE_QUIET_S, max(1.0, end_by - time.time())))
        except ExpectTimeout:
            return


def _spawn_ledger_present(dev: Device) -> bool:
    """True once lane L's spawn ledger is in the image: a turn that asks for a
    sub-agent prints a ledger line before its ``turn end:``. Absent before L
    merges (the turn ends with no ledger line, or nothing arrives in time)."""
    dev.drain(quiet=0.3)
    dev.send("TURN " + PROBE_TURN)
    deadline = time.time() + PROBE_TIMEOUT_S
    while True:
        try:
            m = dev.expect_re(rf"{LEDGER_RE}|{TURN_END_RE}", timeout=max(0.0, deadline - time.time()))
        except ExpectTimeout:
            return False
        verdict = probe_verdict(m.string)
        if verdict is not None:
            if verdict:
                _settle(dev)
            return verdict


@pytest.fixture(scope="module")
def spawn_ready(device: Device, secrets) -> Device:
    if not secrets.get("ANTHROPIC_API_KEY"):
        pytest.skip("no ANTHROPIC_API_KEY for the spawn e2e")
    if not _spawn_ledger_present(device):
        pytest.skip(
            "spawn ledger (lane L) not present: a turn that asked for a sub-agent ended "
            "with no 'spawn ledger spawned=N claimed=M' line (an image without lane L, "
            "or the head did not try to spawn)"
        )
    return device


@_hil_agent
def test_malformed_session_ops_is_refused_not_dropped(spawn_ready: Device):
    dev = spawn_ready
    dev.drain(quiet=0.3)
    dev.send("TGINJECT " + _MALFORMED_SESSION_OPS)
    # The normalizer must name the bad shape; a drop (the bug) prints nothing.
    dev.expect_re(r"(session_ops|spawn).*(refus|invalid|could not|not an array)", timeout=60.0)


@_hil_agent
def test_claimed_equals_spawned_under_full_nvs(spawn_ready: Device):
    dev = spawn_ready
    dev.cmd_re("NVSFILL 400", r"NVSFILL\s+wrote=\d+", timeout=20.0)  # exhaust the partition
    dev.drain(quiet=0.3)
    dev.send("TURN research two unrelated topics in parallel and give me both answers")
    spawned, claimed = parse_ledger(dev.expect_re(LEDGER_RE, timeout=120.0).string)
    assert spawned == claimed, f"claimed {claimed} sub-agents, spawned {spawned}"
    dev.cmd("NVSFILL 0", "NVSFILL cleared", timeout=15.0)


@_hil_agent
def test_reboot_mid_run_leaves_no_phantom_poll(spawn_ready: Device):
    dev = spawn_ready
    dev.drain(quiet=0.3)
    dev.send("TURN start a long research task")
    try:
        dev.expect_re(LEDGER_RE, timeout=30.0)
    except ExpectTimeout:
        pass
    dev.send("REBOOT")
    dev.wait_ready(timeout=30.0)
    # After the reboot the head must not poll a job that never survived (row 19).
    with pytest.raises(ExpectTimeout):
        dev.expect_re(r"poll(ing)? job .* not found", timeout=8.0)


# ---- host tests: the probe's parser (no board) --------------------------------------


@pytest.mark.host
@pytest.mark.parametrize(
    "line, want",
    [
        ("[agent] orchestrator: spawn ledger spawned=3 claimed=3", (3, 3)),  # lane L, on serial
        ("orchestrator: spawn ledger spawned=0 claimed=1", (0, 1)),  # a refusal is a ledger line too
        ("spawn=2 claim=5", (2, 5)),  # the short shape
        ("[agent] spawn ledger spawned=12 claimed=6", (12, 6)),
    ],
)
def test_parse_ledger_reads_both_line_shapes(line, want):
    assert parse_ledger(line) == want
    assert probe_verdict(line) is True


@pytest.mark.host
@pytest.mark.parametrize(
    "line",
    [
        "[agent] turn end: ok host=anthropic tokens in=812 out=40 tools=0 reply=12B",
        "[agent] turn end: FAIL host=anthropic tokens in=0 out=0 tools=0 reply=0B",
    ],
)
def test_a_turn_end_before_any_ledger_line_means_absent(line):
    assert parse_ledger(line) is None
    assert probe_verdict(line) is False


@pytest.mark.host
@pytest.mark.parametrize(
    "line",
    [
        "[agent] usage: spawn:anthropic in=120 out=30",  # the spend tag, not the ledger
        "[agent] orchestrator: spawn not started ('scout') - no task",
        "ORCH REPLY [serial]: hello",
        "spawned=3",  # half a ledger line
        "RENDER screen=0 posture=1 seg=0 single=0 dark=1 bright=60",
    ],
)
def test_other_lines_do_not_decide_the_probe(line):
    assert parse_ledger(line) is None
    assert probe_verdict(line) is None


class _ProbeDevice(Device):
    """The real Device read path over a scripted console: lines queued in order,
    reads never wait, and every sent line is recorded."""

    def __init__(self, lines):
        super().__init__(port="/dev/cu.fake")
        self._ser = object()
        self.lines = list(lines)
        self.sent = []

    def send(self, line):
        self.sent.append(line)

    def drain(self, quiet=0.3):
        pass

    def _readline(self, deadline):
        return self.lines.pop(0) if self.lines else None


@pytest.mark.host
def test_probe_asks_for_a_sub_agent_and_sees_the_ledger():
    dev = _ProbeDevice(
        [
            "[agent] orchestrator: spawn ledger spawned=1 claimed=1",
            "[agent] turn end: ok host=anthropic tokens in=900 out=60 tools=0 reply=40B",
        ]
    )
    assert _spawn_ledger_present(dev) is True
    assert dev.sent == ["TURN " + PROBE_TURN]
    assert "sub-agent" in PROBE_TURN  # a plain hello never prints the ledger


@pytest.mark.host
def test_probe_reports_absent_when_the_turn_ends_without_a_ledger():
    dev = _ProbeDevice(
        [
            "[agent] usage: spawn:anthropic in=120 out=30",
            "[agent] turn end: ok host=anthropic tokens in=900 out=60 tools=0 reply=40B",
            "[agent] orchestrator: spawn ledger spawned=1 claimed=1",  # a later turn's line
        ]
    )
    assert _spawn_ledger_present(dev) is False


@pytest.mark.host
def test_probe_reports_absent_when_nothing_arrives():
    assert _spawn_ledger_present(_ProbeDevice([])) is False
