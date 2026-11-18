"""Host tests for the no-prompt-patch gate (check_no_prompt_patch.py, PRD section 11).

Run: python3 -m pytest tools/release_gate

Each counter-test copies the catalog's sources, goldens, ledger, register and scenarios
into a temp git repo, plants a change and asserts the gate turns red on it: a 300 B
growth in every class of both engines, a byte-neutral swap without its red scenario and
rates, a remove that hides added text, an edited merged entry, a tool file outside the
catalog, a register row without a red scenario. The first test runs the gate on the real tree.
"""

import json
import os
import re
import shutil
import subprocess

import pytest

import check_no_prompt_patch as gate
import gen_model_text_catalog as cat

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))
PAD = " and this sentence was planted by a test to grow the class by exactly the planted bytes"


def _git(root, *args):
    subprocess.run(
        ["git", "-C", root, *args],
        check=True,
        capture_output=True,
        env={
            **os.environ,
            "GIT_AUTHOR_NAME": "t",
            "GIT_AUTHOR_EMAIL": "t@t",
            "GIT_COMMITTER_NAME": "t",
            "GIT_COMMITTER_EMAIL": "t@t",
        },
    )


def make_tree(dst):
    """A copy of everything the gates read, committed as the base of a temp git repo."""
    rels = {p for _, p in cat.e1_sources()} | {p for p, _ in cat.E2_EXTRA_SOURCES}
    rels |= {cat.CATALOGS["e1"]["golden"], gate.CHANGES, gate.REGISTER, "tools/release_gate/gen_model_text_catalog.py"}
    rels.add(gate.OWNERS)
    for rel in sorted(rels):
        src = os.path.join(REPO, rel)
        if os.path.isfile(src):
            os.makedirs(os.path.dirname(os.path.join(dst, rel)), exist_ok=True)
            shutil.copy(src, os.path.join(dst, rel))
    for d in ("tests/evals/owner_regressions/scenarios", "tests/evals/refloop/scenarios"):
        shutil.copytree(os.path.join(REPO, d), os.path.join(dst, d))
    _git(dst, "init", "-q", "-b", "main")
    _git(dst, "add", "-A")
    _git(dst, "commit", "-q", "-m", "base")
    _git(dst, "checkout", "-q", "-b", "lane")
    return dst


@pytest.fixture(scope="session")
def template(tmp_path_factory):
    return make_tree(str(tmp_path_factory.mktemp("template") / "t"))


def copy_template(template, tmp_path):
    """A fresh copy of the template repo, on branch lane; main is its merge base."""
    dst = str(tmp_path / "t")
    shutil.copytree(template, dst, symlinks=True)
    return dst


@pytest.fixture
def tree(tmp_path, template):
    return copy_template(template, tmp_path)


def merge(root):
    """Commit the lane's work and make it the base, as a merge into the integration branch does."""
    _git(root, "add", "-A")
    _git(root, "commit", "-q", "-m", "merged")
    _git(root, "branch", "-f", "main", "HEAD")


def base_of(root):
    return gate.resolve_base(root, "main")


def errors(root, use_base=True):
    return gate.run(root, base_of(root) if use_base else None, use_base=use_base, quiet=True)


def regen(root):
    assert cat.main(["--root", root, "--write"]) == 0


def read(root, rel):
    with open(os.path.join(root, rel), encoding="utf-8") as f:
        return f.read()


def write(root, rel, text):
    os.makedirs(os.path.dirname(os.path.join(root, rel)), exist_ok=True)
    with open(os.path.join(root, rel), "w", encoding="utf-8") as f:
        f.write(text)


def plain_literal(root, cls, catalog="e1"):
    """(path, text) of a catalog entry of the class that is one plain literal in its source."""
    for e in cat.parse(read(root, cat.CATALOGS[catalog]["golden"]))["entries"]:
        if e.cls != cls or len(e.text) < 30 or not re.fullmatch(r"[A-Za-z0-9 ,.;:()'/_-]+", e.text):
            continue
        src = read(root, e.path)
        if src.count('"' + e.text + '"') == 1:
            return e.path, e.text
    raise AssertionError(f"no plain literal in class {cls}")


def edit_literal(root, path, old, new):
    src = read(root, path)
    write(root, path, src.replace('"' + old + '"', '"' + new + '"', 1))


def stats(root, catalog="e1"):
    return cat.class_stats(catalog, cat.BUILDERS[catalog](root))


