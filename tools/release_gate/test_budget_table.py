#!/usr/bin/env python3
"""Tests for check_budget_table.py: a table built from the protocol passes, every planted
violation fails it, and a missing input, column or unknown table fails loudly instead of
skipping.

The clean rows are lane B's shipped figures (docs/reference/request-budgets.md): Mistral Large 4
at 100k tokens / 360,000 B, Claude Sonnet 5.5 at 100k / 250,000 B, a custom endpoint at the
85,000 B floor, under the document's own headings and header. The v5.1.1 rows are
budgets_v5_1_1.json's shape (lane P0). The last tests run the gate on the repository's
table and pin its header.
"""

from __future__ import annotations

import json
import os
import sys

import pytest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import check_budget_table as G  # noqa: E402

# The document's header (lane B, test_request_budget), column for column.
COLS = (
    "Provider", "Model", "Window", "Source", "Bound", "Target", "Bytes", "rNom", "rSafe", "Read window", "Prefill",
    "Decode", "minFinal", "System cap", "Tool result", "Recall", "Replay", "Summary", "Message cap", "Hist max",
    "Hist soft", "Hist keep", "Fold step", "Fold slice", "Rows", "Trigger", "Caps healthy", "Caps fragmented",
)  # fmt: skip
HEAD = "| " + " | ".join(COLS) + " |"
SEP = "|" + "---|" * len(COLS)
LEAD = {"Provider": "provider", "Model": "model", "Window": "window", "Bound": "bound", "Target": "target",
        "Bytes": "nbytes", "rNom": "rnom", "rSafe": "rsafe", "Read window": "read_s", "Prefill": "prefill",
        "Decode": "decode", "minFinal": "min_final", "Caps healthy": "caps_healthy",
        "Caps fragmented": "caps_fragmented"}  # fmt: skip


def row(spec: dict, over: dict) -> str:
    """One table row from a head's inputs, its derived columns from the protocol (over replaces cells)."""
    hist_max = spec["hist_max"]
    d = G.derived(spec["nbytes"], spec["target"], hist_max, G.wire_of(spec["provider"], spec["model"]))
    floor = spec["bound"] == "floor"
    if floor:  # the floor branch keeps v5.1.1's marks (RULINGS item 3)
        d.update(historySoftBytes=4096, historyRows=64)
    soft = round(d.get("historySoftBytes", 0))
    cells = {
        "Source": "listed",
        "System cap": round(d["systemPromptBytes"]),
        "Tool result": round(d["toolResultBytes"]),
        "Recall": round(d["recallBytes"]),
        "Replay": round(d["replayBytes"]),
        "Summary": d["chatSummaryBytes"],
        "Message cap": d["historyMessageCap"],
        "Hist max": hist_max,
        "Hist soft": soft,
        "Hist keep": hist_max // 2,
        "Fold step": 53792,
        "Fold slice": 65536,
        "Rows": round(d.get("historyRows", 64)),
        "Trigger": G.TRIGGER_V511 if floor else soft,
    }
    cells.update({c: spec[k] for c, k in LEAD.items()})
    cells.update(over)
    return "| " + " | ".join(str(cells[c]) for c in COLS) + " |"


ROWS = {
    "large4": dict(provider="mistral", model="mistral-large-4-0", window=1048576, bound="clamp", target=100000,
                   nbytes=360000, rnom=3.6, rsafe=2.5, read_s=60, prefill=9000, decode=50, min_final=1500,
                   hist_max=206080, caps_healthy="1584 / 2112", caps_fragmented="1500 / -"),
    "sonnet": dict(provider="anthropic", model="claude-sonnet-5-5", window=1000000, bound="clamp", target=100000,
                   nbytes=250000, rnom=2.5, rsafe=2.0, read_s=120, prefill=60000, decode=75, min_final=3000,
                   hist_max=118080, caps_healthy="6370 / 8494", caps_fragmented="5909 / 7879"),
    "custom": dict(provider="custom", model="custom", window=100000, bound="floor", target=28333, nbytes=85000,
                   rnom=3.0, rsafe=2.5, read_s=120, prefill=1000, decode=50, min_final=3000, hist_max=8192,
                   caps_healthy="none", caps_fragmented="none"),
}  # fmt: skip
ROUTED = dict(provider="cumulo", model="anthropic/claude-sonnet-5-5", window=1000000, bound="floor", target=34000,
              nbytes=85000, rnom=2.5, rsafe=2.0, read_s=90, prefill=60000, decode=75, min_final=3000,
              hist_max=8192, caps_healthy="4878 / 6504", caps_fragmented="4647 / 6196")  # fmt: skip


