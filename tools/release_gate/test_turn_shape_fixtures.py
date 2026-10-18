"""Privacy tests for tools/gen_turn_shape_fixtures.py and its committed fixtures.

The turn-shape fixtures come from the owner's real conversation exports. Only
the shape may ship: synthetic text, allowlisted tag values, synthetic ids. These
tests plant personal bytes in every free field and require the verifier to
refuse them, then require the committed fixtures to pass.

Run: python3 -m pytest tools/release_gate
"""

import json
import sys
from pathlib import Path

TOOLS = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(TOOLS))

import gen_turn_shape_fixtures as gen  # noqa: E402

FIXTURES = TOOLS.parent / "test/support/fixtures/turn_shapes_2026-10"


def test_sender_name_never_survives_a_tag():
    assert gen.scrub_tag("from:R") == "from:owner"
    assert gen.scrub_tag("from:Some Person") == "from:owner"


def test_known_shapes_keep_their_value_and_unknown_values_do_not():
    assert gen.scrub_tag("tool:memory_write") == "tool:memory_write"
    assert gen.scrub_tag("ev:turnend") == "ev:turnend"
    assert gen.scrub_tag("round:3") == "round:3"
    assert gen.scrub_tag("err") == "err"
    assert gen.scrub_tag("via:Telegram Desktop") == "via:x"  # not a known shape
    assert gen.scrub_tag("note:call mum") == "note:x"  # unknown key
    assert gen.scrub_tag("Weird Key:1") == "x"
    assert gen.scrub_tag("turn:m00000623").startswith("turn:m")
    assert "00000623" not in gen.scrub_tag("turn:m00000623")  # ids are re-derived


def _clean_row():
    return {
        "id": "m1234abcd",
        "chat": "chat1",
        "kind": "message",
        "text": "[synthetic owner message number 4] lorem ipsum",
        "tags": "turn:m1234abcd,round:0",
    }


def test_clean_row_passes():
    assert gen.row_problems(_clean_row()) == []


def test_planted_personal_bytes_are_refused_in_every_field():
    for field, value in (
        ("text", "Meet Dana at 6pm, 07700 900123"),
        ("tags", "turn:m1234abcd,from:Dana"),
        ("chat", "6098532481"),
        ("id", "m00000623x"),
    ):
        row = _clean_row()
        row[field] = value
        assert gen.row_problems(row), f"a personal {field} passed the verifier"


def test_tool_rows_keep_only_the_tool_name():
    shaped = gen.tool_shape('memory_write({"content":"Dana likes tea"})\n-> stored memory m1e227b31', 7)
    assert "Dana" not in shaped and shaped.startswith("memory_write(")
    row = dict(_clean_row(), kind="tool_output", text=shaped)
    assert gen.row_problems(row) == []


def test_committed_fixtures_are_clean():
    files = sorted(FIXTURES.glob("*.jsonl"))
    assert files, "no committed turn-shape fixtures found"
    for f in files:
        for n, line in enumerate(f.read_text().splitlines(), 1):
            if line.strip():
                assert gen.row_problems(json.loads(line)) == [], f"{f.name}:{n}"
