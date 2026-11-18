#!/usr/bin/env python3
"""scenario - load and check the scripted scenarios (YAML).

A scenario is a short script both arms run turn by turn. Format (README.md has the
full table):

  id, title, pain            what it is and which owner pain it stands for
  seed.files                 files both instances hold before turn 1: {project, name, text | fixture}
  seed.memories              facts stored with memory.write before turn 1
  turns                      a list of steps, each exactly one of:
      say: text              one owner message   (+ photos: [fixture, ...]); "{fixture:name}" in the
                             text is replaced by that text fixture
      burst: [text, ...]     messages sent gap_s apart, before any reply
      restart: true          restart nimbusd; the reference loop reloads its transcript
      new_conversation: true a new conversation (earlier chat out of view; memory and files stay),
                             on a new chat key
      advance: N | "90m" | "04:00"
                             move both arms' clocks: N whole seconds, a duration (s, m, h, d), or
                             the next time that local wall time comes round in the rig TZ; the
                             daemons then run idle work (routines, compaction) before the next turn
    say and burst take expect: a list of one-key checks (grade.CHECKS)
  final                      checks on the instance's state after the last turn
  judge_notes                the ground truth the blind judge is given
  models, reps               optional: run only on these models / this many repetitions

An unknown key or check anywhere stops the load, so a typo can never read as a pass.
"""

from __future__ import annotations

import re
from datetime import datetime, timedelta
from pathlib import Path
from zoneinfo import ZoneInfo

import yaml

import fixtures

TOP_KEYS = {"id", "title", "pain", "seed", "turns", "final", "judge_notes", "models", "reps"}
STEP_ACTIONS = ("say", "burst", "restart", "new_conversation", "advance")
# The time zone both arms run in (nimbusd's TZ, the reference loop's stamps, a wall-time advance).
RIG_TZ = "Europe/London"
# The test clock's own limit on one advance (nimbusd vclock::kMaxAdvanceSec: 366 days).
MAX_ADVANCE_S = 366 * 86400
DURATION_RE = re.compile(r"^(\d+)([smhd])$")
WALL_RE = re.compile(r"^([01]\d|2[0-3]):([0-5]\d)$")
UNIT_S = {"s": 1, "m": 60, "h": 3600, "d": 86400}
STEP_KEYS = {*STEP_ACTIONS, "photos", "gap_s", "expect", "timeout_s"}
# Checks that read the instance's state right after the turn (the runner snapshots it then).
STATE_CHECKS = {
    "file_matching",
    "file_contains",
    "file_absent",
    "files_at_most",
    "loops_count",
    "loops_regex",
    "loops_not_regex",
    "memory_regex",
    "memory_not_regex",
}
REPLY_CHECKS = {
    "reply_regex",
    "reply_not_regex",
    "last_reply_regex",
    "max_replies",
    "difference_from_turn",
    "tool_called",
    "tool_not_called",
    "min_tool_calls",
}
CHECK_NAMES = STATE_CHECKS | REPLY_CHECKS


class ScenarioError(ValueError):
    pass


class _Loader(yaml.SafeLoader):
    """YAML 1.1 reads an unquoted 12:00 as the base-60 integer 720, so `advance: 12:00` would move
    12 minutes. This loader keeps every implicit type except base-60 numbers: 12:00 stays text."""


_Loader.yaml_implicit_resolvers = {
    first: [(tag, rx) for tag, rx in resolvers if tag not in ("tag:yaml.org,2002:int", "tag:yaml.org,2002:float")]
    for first, resolvers in yaml.SafeLoader.yaml_implicit_resolvers.items()
}
_Loader.add_implicit_resolver(
    "tag:yaml.org,2002:int",
    re.compile(r"^(?:[-+]?0b[0-1_]+|[-+]?0[0-7_]+|[-+]?(?:0|[1-9][0-9_]*)|[-+]?0x[0-9a-fA-F_]+)$"),
    list("-+0123456789"),
)
_Loader.add_implicit_resolver(
    "tag:yaml.org,2002:float",
    re.compile(
        r"^(?:[-+]?(?:[0-9][0-9_]*)\.[0-9_]*(?:[eE][-+][0-9]+)?|\.[0-9_]+(?:[eE][-+][0-9]+)?|[-+]?\.(?:inf|Inf|INF)|\.(?:nan|NaN|NAN))$"
    ),
    list("-+0123456789."),
)


def parse_yaml(text: str):
    return yaml.load(text, Loader=_Loader)  # noqa: S506 - a SafeLoader subclass


