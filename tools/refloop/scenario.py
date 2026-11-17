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
      new_conversation: true a new conversation (earlier chat out of view; memory and files stay)
    say and burst take expect: a list of one-key checks (grade.CHECKS)
  final                      checks on the instance's state after the last turn
  judge_notes                the ground truth the blind judge is given
  models, reps               optional: run only on these models / this many repetitions

An unknown key or check anywhere stops the load, so a typo can never read as a pass.
"""

from __future__ import annotations

import re
from pathlib import Path

import yaml

import fixtures

TOP_KEYS = {"id", "title", "pain", "seed", "turns", "final", "judge_notes", "models", "reps"}
STEP_ACTIONS = ("say", "burst", "restart", "new_conversation")
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
    if actions[0] in ("restart", "new_conversation") and (step.get("expect") or step.get("photos")):
        raise ScenarioError(f"{where}: {actions[0]} takes no expect and no photos")
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
    return _expand(validate(yaml.safe_load(Path(path).read_text()), Path(path).name))


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