def v511_model(provider, model, **over):
    m = {"provider": provider, "id": model, "systemPromptBytes": 32768, "chatSummaryBytes": 4096,
         "toolResultBytes": 8192, "recallBytes": 4096, "replayBytes": 16384, "historyMaxBytes": 8192,
         "historySoftBytes": 4096,
         "historyRows": 64, "historyMessageCap": 6144, "foldSliceBytes": 65536, "perTurnFloorBytes": 0}  # fmt: skip
    m.update(over)
    return m


def write(tmp_path, rows, v511=None, more=""):
    table = tmp_path / "request-budgets.md"
    table.write_text("# Request budgets\n\n## Every recorded model\n\n" + "\n".join([HEAD, SEP, *rows]) + "\n" + more)
    models = v511 if v511 is not None else [v511_model(r["provider"], r["model"]) for r in ROWS.values()]
    fx = tmp_path / "budgets_v5_1_1.json"
    fx.write_text(json.dumps({"about": "test", "models": models}))
    return str(table), str(fx)


def lift_header(tmp_path, lift: str) -> str:
    h = tmp_path / "request_budget.h"
    h.write_text(f"inline constexpr bool kRoutedBudgetLift = {lift};\n")
    return str(h)


def one_row(spec: dict, **changes) -> str:
    args, over = dict(spec), {}
    for k, v in changes.items():
        (args if k in args else over)[k] = v
    return row(args, over)


def clean_rows(**changes):
    return [one_row(r, **changes.get(name, {})) for name, r in ROWS.items()]


def section(heading: str, rows: list) -> str:
    return f"\n## {heading}\n\n" + "\n".join([HEAD, SEP, *rows]) + "\n"


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
        ({"large4": {"Trigger": 20000}}, "fold trigger 20000 B under 24576 B"),
        ({"large4": {"Trigger": 49152}}, "fold trigger 49152 B, expected 164864 B (clamp bound)"),
        ({"custom": {"Trigger": 30000}}, "fold trigger 30000 B, expected 49152 B (floor bound)"),
        ({"sonnet": {"caps_fragmented": "2500 / 3400"}}, "fragmented cap 2500 under minFinal 3000 above the floor"),
        ({"sonnet": {"caps_healthy": "6370 / 6600"}}, "healthy retry 6600 adds under 400 tokens to 6370"),
        ({"large4": {"caps_fragmented": "1700 / -"}}, "fragmented cap 1700 over the healthy cap 1584"),
        (
            {"large4": {"caps_healthy": "2300 / 3000", "caps_fragmented": "1500 / -"}},
            "a healthy round at its 2300 cap takes",
        ),
    ],
)
def test_a_planted_violation_fails(tmp_path, change, why):
    t, v = write(tmp_path, clean_rows(**change))
    errs = G.problems(t, v)
    assert any(why in e for e in errs), errs


def test_a_floor_head_keeps_v511_marks_and_a_lifted_one_does_not(tmp_path):
    t, v = write(tmp_path, clean_rows(custom={"Hist soft": 6554}))
    assert any("custom/custom: historySoftBytes 6554, the protocol gives 4096" in e for e in G.problems(t, v))
    t, v = write(tmp_path, clean_rows(large4={"Hist soft": 4096, "Trigger": 4096}))
    assert any("mistral-large-4-0: historySoftBytes 4096, the protocol gives 164864" in e for e in G.problems(t, v))