def parse_advance(value) -> tuple:
    """("seconds", n) or ("wall", hour, minute) for an advance step's value. Anything else is refused."""
    if isinstance(value, int) and not isinstance(value, bool):
        seconds = value
    elif isinstance(value, str) and WALL_RE.match(value):
        hour, minute = WALL_RE.match(value).groups()
        return ("wall", int(hour), int(minute))
    elif isinstance(value, str) and DURATION_RE.match(value):
        n, unit = DURATION_RE.match(value).groups()
        seconds = int(n) * UNIT_S[unit]
    else:
        raise ScenarioError(f"advance takes whole seconds, a duration like 90m, or a wall time HH:MM, got {value!r}")
    if not 0 < seconds <= MAX_ADVANCE_S:
        raise ScenarioError(f"advance of {seconds} s is outside 1..{MAX_ADVANCE_S}")
    return ("seconds", seconds)


def advance_target(value, epoch: int, tz: str = RIG_TZ) -> int:
    """The unix time an advance from `epoch` lands on. A wall time is the next one strictly after epoch."""
    kind, *rest = parse_advance(value)
    if kind == "seconds":
        return int(epoch) + rest[0]
    now = datetime.fromtimestamp(int(epoch), ZoneInfo(tz))
    day = now.date()
    while True:  # today's, else tomorrow's; the calendar date steps, so a DST change keeps the wall time
        at = datetime(day.year, day.month, day.day, rest[0], rest[1], tzinfo=ZoneInfo(tz))
        if at.timestamp() > epoch:
            return int(at.timestamp())
        day += timedelta(days=1)


def _check_list(checks, where: str, allowed: set):
    for c in checks or []:
        if not isinstance(c, dict) or len(c) != 1:
            raise ScenarioError(f"{where}: each check is one name and one value, got {c!r}")
        name = next(iter(c))
        if name not in allowed:
            raise ScenarioError(f"{where}: unknown check {name!r}")


def _check_step(step, where: str):
    if not isinstance(step, dict) or set(step) - STEP_KEYS:
        raise ScenarioError(f"{where}: unknown key(s) {sorted(set(step) - STEP_KEYS)}")
    actions = [a for a in STEP_ACTIONS if a in step]
    if len(actions) != 1:
        raise ScenarioError(f"{where}: a step has exactly one of {STEP_ACTIONS}")
    if actions[0] in ("restart", "new_conversation", "advance") and (step.get("expect") or step.get("photos")):
        raise ScenarioError(f"{where}: {actions[0]} takes no expect and no photos")
    if actions[0] == "advance":
        try:
            parse_advance(step["advance"])
        except ScenarioError as e:
            raise ScenarioError(f"{where}: {e}") from None
    if actions[0] == "burst" and (not isinstance(step["burst"], list) or len(step["burst"]) < 2):
        raise ScenarioError(f"{where}: burst is a list of at least two messages")
    _check_list(step.get("expect"), where, CHECK_NAMES)


def validate(sc: dict, source: str = "") -> dict:
    if not isinstance(sc, dict) or set(sc) - TOP_KEYS:
        raise ScenarioError(f"{source}: unknown top-level key(s) {sorted(set(sc) - TOP_KEYS)}")
    for key in ("id", "title", "turns", "judge_notes"):
        if not sc.get(key):
            raise ScenarioError(f"{source}: missing {key}")
    for n, step in enumerate(sc["turns"]):
        _check_step(step, f"{source} step {n + 1}")
    _check_list(sc.get("final"), f"{source} final", STATE_CHECKS)
    seed = sc.get("seed") or {}
    if set(seed) - {"files", "memories"}:
        raise ScenarioError(f"{source}: unknown seed key(s)")
    for f in seed.get("files", []):
        if set(f) - {"project", "name", "text", "fixture"} or ("text" in f) == ("fixture" in f):
            raise ScenarioError(f"{source}: a seed file is project, name and one of text or fixture")
    return sc


def user_turns(sc: dict) -> list:
    """The steps the owner speaks in (say or burst), in order: turn N is user_turns(sc)[N - 1]."""
    return [s for s in sc["turns"] if "say" in s or "burst" in s]


def needs_state(step: dict) -> bool:
    return any(next(iter(c)) in STATE_CHECKS for c in step.get("expect") or [])


FIXTURE_RE = re.compile(r"\{fixture:(\w+)\}")


def _expand(sc: dict) -> dict:
    """A say of "{fixture:name}" is that text fixture's content: long made-up text stays out of the YAML."""
    for step in sc["turns"]:
        if isinstance(step.get("say"), str) and "{fixture:" in step["say"]:
            step["say"] = FIXTURE_RE.sub(lambda m: fixtures.text(m.group(1)), step["say"])
    return sc


def load(path: Path) -> dict:
    return _expand(validate(parse_yaml(Path(path).read_text()), Path(path).name))


def load_all(paths: list) -> list:
    """Scenario files, or directories of them, in name order. Duplicate ids are refused."""
    files = []
    for p in map(Path, paths):
        files += sorted(p.glob("*.yaml")) if p.is_dir() else [p]
    scenarios = [load(f) for f in files]
    ids = [s["id"] for s in scenarios]
    if len(set(ids)) != len(ids):
        raise ScenarioError(f"duplicate scenario ids: {sorted(i for i in ids if ids.count(i) > 1)}")
    return scenarios