def append_entry(root, **fields):
    """Add an entry under ## Changes (replacing the 'None yet.' line)."""
    text = read(root, gate.CHANGES).replace("\nNone yet.\n", "\n")
    n = len(re.findall(r"^### PC-\d{4}", text, re.M)) + 1
    title = fields.pop("title", "a test change")
    lines = [f"### PC-{n:04d}: {title}"] + [f"- {k}: {v}" for k, v in fields.items()]
    write(root, gate.CHANGES, text.rstrip("\n") + "\n\n" + "\n".join(lines) + "\n")


def entry_for(root, catalog, cls, before, kind, **extra):
    now = stats(root, catalog)[cls]
    fields = {
        "catalog": catalog,
        "class": cls,
        "kind": kind,
        "bytes": f"{before['bytes']} -> {now['bytes']}",
        "sha8": f"{before['sha8']} -> {now['sha8']}",
        "lane": "RB-TEST",
        "why": "a test",
    }
    fields.update(extra)
    return fields


# red_at names the template's base commit (main), which exists in every copied tree.
SWAP_PROOF = {"scenario": "lc-day-total", "red_at": "main-commit", "rates": "before=0.60 after=0.98 (test)"}


def proof(root, **over):
    sha = subprocess.run(["git", "-C", root, "rev-parse", "main"], capture_output=True, text=True).stdout.strip()
    return {**SWAP_PROOF, "red_at": sha, **over}


# ---------------------------------------------------------------------------
# The real tree
# ---------------------------------------------------------------------------


def test_real_tree_is_green():
    base = gate.resolve_base(REPO, None)
    errs = gate.run(REPO, base, use_base=base is not None, quiet=True)
    assert errs == [], errs


def test_real_ledger_baseline_matches_the_engine1_ceilings():
    changes = gate.parse_changes(read(REPO, gate.CHANGES))
    for cls in cat.E1_CLASSES:
        assert changes["baseline"][("e1", cls)][0] == cat.E1_CEILINGS[cls]


def test_copied_tree_is_green(tree):
    assert errors(tree) == []


# ---------------------------------------------------------------------------
# Growth: a seeded 300 B addition fails in every class
# ---------------------------------------------------------------------------