def test_the_replay_floor_is_the_wires_v511_cap(tmp_path):
    # Mistral, OpenAI and custom kept 24,576 B of tool rounds in v5.1.1; Anthropic 16,384.
    t, v = write(tmp_path, clean_rows(custom={"Replay": 16384}))
    assert any("custom/custom: replayBytes 16384, the protocol gives 24576" in e for e in G.problems(t, v))
    t, v = write(tmp_path, clean_rows())
    assert G.problems(t, v) == []
    assert (
        G.wire_of("cumulo", "anthropic/claude-sonnet-5-5") == "anthropic" and G.wire_of("cumulo", "gpt-5.6") == "openai"
    )


def test_a_floor_head_is_exempt_from_the_worst_round_and_minfinal_only(tmp_path):
    # At the floor (v5.1.1's 85,000 B) a slow decode does not fail the gate, and its cap may sit
    # under minFinal: the caps fit the round instead. Above the floor both still fail.
    t, v = write(tmp_path, clean_rows(custom={"decode": 10, "caps_healthy": "600 / -", "caps_fragmented": "500 / -"}))
    assert G.problems(t, v) == []
    # ...but a floor head's cap still has to fit its round (900 tokens at 10 tok/s do not).
    t, v = write(tmp_path, clean_rows(custom={"decode": 10, "caps_healthy": "900 / -", "caps_fragmented": "800 / -"}))
    assert any("custom/custom: a healthy round at its 900 cap takes" in e for e in G.problems(t, v))
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
    for column, why in (("| Decode |", "needs bytes, target, prefill, decode"), ("| Trigger |", "needs trigger"),
                        ("| Caps healthy |", "needs caps healthy")):  # fmt: skip
        with open(t, "w") as f:
            f.write(text.replace(column, "| Other |"))
        assert any(why in e for e in G.problems(t, v)), column
    with open(t, "w") as f:
        f.write("# nothing\n")
    assert any("no table with a model column" in e for e in G.problems(t, v))


# ---- every table of the document, by its heading --------------------------------------------


def test_a_table_under_an_unknown_heading_fails(tmp_path):
    t, v = write(tmp_path, clean_rows(), more=section("Heads on the moon", clean_rows()))
    assert any("'Heads on the moon' is not one the gate knows" in e for e in G.problems(t, v))


def test_a_document_without_the_recorded_models_table_fails(tmp_path):
    t, v = write(tmp_path, clean_rows())
    with open(t) as f:
        text = f.read()
    with open(t, "w") as f:
        f.write(text.replace("## Every recorded model", "## Virtual Nimbus"))
    assert any("no 'Every recorded model' table" in e for e in G.problems(t, v))


def test_a_routed_head_stays_on_the_floor_while_the_lift_is_off(tmp_path):
    lifted = dict(ROUTED, bound="clamp", target=100000, nbytes=250000, hist_max=118080)
    for rows, lift, bad in (
        ([one_row(ROUTED)], "false", False),
        ([one_row(lifted)], "false", True),  # v5.1.2 ships with the lift off (RULINGS-v3)
        ([one_row(lifted)], "true", False),  # the later release that flips the constant
    ):
        models = [v511_model(r["provider"], r["model"]) for r in ROWS.values()]
        t, v = write(tmp_path, clean_rows(), models, more=section("Routed heads and the custom endpoint", rows))
        errs = G.problems(t, v, lift_header(tmp_path, lift))
        assert any("while kRoutedBudgetLift is false" in e for e in errs) == bad, errs
    errs = G.problems(t, v, str(tmp_path / "missing.h"))
    assert any("kRoutedBudgetLift not found" in e for e in errs)


