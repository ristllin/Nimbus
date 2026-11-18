"""Host tests for the no-harness-lines gate (check_no_harness_lines.py), with its counter-tests.

Run: python3 -m pytest tools/release_gate

The last test reads the REAL tree's built images when they exist (the battery builds the device
envs before the citools leg); in CI's gate-logic job there are none and it skips with a reason.
"""

import os

import pytest

import check_no_harness_lines as gate

ALLOWED = (
    {"text": "(Note: the device kept ", "owner": "device", "why": "a device record"},
)


def _image(*texts: str) -> bytes:
    blob = b"\x00\x01header"
    for t in texts:
        blob += b"\x00" + t.encode("utf-8") + b"\x00\xff"
    return blob + b"tail"


def test_a_clean_image_with_its_allowed_lines_is_green():
    img = _image("Hello there.", "(Note: the device kept it.)")
    assert gate.problems({"cyd": img}, ALLOWED) == []


@pytest.mark.parametrize(
    "line",
    [
        "(Correction: that has not been saved.)",
        "(Note: this was not checked live; no lookup ran.)",
        "\n\n(Correction: the sub-agents did not start. Ask me again and I will retry.)",
    ],
)
def test_a_claim_line_in_the_image_is_red(line):
    errs = gate.problems({"cyd": _image(line, "(Note: the device kept it.)")}, ALLOWED)
    assert len(errs) == 1 and "reply-check line" in errs[0], errs


def test_every_image_is_read():
    clean = _image("(Note: the device kept it.)")
    dirty = _image("(Note: the device kept it.)", "(Correction: nothing was pinned this turn.)")
    errs = gate.problems({"esp32s3": clean, "test-cyd": dirty}, ALLOWED)
    assert len(errs) == 1 and errs[0].startswith("test-cyd:"), errs


def test_a_stale_allowed_entry_is_red():
    errs = gate.problems({"cyd": _image("Hello.")}, ALLOWED)
    assert len(errs) == 1 and "stale entry" in errs[0], errs


def test_no_image_is_red_never_a_silent_pass():
    errs = gate.problems({}, ALLOWED)
    assert errs and "no built image" in errs[0]


def test_an_entry_needs_an_owner_and_a_reason():
    bad = ({"text": "(Note: the device kept ", "owner": "", "why": ""},)
    errs = gate.problems({"cyd": _image("(Note: the device kept it.)")}, bad)
    assert any("owner and a reason" in e for e in errs), errs


def test_the_real_list_names_an_owner_and_a_reason_for_each_entry():
    for a in gate.ALLOWED:
        assert a["text"].startswith(gate.LEADS[0].decode()) or a["text"].startswith(gate.LEADS[1].decode())
        assert a["owner"] and a["why"]


def test_the_real_images_hold_no_reply_check_line():
    images = gate.default_images()
    if not images:
        pytest.skip("no built device image in this tree (CI gate-logic job): the battery's citools leg reads them")
    assert gate.problems(images) == []
