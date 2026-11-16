#!/usr/bin/env python3
"""Tests for check_budget_table.py: a table built from the protocol passes, every planted
violation fails it, and a missing input or column fails loudly instead of skipping.

The clean rows are the plan v3 figures (PROTOCOLS.md provider matrix): Mistral Large 4 at
100k tokens / 360,000 B, Claude Sonnet 5.5 at 100k / 250,000 B, a custom endpoint at the
85,000 B floor. The v5.1.1 rows are budgets_v5_1_1.json's shape (lane P0).
"""

from __future__ import annotations

import json
import os
import sys

import pytest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import check_budget_table as G  # noqa: E402

HEAD = (
    "| Provider | Model | Window | Target | Bytes | rNom | rSafe | Read window | Prefill | Decode | minFinal "
    "| System cap | Tool result | Recall | Replay | Summary | Message cap | Hist max | Hist soft | Rows |"
)
SEP = "|" + "---|" * 20


LEAD = ("provider", "model", "window", "target", "nbytes", "rnom", "rsafe", "read_s", "prefill", "decode", "min_final")


def row(spec: dict, over: dict) -> str:
    """One table row from a head's inputs, its derived columns from the protocol (over replaces cells)."""
    hist_max = spec["hist_max"]
    d = G.derived(spec["nbytes"], spec["target"], hist_max, G.wire_of(spec["provider"], spec["model"]))
    if hist_max <= 8192:  # the floor branch keeps v5.1.1's marks (RULINGS item 3)
        d.update(historySoftBytes=4096, historyRows=64)
    cells = {
        "System cap": d["systemPromptBytes"],
        "Tool result": d["toolResultBytes"],
        "Recall": d["recallBytes"],
        "Replay": d["replayBytes"],
        "Summary": d["chatSummaryBytes"],
        "Message cap": d["historyMessageCap"],
        "Hist max": hist_max,
        "Hist soft": d.get("historySoftBytes", 0),
        "Rows": d.get("historyRows", 64),
    }
    cells.update(over)
    vals = [spec[k] for k in LEAD]
    vals += [round(cells[k]) for k in ("System cap", "Tool result", "Recall", "Replay", "Summary", "Message cap")]
    vals += [round(cells[k]) for k in ("Hist max", "Hist soft", "Rows")]
    return "| " + " | ".join(str(v) for v in vals) + " |"


ROWS = {
    "large4": dict(provider="mistral", model="mistral-large-4-0", window=1048576, target=100000, nbytes=360000,
                   rnom=3.6, rsafe=2.5, read_s=60, prefill=9000, decode=50, min_final=1500, hist_max=206000),
    "sonnet": dict(provider="anthropic", model="claude-sonnet-5-5", window=1000000, target=100000, nbytes=250000,
                   rnom=2.5, rsafe=2.0, read_s=120, prefill=60000, decode=75, min_final=3000, hist_max=118000),
    "custom": dict(provider="custom", model="custom", window=100000, target=28333, nbytes=85000,
                   rnom=3.0, rsafe=2.5, read_s=120, prefill=1000, decode=50, min_final=3000, hist_max=8192),
}  # fmt: skip


def v511_model(provider, model, **over):
    m = {"provider": provider, "id": model, "systemPromptBytes": 32768, "chatSummaryBytes": 4096,
         "toolResultBytes": 8192, "recallBytes": 4096, "replayBytes": 16384, "historyMaxBytes": 8192,
         "historySoftBytes": 4096,
         "historyRows": 64, "historyMessageCap": 6144, "foldSliceBytes": 65536, "perTurnFloorBytes": 0}  # fmt: skip
    m.update(over)
    return m


def write(tmp_path, rows, v511=None):
    table = tmp_path / "request-budgets.md"
    table.write_text("# Request budgets\n\nGenerated.\n\n" + "\n".join([HEAD, SEP, *rows]) + "\n")
    models = v511 if v511 is not None else [v511_model(r["provider"], r["model"]) for r in ROWS.values()]
    fx = tmp_path / "budgets_v5_1_1.json"
    fx.write_text(json.dumps({"about": "test", "models": models}))
    return str(table), str(fx)


def clean_rows(**changes):
    out = []
    for name, r in ROWS.items():
        args = dict(r)
        over = {}
        for k, v in changes.get(name, {}).items():
            (args if k in args else over)[k] = v
        out.append(row(args, over))
    return out


def test_a_protocol_table_passes(tmp_path):
    t, v = write(tmp_path, clean_rows())
    assert G.problems(t, v) == []
    assert G.main(["--table", t, "--v511", v]) == 0