def test_a_routed_head_is_held_to_its_upstreams_v511_row(tmp_path):
    t, v = write(tmp_path, clean_rows(),
                 more=section("Routed heads and the custom endpoint", [one_row(ROUTED, **{"Tool result": 4096})]))  # fmt: skip
    errs = G.problems(t, v, lift_header(tmp_path, "false"))
    assert any(
        "cumulo/anthropic/claude-sonnet-5-5: toolResultBytes 4096 under its v5.1.1 value 8192" in e for e in errs
    )


def test_a_server_round_counts_no_upload_and_no_pacing(tmp_path):
    # Mistral Medium on a Virtual Nimbus (lane B's row): 55,806 tokens of prefill at 2,500 tok/s
    # and a 3,000 token final at 85 tok/s take 59.1 s of 60 s. On a device the same body adds
    # 2.5 s of upload and 6.3 s of pacing: over the window.
    medium = dict(provider="mistral", model="mistral-medium-latest", window=262144, bound="latency", target=55806,
                  nbytes=173000, rnom=3.1, rsafe=2.5, read_s=60, prefill=2500, decode=85, min_final=3000,
                  hist_max=58528, caps_healthy="3000 / -", caps_fragmented="3000 / -")  # fmt: skip
    over = {"Summary": 4096, "Message cap": 6144}
    models = [v511_model(r["provider"], r["model"]) for r in ROWS.values()] + [v511_model("mistral", medium["model"])]
    t, v = write(tmp_path, clean_rows() + [one_row(medium, **over)], models)
    assert any("mistral-medium-latest: worst round" in e for e in G.problems(t, v))
    t, v = write(tmp_path, clean_rows(), more=section("Virtual Nimbus", [one_row(medium, **over)]))
    assert G.problems(t, v) == []


def test_the_preview_table_is_held_to_the_clamp_and_the_floor(tmp_path):
    head = "| Provider | Model | Window | Bound | Target | Bytes | Read window | Caps healthy | Caps fragmented |"
    body = "| cumulo | openai/gpt-5.6 | 272000 | clamp | {t} | {b} | 90 | none | none |"
    for target, nbytes, why in ((100000, 360000, None), (120000, 432000, "over the 100000 clamp"),
                                (20000, 72000, "under the 85000 B floor")):  # fmt: skip
        more = "\n## Routed heads once the router meters cached tokens\n\n" + "\n".join(
            [head, "|" + "---|" * 9, body.format(t=target, b=nbytes)]
        )
        t, v = write(tmp_path, clean_rows(), more=more + "\n")
        errs = G.problems(t, v)
        assert (errs == []) if why is None else any(why in e for e in errs), errs


def test_a_row_with_no_v511_record_uses_the_nearest_one(tmp_path):
    old = {("openai", "gpt-5.6-sol"): {"id": "sol"}, ("openai", "gpt-5.6-luna"): {"id": "luna"},
           ("anthropic", "claude-sonnet-5-5"): {"id": "sonnet"}}  # fmt: skip
    pick = lambda p, m: (G.baseline_of({"provider": p, "model": m}, old) or {}).get("id")  # noqa: E731
    assert pick("openai", "gpt-5.6-luna") == "luna"  # its own row
    assert pick("cumulo", "anthropic/claude-sonnet-5-5") == "sonnet"  # a routed head's upstream
    assert pick("custom", "gpt-5.6-luna") == "luna"  # the same id on another provider
    assert pick("cumulo", "gpt-5.6") == "luna" or pick("cumulo", "gpt-5.6") == "sol"  # an alias: its family
    assert pick("custom", "llama-3-70b") is None


# ---- a floor head's cap: the wire's fitted cap (lane CTXR2) ----------------------------------

