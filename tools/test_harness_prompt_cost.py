#!/usr/bin/env python3
"""Unit tests (T1) for tools/harness_prompt_cost.py - the prompt cost instrument.

Run: python3 -m pytest tools/test_harness_prompt_cost.py
"""
import importlib.util
import os

_HERE = os.path.dirname(os.path.abspath(__file__))
_ROOT = os.path.dirname(_HERE)


def _load():
    spec = importlib.util.spec_from_file_location(
        "harness_prompt_cost", os.path.join(_HERE, "harness_prompt_cost.py")
    )
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


hpc = _load()


SYNTH = (
    "You are Nimbus, the always-on head orchestrator of a device. "
    "Fill the orch_turn fields:\n"
    "- reply: text now.\n"
    "- device: knobs.\n"
    "When [FRESH RESULTS] appears, synthesize.\n"
    "You are Nimbus, an always-on personal assistant, running in Orchestrator mode.\n"
    "\n[HOW YOU RUN]\n- turns.\n"
    "\n## DIRECTIVE\nServe.\n"
    "\n## CAPABILITIES\nHardware present: ring.\nTools you can call:\n- memory_search: search.\n"
    "\n## RUNNING SESSIONS (your sub-agents)\n- [job1] running\n"
    "\n## SCRATCHPAD (your own working notes)\nNow: qa\n"
    "\n## HOW YOUR MEMORY WORKS\n- recall.\n"
)


def test_sections_cover_whole_prompt_without_overlap():
    secs = hpc.split_sections(SYNTH)
    # concatenating the chunks reproduces the input exactly (no bytes lost/dup)
    assert "".join(chunk for _, chunk in secs) == SYNTH


def test_expected_section_labels_present_and_ordered():
    labels = [lbl for lbl, _ in hpc.split_sections(SYNTH)]
    assert labels[0] == "role_field_docs"
    assert "identity_how_you_run" in labels
    for lbl in ("directive", "capabilities", "running_sessions", "scratchpad", "memory_howto"):
        assert lbl in labels
    # order: role -> identity -> directive -> capabilities -> ... -> memory_howto
    assert labels.index("identity_how_you_run") < labels.index("directive")
    assert labels.index("directive") < labels.index("capabilities")
    assert labels.index("capabilities") < labels.index("memory_howto")


def test_field_docs_extracted():
    fd = hpc.field_docs_span(SYNTH)
    assert fd is not None
    assert "- reply:" in fd and "- device:" in fd
    # the field-docs block stops before the FRESH RESULTS sentence
    assert "synthesize" not in fd
    # and before the identity line
    assert "personal assistant" not in fd


def test_catalog_labeled_not_role():
    cat = "[PROVIDERS & CONNECTORS]\n- anthropic: github, gmail.\n"
    secs = hpc.split_sections(cat)
    assert secs == [("connector_catalog", cat)]


def test_measure_counts_are_positive():
    m = hpc.measure_file(os.path.join(_ROOT, "test/golden/orch_prompt_default.txt"))
    assert m["total_bytes"] > 10000
    assert m["field_docs"] is not None
    # field-docs is the dominant single contributor to this golden (>40%)
    assert m["field_docs"]["bytes"] > 0.4 * m["total_bytes"]


def test_tokenizer_available():
    # tokens should be a smaller, positive integer relative to bytes
    n = hpc.count_tokens("hello world, this is a token test")
    assert 0 < n < 20
