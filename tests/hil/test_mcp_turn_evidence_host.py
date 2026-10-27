"""Host tier: an MCP leg proves the turn CALLED the tool, from the device's own
evidence (no board).

L15b passed with no MCP call at all: its echo marker rides the prompt, so a model
that only repeated it satisfied "marker in the reply". connectors.mcp_result_line
reads the tool-loop transcript /api/lastturn carries (section 4: "[result] <tool
name> <output>") for the call's result holding the marker.
"""

from __future__ import annotations

import pytest

from connectors import mcp_result_line

pytestmark = pytest.mark.host

MARKER = "NBX-l15b-echo-1a2b3c"
SECTION_4 = (
    "===== 4. TOOL-LOOP TRANSCRIPT - the mid-turn rounds =====\n"
    f"[user] Use the echo tool from the hilrobust_b MCP server to echo exactly this text: {MARKER}\n"
    f'[tool] mcp_hilrobust_b_echo {{"message":"{MARKER}"}}\n'
    f"[result] mcp_hilrobust_b_echo Echo: {MARKER}\n"
)


def test_the_call_result_with_the_marker_is_the_evidence():
    assert mcp_result_line({"_raw": SECTION_4}, "hilrobust_b", "echo", MARKER).startswith(
        "[result] mcp_hilrobust_b_echo"
    )
    dotted = SECTION_4.replace("[result] mcp_hilrobust_b_echo", "[result] mcp.hilrobust_b.echo")
    assert mcp_result_line({"_raw": dotted}, "hilrobust_b", "echo", MARKER)


@pytest.mark.parametrize(
    "raw",
    [
        # the model only repeated the prompt's marker: a single-shot turn, no tool line
        f"[USER]\n... echo exactly this text: {MARKER}\n===== 3. MODEL RAW OUTPUT =====\n"
        f'{{"reply":"{MARKER}"}}\n(single-shot turn - the model answered without calling a tool)\n',
        # the call was made but only its arguments carry the marker
        SECTION_4.replace(f"Echo: {MARKER}", "Echo: something else"),
        # another server's echo
        SECTION_4.replace("[result] mcp_hilrobust_b_echo", "[result] mcp_hilrobust_a_echo"),
        # a failed call
        SECTION_4.replace("[result] mcp_hilrobust_b_echo Echo:", "[result] mcp_hilrobust_b_echo (error) Echo:"),
    ],
)
def test_a_repeated_marker_or_a_wrong_or_failed_call_is_no_evidence(raw):
    assert mcp_result_line({"_raw": raw}, "hilrobust_b", "echo", MARKER) == ""