# Codestral at the floor, as the table has it since CTXR2: the wire's fittedCap raises a cap the
# round cannot fit to the host's minimum final (3000), v5.1.1's first floor (kFirstFloor), and a
# round at that cap runs past the 60 s read window at 55 tok/s. v5.1.1 sent the same cap.
FLOOR_MISTRAL = dict(provider="mistral", model="codestral-2508", window=256000, bound="floor", target=27419,
                     nbytes=85000, rnom=3.1, rsafe=2.5, read_s=60, prefill=2500, decode=55, min_final=3000,
                     hist_max=8192, caps_healthy="3000 / -", caps_fragmented="3000 / -")  # fmt: skip


def floor_table(tmp_path, **changes):
    rows = clean_rows() + [one_row(FLOOR_MISTRAL, **changes)]
    v511 = [v511_model(r["provider"], r["model"]) for r in ROWS.values()]
    v511.append(v511_model("mistral", "codestral-2508"))
    return write(tmp_path, rows, v511=v511)


def test_a_floor_heads_cap_raised_to_minfinal_is_v511s_and_passes(tmp_path):
    t, v = floor_table(tmp_path)
    assert [e for e in G.problems(t, v) if "codestral" in e] == []


@pytest.mark.parametrize("cell", ["3500 / -", "4000 / 4400"])
def test_a_floor_cap_above_minfinal_still_has_to_fit_the_window(tmp_path, cell):
    # Only the raise to the minimum final is v5.1.1's: a cap above it at the floor is a choice
    # this release made, and its round must fit the read window like any other.
    t, v = floor_table(tmp_path, caps_healthy=cell)
    errs = [e for e in G.problems(t, v) if "codestral" in e]
    assert any("healthy round at its" in e for e in errs), errs


def test_a_head_above_the_floor_gets_no_minfinal_exemption(tmp_path):
    # The same slow rates on a head with room: a cap of minFinal whose round does not fit fails.
    t, v = write(tmp_path, clean_rows(large4=dict(decode=40, caps_healthy="3000 / -", caps_fragmented="3000 / -")))
    errs = G.problems(t, v)
    assert any("large-4-0" in e and "round at its 3000 cap" in e for e in errs), errs


# ---- with the tool loop off (lane CTXL) -------------------------------------------------------

LOOP_COLS = (
    "Provider", "Model", "Bytes", "Sized from", "System cap", "Recall", "Summary", "Message cap", "Hist max",
    "Hist soft", "Hist keep", "Fold slice", "Rows", "Trigger",
)  # fmt: skip
LOOP_OFF = "With the tool loop off"


def loop_row(provider="mistral", model="mistral-large-4-0", nbytes=360000, **over) -> str:
    """A loop-off row at v5.1.1's window (v511_model's figures), sized from the flat prompt."""
    cells = {"Provider": provider, "Model": model, "Bytes": nbytes, "Sized from": 55904, "System cap": 32768,
             "Recall": 4096, "Summary": 4096, "Message cap": 6144, "Hist max": 8192, "Hist soft": 4096,
             "Hist keep": 4096, "Fold slice": 65536, "Rows": 64, "Trigger": 49152}  # fmt: skip
    cells.update({k.replace("_", " ").capitalize() if k not in cells else k: v for k, v in over.items()})
    return "| " + " | ".join(str(cells[c]) for c in LOOP_COLS) + " |"


def loop_section(rows: list) -> str:
    return (
        f"\n## {LOOP_OFF}\n\n"
        + "\n".join(["| " + " | ".join(LOOP_COLS) + " |", "|" + "---|" * len(LOOP_COLS), *rows])
        + "\n"
    )


def flat_header(tmp_path, guard: int = 60000, minus: int = 4096) -> tuple:
    w = tmp_path / "wire_limits.h"
    w.write_text(f"inline constexpr size_t kJsonStrGuard = {guard};        // every model-bound string\n")
    h = tmp_path / "request_budget.h"
    h.write_text("inline constexpr bool kRoutedBudgetLift = false;\n"
                 f"inline constexpr uint32_t kFlatPromptBytes = (uint32_t)kJsonStrGuard - {minus};\n")  # fmt: skip
    return str(h), str(w)