@pytest.mark.parametrize(
    "change,why",
    [
        ({"large4": {"target": 120000, "nbytes": 432000}}, "over the 100000 clamp"),
        ({"custom": {"nbytes": 120000}}, "not the 85000 B floor"),
        ({"sonnet": {"window": 128000, "nbytes": 250000}}, "over 0.75 x window"),
        ({"sonnet": {"nbytes": 60000, "target": 24000}}, "under the floor"),
        ({"large4": {"decode": 30}}, "worst round"),
        ({"large4": {"Recall": 20000}}, "recallBytes 20000, the protocol gives"),
        ({"sonnet": {"System cap": 30000}}, "systemPromptBytes 30000"),
        ({"large4": {"Summary": 4096}}, "chatSummaryBytes 4096"),
    ],
)
def test_a_planted_violation_fails(tmp_path, change, why):
    t, v = write(tmp_path, clean_rows(**change))
    errs = G.problems(t, v)
    assert any(why in e for e in errs), errs


def test_a_floor_head_keeps_v511_marks_and_a_lifted_one_does_not(tmp_path):
    t, v = write(tmp_path, clean_rows(custom={"Hist soft": 6554}))
    assert any("custom/custom: historySoftBytes 6554, the protocol gives 4096" in e for e in G.problems(t, v))
    t, v = write(tmp_path, clean_rows(large4={"Hist soft": 4096}))
    assert any("mistral-large-4-0: historySoftBytes 4096, the protocol gives 164800" in e for e in G.problems(t, v))


def test_the_replay_floor_is_the_wires_v511_cap(tmp_path):
    # Mistral, OpenAI and custom kept 24,576 B of tool rounds in v5.1.1; Anthropic 16,384.
    t, v = write(tmp_path, clean_rows(custom={"Replay": 16384}))
    assert any("custom/custom: replayBytes 16384, the protocol gives 24576" in e for e in G.problems(t, v))
    t, v = write(tmp_path, clean_rows())
    assert G.problems(t, v) == []
    assert (
        G.wire_of("cumulo", "anthropic/claude-sonnet-5-5") == "anthropic" and G.wire_of("cumulo", "gpt-5.6") == "openai"
    )


def test_a_floor_head_is_exempt_from_the_worst_round_only(tmp_path):
    # At the floor (v5.1.1's 85,000 B) a slow decode does not fail the gate: the caps fit
    # the round instead. The same decode above the floor still fails (large4 above).
    t, v = write(tmp_path, clean_rows(custom={"decode": 10}))
    assert G.problems(t, v) == []
    t, v = write(tmp_path, clean_rows(custom={"decode": 10, "nbytes": 120000}))
    assert any("custom/custom: worst round" in e for e in G.problems(t, v))


def test_a_field_under_v511_fails(tmp_path):
    models = [v511_model(r["provider"], r["model"]) for r in ROWS.values()]
    models[1]["toolResultBytes"] = 9000  # Sonnet's derived 8,192 would shrink it
    t, v = write(tmp_path, clean_rows(), models)
    assert any("toolResultBytes 8192 under its v5.1.1 value 9000" in e for e in G.problems(t, v))


def test_a_recorded_model_with_no_row_fails(tmp_path):
    models = [v511_model(r["provider"], r["model"]) for r in ROWS.values()] + [v511_model("zai", "glm-5.3")]
    t, v = write(tmp_path, clean_rows(), models)
    assert "zai/glm-5.3: in budgets_v5_1_1.json, no row in the table" in G.problems(t, v)


def test_missing_inputs_and_columns_fail_loudly(tmp_path):
    assert any("does not exist" in e for e in G.problems(str(tmp_path / "nope.md"), str(tmp_path / "nope.json")))
    t, v = write(tmp_path, clean_rows())
    with open(t) as f:
        text = f.read()
    with open(t, "w") as f:
        f.write(text.replace("| Decode |", "| Speed |"))
    assert any("needs bytes, target, prefill, decode" in e for e in G.problems(t, v))
    with open(t, "w") as f:
        f.write("# nothing\n")
    assert any("no table with a model column" in e for e in G.problems(t, v))


def test_numbers_read_with_their_units():
    assert G.number("360 KB") == 360000 and G.number("100k") == 100000
    assert G.number("1,048,576") == 1048576 and G.number("3.6") == 3.6 and G.number("-") is None


def test_the_repository_table_is_checked_once_it_exists():
    # The table is lane B's generated output and the fixture lane P0's; before both land the
    # gate reports the missing input (it never passes on an absent table).
    errs = G.problems(G.TABLE, G.V511)
    if os.path.exists(G.TABLE) and os.path.exists(G.V511):
        pytest.skip("both inputs exist: the gate runs in run_gate.py")
    assert errs and "does not exist" in errs[0]


if __name__ == "__main__":
    sys.exit(pytest.main([__file__, "-q"]))