@pytest.mark.parametrize("cls", cat.E1_CLASSES)
def test_a_seeded_300_byte_addition_fails_in_every_e1_class(tree, cls):
    path, text = plain_literal(tree, cls)
    # A class a remove entry shrank (RB-Q1b: device_notes) sits below its ceiling; the seed
    # still grows it past the ceiling, by at least 300 B. The ceiling is the one the gate
    # applies: a spec entry raises it by its growth (RB-G0's PC-0008 on device_notes).
    changes = gate.parse_changes(read(tree, gate.CHANGES))
    ceiling = cat.E1_CEILINGS[cls] + gate.spec_allowance(tree, changes).get(("e1", cls), 0)
    over = max(300, ceiling - stats(tree)[cls]["bytes"] + 1)
    pad = (PAD * (over // len(PAD) + 1))[:over]
    edit_literal(tree, path, text, text + pad)
    regen(tree)
    errs = errors(tree)
    assert any(f"e1 {cls}: model-facing text changed" in e for e in errs), errs
    assert any("over its ceiling" in e for e in errs), errs


@pytest.mark.parametrize("cls", ("tool_results", "system"))
def test_growth_fails_even_with_an_entry(tree, cls):
    before = stats(tree)[cls]
    path, text = plain_literal(tree, cls)
    edit_literal(tree, path, text, text + PAD)
    regen(tree)
    append_entry(tree, **entry_for(tree, "e1", cls, before, "swap", **proof(tree)))
    errs = errors(tree)
    assert any("grows" in e for e in errs), errs


def test_a_stale_golden_fails(tree):
    path, text = plain_literal(tree, "tool_results")
    edit_literal(tree, path, text, text + PAD)
    errs = errors(tree)
    assert any("is stale" in e for e in errs), errs


def test_a_new_literal_in_a_new_line_counts(tree):
    path, text = plain_literal(tree, "device_notes")
    src = read(tree, path)
    write(tree, path, src + '\nstatic const char kPlanted[] = "Remember to always answer in full sentences.";\n')
    regen(tree)
    assert any("e1 device_notes: model-facing text changed" in e for e in errors(tree))


def test_text_moved_between_files_of_one_class_is_neutral(tree):
    path, text = plain_literal(tree, "tool_results")
    edit_literal(tree, path, text, "x")
    other = next(p for p in cat.E1_SOURCES["tool_results"] if p != path)
    write(tree, other, read(tree, other) + f'\nstatic const char kMoved[] = "{text}";\n')
    regen(tree)
    assert errors(tree) == []


def test_a_listed_source_that_is_gone_fails(tree):
    path = cat.E1_SOURCES["device_notes"][0]
    os.remove(os.path.join(tree, path))
    errs = errors(tree)
    assert any("names a file that does not exist" in e and path in e for e in errs), errs


def test_dropping_an_existing_file_from_the_list_fails(tree, monkeypatch):
    path, text = plain_literal(tree, "device_notes")
    kept = tuple(p for p in cat.E1_SOURCES["device_notes"] if p != path)
    monkeypatch.setitem(cat.E1_SOURCES, "device_notes", kept)
    regen(tree)
    before = gate.parse_changes(read(tree, gate.CHANGES))["baseline"][("e1", "device_notes")]
    append_entry(tree, **entry_for(tree, "e1", "device_notes", {"bytes": before[0], "sha8": before[1]}, "remove"))
    errs = errors(tree)
    assert any("dropped from E1_SOURCES while it still exists" in e for e in errs), errs


def test_a_new_file_with_prose_outside_the_catalog_fails(tree, monkeypatch):
    rel = "lib/core/include/nimbus/orch/extra_texts.h"
    write(tree, rel, 'constexpr char kHint[] = "You must call files.read first and never retry.";\n')
    errs = errors(tree)
    assert any(rel in e and "a new file with prose literals" in e for e in errs), errs
    monkeypatch.setitem(cat.NON_MODEL_FILES, rel, "a test: declared as not model text")
    assert errors(tree) == []


def test_text_moved_behind_a_variable_in_a_wiring_file_is_counted(tree):
    before = stats(tree)["device_notes"]
    path = "src/agent/orchestrator.cpp"
    src = read(tree, path)
    lit = next(x for x in cat.model_literals(path, src) if len(x.text) > 60 and '"' + x.text + '"' in src)
    hidden = 'std::string m = "missing action. You must call device.state first."'
    write(tree, path, src.replace('"' + lit.text + '"', '"x"', 1) + "\nvoid planted() { " + hidden + "; }\n")
    regen(tree)
    append_entry(tree, **entry_for(tree, "e1", "device_notes", before, "remove"))
    errs = errors(tree)
    assert any("logged as remove but text was added" in e for e in errs), errs


def test_a_new_tool_file_outside_the_catalog_fails(tree):
    write(
        tree,
        "src/agent/new_tool.cpp",
        '#include "x.h"\nToolResult f() { return ToolResult::ok("done; now tell the owner"); }\n',
    )
    errs = errors(tree)
    assert any("in no catalog class" in e and "src/agent/new_tool.cpp" in e for e in errs), errs


# ---------------------------------------------------------------------------
# Byte-neutral swap: needs the red scenario, its commit and the rates
# ---------------------------------------------------------------------------


def _swap(tree, cls="tool_results", tail=" never mind."):
    before = stats(tree)[cls]
    path, text = plain_literal(tree, cls)
    new = text[: -len(tail)] + tail
    assert len(new) == len(text)
    edit_literal(tree, path, text, new)
    regen(tree)
    after = stats(tree)[cls]
    assert after["bytes"] == before["bytes"] and after["sha8"] != before["sha8"]
    return before


@pytest.mark.parametrize("cls", cat.E1_CLASSES)
def test_a_byte_neutral_swap_fails_without_an_entry(tree, cls):
    _swap(tree, cls)
    errs = errors(tree)
    assert any(f"e1 {cls}: model-facing text changed" in e for e in errs), errs


@pytest.mark.parametrize(
    "drop, message",
    [
        ("scenario", "red scenario id"),
        ("red_at", "red_at"),
        ("rates", "parity rates"),
    ],
)
def test_a_swap_entry_needs_scenario_commit_and_rates(tree, drop, message):
    before = _swap(tree)
    kept = {k: v for k, v in proof(tree).items() if k != drop}
    append_entry(tree, **entry_for(tree, "e1", "tool_results", before, "swap", **kept))
    errs = errors(tree)
    assert any(message in e for e in errs), errs


def test_a_swap_entry_with_an_unknown_scenario_fails(tree):
    before = _swap(tree)
    append_entry(
        tree, **entry_for(tree, "e1", "tool_results", before, "swap", **proof(tree, scenario="lc-does-not-exist"))
    )
    assert any("does not resolve" in e for e in errors(tree))


def test_a_swap_entry_with_rates_out_of_range_fails(tree):
    before = _swap(tree)
    append_entry(
        tree, **entry_for(tree, "e1", "tool_results", before, "swap", **proof(tree, rates="before=60 after=98"))
    )
    assert any("parity rates" in e for e in errors(tree))


def test_a_swap_that_lowers_the_parity_rate_fails(tree):
    before = _swap(tree)
    append_entry(
        tree,
        **entry_for(tree, "e1", "tool_results", before, "swap", **proof(tree, rates="before=0.98 after=0.90")),
    )
    assert any("parity rate fell" in e for e in errors(tree))


def test_a_base_ref_that_looks_like_an_option_is_refused(tree):
    assert gate.resolve_base(tree, "--output=/tmp/x") is None


def test_a_complete_swap_entry_passes(tree):
    before = _swap(tree)
    append_entry(tree, **entry_for(tree, "e1", "tool_results", before, "swap", **proof(tree)))
    assert errors(tree) == []


def test_a_swap_whose_red_commit_does_not_exist_fails(tree):
    before = _swap(tree)
    append_entry(tree, **entry_for(tree, "e1", "tool_results", before, "swap", **proof(tree, red_at="deadbeef")))
    assert any("red_at deadbeef is not a commit" in e for e in errors(tree))


def test_an_empty_file_named_for_the_scenario_does_not_resolve(tree):
    write(tree, "tests/evals/x/scenarios/lc-made-up.txt", "")
    write(tree, "tests/evals/x/scenarios/lc-made-up.json", "{}")
    assert not gate.scenario_exists(tree, "lc-made-up")
    write(tree, "tests/evals/x/scenarios/lc-made-up.json", '{"id": "lc-made-up"}')
    assert gate.scenario_exists(tree, "lc-made-up")


def test_a_refloop_scenario_id_resolves(tree):
    before = _swap(tree)
    append_entry(
        tree,
        **entry_for(tree, "e1", "tool_results", before, "swap", **proof(tree, scenario="recent-compare-food")),
    )
    assert errors(tree) == []


def test_an_entry_claiming_the_wrong_after_state_fails(tree):
    before = _swap(tree)
    fields = entry_for(tree, "e1", "tool_results", before, "swap", **SWAP_PROOF)
    fields["sha8"] = f"{before['sha8']} -> 00000000"
    append_entry(tree, **fields)
    assert any("no entry in" in e for e in errors(tree))


def test_entries_chain_an_old_entry_cannot_cover_a_new_change(tree):
    before = _swap(tree)
    append_entry(tree, **entry_for(tree, "e1", "tool_results", before, "swap", **proof(tree)))
    merge(tree)
    _swap(tree, tail=" once again.")  # a second change, no second entry
    assert any("e1 tool_results: model-facing text changed" in e for e in errors(tree))


def test_a_second_entry_must_start_where_the_first_ended(tree):
    before = _swap(tree)
    append_entry(tree, **entry_for(tree, "e1", "tool_results", before, "swap", **proof(tree)))
    append_entry(tree, **entry_for(tree, "e1", "tool_results", before, "swap", **proof(tree)))
    assert any("is not the last accepted state" in e for e in errors(tree))


# ---------------------------------------------------------------------------
# Removal
# ---------------------------------------------------------------------------


def test_a_removal_needs_an_entry(tree):
    path, text = plain_literal(tree, "device_notes")
    edit_literal(tree, path, text, "x")
    regen(tree)
    assert any("e1 device_notes: model-facing text changed" in e for e in errors(tree))


def test_a_removal_with_a_remove_entry_passes(tree):
    before = stats(tree)["device_notes"]
    path, text = plain_literal(tree, "device_notes")
    edit_literal(tree, path, text, "x")
    regen(tree)
    append_entry(tree, **entry_for(tree, "e1", "device_notes", before, "remove"))
    assert errors(tree) == []


def test_a_remove_entry_that_hides_added_text_fails(tree):
    before = stats(tree)["device_notes"]
    path, text = plain_literal(tree, "device_notes")
    edit_literal(tree, path, text, "Always call files.read first.")  # shorter, but new text
    regen(tree)
    append_entry(tree, **entry_for(tree, "e1", "device_notes", before, "remove"))
    errs = errors(tree)
    assert any("logged as remove but text was added" in e for e in errs), errs


# ---------------------------------------------------------------------------
# History and ceilings
# ---------------------------------------------------------------------------


def test_editing_a_merged_entry_fails(tree):
    before = _swap(tree)
    append_entry(tree, **entry_for(tree, "e1", "tool_results", before, "swap", **proof(tree)))
    merge(tree)
    write(tree, gate.CHANGES, read(tree, gate.CHANGES).replace("before=0.60", "before=0.10"))
    assert any("a merged entry was edited" in e for e in errors(tree))


@pytest.mark.parametrize("use_base", [True, False])
def test_editing_the_baseline_fails_with_or_without_a_base(tree, use_base):
    # A class no ledger entry has moved yet (RB-Q3a: an entry moves system), so its state is
    # its baseline row; a swap, then that row rewritten to match it.
    text = read(tree, gate.CHANGES)
    cls = next(
        c for c in cat.E1_CLASSES if f"| e1 | {c} | {stats(tree)[c]['bytes']} | {stats(tree)[c]['sha8']} |" in text
    )
    before = _swap(tree, cls)
    now = stats(tree)[cls]
    row = f"| e1 | {cls} | {before['bytes']} | {before['sha8']} |"
    assert row in text
    write(tree, gate.CHANGES, text.replace(row, f"| e1 | {cls} | {now['bytes']} | {now['sha8']} |"))
    errs = errors(tree, use_base=use_base)
    assert any("baseline" in e and "differs" in e for e in errs), errs


def test_an_e1_ceiling_above_its_baseline_fails(tree, monkeypatch):
    monkeypatch.setitem(cat.E1_CEILINGS, "system", cat.E1_CEILINGS["system"] + 300)
    regen(tree)
    assert any("above its baseline" in e for e in errors(tree))


def test_no_base_fails_closed_outside_ci(tree, monkeypatch):
    monkeypatch.delenv("GITHUB_ACTIONS", raising=False)
    assert any("no git base" in e for e in gate.run(tree, None, use_base=True, quiet=True))


def test_main_markers(tree, capsys):
    assert gate.main(["--root", tree, "--base", "main", "--quiet"]) == 0
    assert capsys.readouterr().out.strip().splitlines()[-1].startswith("NO PROMPT PATCH GREEN")
    _swap(tree)
    assert gate.main(["--root", tree, "--base", "main", "--quiet"]) == 1
    assert capsys.readouterr().out.strip().splitlines()[-1] == "NO PROMPT PATCH RED"


# ---------------------------------------------------------------------------
# Engine 2: texts.h by namespace, the plug-in point for RB-E2
# ---------------------------------------------------------------------------

TEXTS_H = """#pragma once
// The model-facing text of the plain engine (PRD section 4). One namespace per class.
namespace nimbus {{
namespace harness {{
namespace plain {{
namespace texts {{
namespace core {{
constexpr char kIdentity[] = "You are Nimbus, a personal assistant on a small device.{core}";
}}  // namespace core
namespace device_notes {{
constexpr char kRestart[] = "[device] the device restarted at {{time}}";
}}
namespace stubs {{
constexpr char kStub[] = "result r{{id}} stored, {{bytes}} B; view brings it back";
}}
namespace tool_results {{
constexpr char kWrote[] = "wrote {{path}} size={{size}}";
}}
namespace probe {{
constexpr char kProbe[] = "Does the reply claim an effect the list does not show?";
}}
namespace summarizer {{
constexpr char kSummarize[] = "Summarize the day as a dated digest.";
}}
namespace tool_descriptions {{
constexpr char kReadFile[] = "read a file by path with exact paging";
}}
}}  // namespace texts
}}  // namespace plain
}}  // namespace harness
}}  // namespace nimbus
"""


def _e2(tree, core=""):
    write(tree, cat.E2_TEXTS, TEXTS_H.format(core=core))
    regen(tree)


def _e2_ledger_state(tree):
    """Each e2 class's last accepted state in the tree's ledger (the repository's own entries chain first)."""
    state = {}
    for e in gate.parse_changes(read(tree, gate.CHANGES))["entries"]:
        if e.get("catalog") == "e2":
            state[e["class"]] = {"bytes": int(e["bytes"].split("->")[1]), "sha8": e["sha8"].split("->")[1].strip()}
    return state


def _e2_build_entries(tree, before=None):
    st = stats(tree, "e2")
    before = before or _e2_ledger_state(tree)
    for cls in cat.E2_CLASSES:
        b = before.get(cls, {"bytes": 0, "sha8": gate.EMPTY_SHA8})
        if (b["bytes"], b["sha8"]) == (st[cls]["bytes"], st[cls]["sha8"]):
            continue
        append_entry(tree, **entry_for(tree, "e2", cls, b, "build", prd="section 4", lane="RB-E2"))


def test_e2_is_not_live_without_texts_h(tree):
    assert cat.build_e2(tree) is None
    assert not os.path.exists(os.path.join(tree, cat.CATALOGS["e2"]["golden"]))


def test_e2_catalog_needs_build_entries(tree):
    _e2(tree)
    errs = errors(tree)
    assert {f"e2 {c}" for c in cat.E2_CLASSES} <= {e.split(":")[0] for e in errs}, errs
    _e2_build_entries(tree)
    assert errors(tree) == []


def test_e2_seeded_300_byte_addition_fails(tree):
    _e2(tree)
    _e2_build_entries(tree)
    merge(tree)
    _e2(tree, core=(PAD * 5)[:300])
    errs = errors(tree)
    assert any("e2 core: model-facing text changed" in e for e in errs), errs


def test_e2_build_growth_is_refused_once_frozen(tree, monkeypatch):
    _e2(tree)
    monkeypatch.setitem(cat.CATALOGS["e2"], "phase", "frozen")
    _e2_build_entries(tree)
    assert any("kind build is refused" in e for e in errors(tree))


def test_e2_build_growth_over_the_ceiling_fails(tree):
    _e2(tree, core="x " * 1600)
    _e2_build_entries(tree)
    errs = errors(tree)
    assert any("e2 core" in e and "over its ceiling" in e for e in errs), errs


def test_e2_literal_outside_a_class_namespace_fails(tree):
    write(tree, cat.E2_TEXTS, TEXTS_H.format(core="") + 'constexpr char kLoose[] = "loose text here";\n')
    errs = errors(tree)
    assert any("in no class namespace" in e for e in errs), errs


def test_e2_model_text_in_a_plain_engine_file_fails(tree):
    _e2(tree)
    _e2_build_entries(tree)
    write(
        tree,
        "lib/harness/src/plain_request.cpp",
        'std::string sys() { return std::string("Always check the ledger before you answer."); }\n'
        'void f() { pending("plain::stub marker text"); hlog::logf("a log line %d", 1); }\n',
    )
    errs = errors(tree)
    assert any("model text outside texts.h" in e and "plain_request.cpp:1" in e for e in errs), errs
    assert not any("plain_request.cpp:2" in e for e in errs), errs


def test_e2_extra_manifest_source_is_counted(tree, monkeypatch):
    _e2(tree)
    _e2_build_entries(tree)
    assert errors(tree) == []
    write(tree, "lib/core/src/orch_tool_manifest.cpp", 'const char* kDesc = "send a file to this chat";\n')
    monkeypatch.setattr(
        cat, "E2_EXTRA_SOURCES", cat.E2_EXTRA_SOURCES + (("lib/core/src/orch_tool_manifest.cpp", "tool_descriptions"),)
    )
    regen(tree)
    assert any("e2 tool_descriptions: model-facing text changed" in e for e in errors(tree))


# ---------------------------------------------------------------------------
# The register: a red scenario per row, no 'missing check' root cause
# ---------------------------------------------------------------------------

ROW = "| OIR-{n} | 18 Nov | The owner saw X | class | {cause} | open | oir_x | {live} |"


def _register(tree, **kw):
    text = read(tree, gate.REGISTER)
    last = list(re.finditer(r"^\| OIR-\d+ \|.*$", text, re.M))[-1]
    row = ROW.format(**{"n": 56, "cause": "the ledger day ended at midnight, not 04:00", "live": "lc-day-total", **kw})
    write(tree, gate.REGISTER, text[: last.end()] + "\n" + row + text[last.end() :])


def test_register_row_with_a_scenario_passes(tree):
    _register(tree)
    assert gate.check_register(tree) == []


@pytest.mark.parametrize("live", ["n/a (deterministic)", "lc-not-a-scenario", ""])
def test_register_row_needs_a_red_scenario(tree, live):
    _register(tree, live=live)
    errs = gate.check_register(tree)
    assert any("names no scenario" in e for e in errs), errs


@pytest.mark.parametrize(
    "cause", ["missing check on the reply", "no guard on the total", "the claim check lacked a test"]
)
def test_register_rejects_missing_check_as_root_cause(tree, cause):
    _register(tree, cause=cause)
    errs = gate.check_register(tree)
    assert any("not an accepted root cause" in e for e in errs), errs


def test_register_grandfathered_rows_are_left_alone():
    text = read(REPO, gate.REGISTER)
    assert "| OIR-55 |" in text and gate.REGISTER_LAST_GRANDFATHERED == 55
    assert gate.check_register(REPO) == []


# ---------------------------------------------------------------------------
# Re-baseline: the orchestrator's repeatable step when a release line arrives
# ---------------------------------------------------------------------------


@pytest.fixture
def isolated_baseline(monkeypatch):
    """rebaseline() updates the module's baseline in place: keep that inside the test."""
    monkeypatch.setattr(cat, "E1_BASELINE", dict(cat.E1_BASELINE))
    monkeypatch.setattr(cat, "E1_CEILINGS", dict(cat.E1_CEILINGS))


def _release_line_growth(tree):
    path, lit = plain_literal(tree, "side_prompts")
    edit_literal(tree, path, lit, lit + " (a notice the release line added)")


def test_a_rebaseline_makes_a_release_line_growth_the_baseline(tree, isolated_baseline):
    _release_line_growth(tree)
    regen(tree)
    assert any("e1 side_prompts" in e for e in errors(tree))
    record = cat.rebaseline(tree, "a release line added a notice")
    assert "- e1 side_prompts: " in record and "(+" in record
    assert errors(tree) == []
    assert cat.main(["--root", tree]) == 0


def test_a_rebaseline_is_repeatable_and_records_chain(tree, isolated_baseline):
    already = len(gate.parse_changes(read(tree, gate.CHANGES))["rebaselines"])
    _release_line_growth(tree)
    cat.rebaseline(tree, "round 1")
    merge(tree)
    path, lit = plain_literal(tree, "device_notes")
    edit_literal(tree, path, lit, lit + " (round 2)")
    cat.rebaseline(tree, "round 2")
    assert errors(tree) == []
    assert len(gate.parse_changes(read(tree, gate.CHANGES))["rebaselines"]) == already + 2


def test_a_moved_baseline_without_a_record_fails(tree, isolated_baseline):
    _release_line_growth(tree)
    cat.rebaseline(tree, "a release line added a notice")
    text = read(tree, gate.CHANGES)
    start = text.rindex("### RBL-")
    write(tree, gate.CHANGES, text[:start] + text[text.index("\n## Changes", start) + 1 :])
    errs = errors(tree)
    assert any("without a re-baseline record" in e for e in errs), errs


def test_a_merged_rebaseline_record_cannot_be_edited(tree, isolated_baseline):
    _release_line_growth(tree)
    cat.rebaseline(tree, "a release line added a notice")
    merge(tree)
    write(tree, gate.CHANGES, read(tree, gate.CHANGES).replace("a release line added a notice", "nothing moved"))
    assert any("a merged re-baseline was edited" in e for e in errors(tree))


def test_a_rebaseline_record_that_does_not_end_at_the_table_fails(tree, isolated_baseline):
    _release_line_growth(tree)
    cat.rebaseline(tree, "a release line added a notice")
    text = read(tree, gate.CHANGES)
    row = list(re.finditer(r"^- e1 side_prompts: \d+ [0-9a-f]{8} -> (\d+) ", text, re.M))[-1]
    write(
        tree, gate.CHANGES, text.replace(row.group(0), row.group(0).replace(row.group(1), str(int(row.group(1)) + 1)))
    )
    errs = errors(tree, use_base=False)
    assert any("does not end at the baseline table" in e for e in errs), errs


def test_a_rebaseline_keeps_the_engine2_entries_live(tree, isolated_baseline):
    # Lane RB-SYNC: a re-baseline re-measures engine 1 only, so the e2 build entries before it stay
    # in the chain (RBL-0002 superseded PC-0003..PC-0008 and turned the e2 classes RED before this).
    _e2(tree)
    _e2_build_entries(tree)
    merge(tree)
    assert errors(tree) == []
    _release_line_growth(tree)
    regen(tree)
    cat.rebaseline(tree, "a release line added a notice")
    assert errors(tree) == []
    _e2(tree, core=" Grown after the record.")
    assert any("e2 core: model-facing text changed" in e for e in errors(tree))


def test_a_superseded_spec_entry_raises_no_ceiling(tree, plan, isolated_baseline):
    before = _grow(tree)
    append_entry(tree, **entry_for(tree, "e1", "tool_descriptions", before, "spec", **SPEC))
    merge(tree)
    changes = gate.parse_changes(read(tree, gate.CHANGES))
    assert gate.spec_allowance(tree, changes).get(("e1", "tool_descriptions"), 0) > 0
    cat.rebaseline(tree, "a release line arrived")
    changes = gate.parse_changes(read(tree, gate.CHANGES))
    assert ("e1", "tool_descriptions") not in gate.spec_allowance(tree, changes)
    assert errors(tree) == []
    _grow(tree, text=" and a growth the superseded spec entry must not cover")
    errs = errors(tree)
    assert any("e1 tool_descriptions: " in e and "over its ceiling" in e for e in errs), errs


# ---------------------------------------------------------------------------
# Planned work (ruling 18): kind spec, and the release total
# ---------------------------------------------------------------------------


@pytest.fixture
def plan(tmp_path, monkeypatch):
    """A live plan: the real plan's lanes (the ledger's own spec entries name them) and RB-L1b, never RB-ZZ9."""
    path = tmp_path / "PLAN.json"
    lanes = (gate.plan_lanes(REPO) | {"RB-L1b"}) - {"RB-ZZ9"}
    path.write_text(json.dumps({"lanes": [{"id": lane} for lane in sorted(lanes)], "stages": []}))
    monkeypatch.setattr(gate, "PLAN_JSON", str(path))
    return path


def _grow(tree, cls="tool_descriptions", text=PAD):
    before = stats(tree)[cls]
    path, lit = plain_literal(tree, cls)
    edit_literal(tree, path, lit, lit + text)
    regen(tree)
    return before


SPEC = {"lane": "RB-L1b", "prd": "section 4 (ledger tools)"}


def test_a_spec_entry_for_a_planned_lane_may_grow_one_class(tree, plan):
    before = _grow(tree)
    append_entry(tree, **entry_for(tree, "e1", "tool_descriptions", before, "spec", **SPEC))
    assert errors(tree) == []


def test_a_spec_entry_for_a_lane_not_in_the_plan_fails(tree, plan):
    before = _grow(tree)
    append_entry(tree, **entry_for(tree, "e1", "tool_descriptions", before, "spec", **{**SPEC, "lane": "RB-ZZ9"}))
    errs = errors(tree)
    assert any("a spec entry names a lane of the live plan" in e for e in errs), errs
    assert any("over its ceiling" in e for e in errs), errs


def test_a_spec_entry_needs_a_prd_section(tree, plan):
    before = _grow(tree)
    append_entry(tree, **entry_for(tree, "e1", "tool_descriptions", before, "spec", **{**SPEC, "prd": "the ledger"}))
    assert any("names the PRD section" in e for e in errors(tree))


def test_a_spec_entry_reused_for_a_second_change_fails(tree, plan):
    before = _grow(tree)
    append_entry(tree, **entry_for(tree, "e1", "tool_descriptions", before, "spec", **SPEC))
    merge(tree)
    assert errors(tree) == []
    _grow(tree, text=" and a second planted growth that no entry covers yet")  # same class, no new entry
    errs = errors(tree)
    assert any("e1 tool_descriptions: model-facing text changed" in e for e in errs), errs
    assert any("over its ceiling" in e for e in errs), errs


def test_a_copied_spec_entry_cannot_cover_a_second_change(tree, plan):
    before = _grow(tree)
    first = entry_for(tree, "e1", "tool_descriptions", before, "spec", **SPEC)
    append_entry(tree, **first)
    _grow(tree, text=" and a second planted growth that no entry covers yet")
    append_entry(tree, **first, title="the first entry copied")
    errs = errors(tree)
    assert any("is not the last accepted state" in e for e in errs), errs


def test_a_spec_entry_grows_only_its_own_class(tree, plan):
    before = _grow(tree)
    append_entry(tree, **entry_for(tree, "e1", "tool_descriptions", before, "spec", **SPEC))
    _grow(tree, cls="tool_results")
    errs = errors(tree)
    assert any("e1 tool_results: model-facing text changed" in e for e in errs), errs


def test_without_the_plan_file_the_owners_table_names_the_lanes(tree, monkeypatch):
    monkeypatch.setattr(gate, "PLAN_JSON", os.path.join(tree, "no-plan.json"))
    lanes = gate.plan_lanes(tree)
    assert "RB-B5" in lanes and "RB-ZZ9" not in lanes


def test_the_release_total_above_the_baseline_fails(tree, monkeypatch):
    monkeypatch.setattr(cat, "E1_BASELINE", {c: (st["bytes"], st["sha8"]) for c, st in stats(tree).items()})
    _grow(tree)
    errs = gate.release_total_problems(tree)
    assert errs and "not lower than its baseline" in errs[0], errs


def test_the_release_total_equal_to_the_baseline_fails_and_lower_passes(tree, monkeypatch, capsys):
    monkeypatch.setattr(cat, "E1_BASELINE", {c: (st["bytes"], st["sha8"]) for c, st in stats(tree).items()})
    assert gate.release_total_problems(tree)  # equal is not lower
    path, lit = plain_literal(tree, "skills")
    edit_literal(tree, path, lit, "x")
    assert gate.release_total_problems(tree) == []
    assert gate.main(["--root", tree, "--release"]) == 0
    assert capsys.readouterr().out.strip().splitlines()[-1] == "PROMPT TOTAL GREEN"