def test_a_loop_off_table_at_v511s_window_passes(tmp_path):
    t, v = write(tmp_path, clean_rows(), more=loop_section([loop_row()]))
    h, w = flat_header(tmp_path)
    assert G.problems(t, v, h, w) == []


@pytest.mark.parametrize(
    "over, why",
    [
        ({"Sized from": 360000}, "sized from 360000"),
        ({"Hist max": 206080}, "historyMaxBytes 206080, v5.1.1"),
        ({"Hist soft": 3000}, "historySoftBytes 3000, v5.1.1"),
        ({"Rows": 1073}, "historyRows 1073, v5.1.1"),
        ({"Trigger": 164864}, "fold trigger 164864"),
        ({"Recall": 2048}, "recallBytes 2048 under its v5.1.1"),
        ({"Bytes": 250000}, "250000 B here"),
    ],
)  # fmt: skip
def test_a_planted_loop_off_violation_fails(tmp_path, over, why):
    t, v = write(
        tmp_path, clean_rows(), more=loop_section([loop_row(**{k.replace(" ", "_"): x for k, x in over.items()})])
    )
    h, w = flat_header(tmp_path)
    errs = G.problems(t, v, h, w)
    assert any(why in e for e in errs), errs


def test_a_loop_off_row_on_a_split_prompt_wire_fails(tmp_path):
    # Anthropic and OpenAI keep a split prompt with the loop off: their rows do not change, so a
    # row of theirs in this table is a table that sized the wrong turn.
    t, v = write(tmp_path, clean_rows(), more=loop_section([loop_row("anthropic", "claude-sonnet-5-5", 250000)]))
    h, w = flat_header(tmp_path)
    assert any("split prompt" in e for e in G.problems(t, v, h, w))


def test_the_flat_prompt_figure_is_read_from_the_code(tmp_path):
    h, w = flat_header(tmp_path, guard=64000, minus=4096)
    assert G.flat_prompt_bytes(h, w) == 59904
    t, v = write(tmp_path, clean_rows(), more=loop_section([loop_row()]))
    assert any("sized from 55904" in e for e in G.problems(t, v, h, w))
    (tmp_path / "wire_limits.h").write_text("// no guard here\n")
    assert any("kFlatPromptBytes" in e for e in G.problems(t, v, h, w))


def test_numbers_read_with_their_units():
    assert G.number("360 KB") == 360000 and G.number("100k") == 100000
    assert G.number("1,048,576") == 1048576 and G.number("3.6") == 3.6 and G.number("-") is None
    assert G.caps_of("6370 / 8494") == (6370, 8494) and G.caps_of("1600 / -") == (1600, None)
    assert G.caps_of("none") is None


# ---- the repository's own table ---------------------------------------------------------------


def test_the_repository_table_passes_and_its_header_is_the_one_bound_here():
    # docs/reference/request-budgets.md is generated by test_request_budget (lane B); this pins
    # the header the gate reads, so a renamed column fails here before it fails the release.
    assert G.problems(G.TABLE, G.V511) == []
    with open(G.TABLE, encoding="utf-8") as f:
        text = f.read()
    assert HEAD in text
    headings = [h for h, _ in G.parse_tables(text)]
    assert [G.norm(h) for h in headings] == list(G.TABLES)
    full = [rows[0] for h, rows in G.parse_tables(text) if not ({"preview", "loop_off"} & set(G.TABLES[G.norm(h)]))]
    assert all(set(r) == set(G.COLUMNS) - {"perTurnFloorBytes", "sized_from"} for r in full)
    assert "| " + " | ".join(LOOP_COLS) + " |" in text  # lane CTXL's loop-off header, column for column


def test_the_release_ships_with_the_routed_lift_off():
    # RULINGS-v3, release shape: v5.1.2 is not gated on the cloud; routed heads keep the floor.
    assert G.routed_lift(G.BUDGET_HEADER) is False


if __name__ == "__main__":
    sys.exit(pytest.main([__file__, "-q"]))
