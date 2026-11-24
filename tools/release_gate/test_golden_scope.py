"""Host tests for tools/router/check_golden_scope.py, the L5 scope gate of the engine-2 router.

The gate passes on this branch and on a change that names what it moves; each way a change can
move another provider's goldens unnoticed is counter-tested by a planted change, the last one in a
real git history so the staged and range plumbing is exercised too.
"""

import shutil
import subprocess
import sys
from pathlib import Path

import pytest

REPO = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO / "tools" / "router"))

import check_golden_scope as gs  # noqa: E402

G = gs.GOLDENS
ANT = G + "messages-anthropic-direct/one_call.req"
MIS = G + "chat-mistral-direct/one_call.req"
OAI = G + "responses-openai-routed/text_only.req"
MODELS_TEXT = (REPO / gs.MODELS).read_text(encoding="utf-8")


def test_the_tree_passes():
    assert gs.main(["--range", "HEAD~1..HEAD"]) == 0


def test_signatures_name_their_dialect_and_slot():
    assert gs.golden_of(ANT) == ("messages-anthropic-direct", "messages", "anthropic")
    assert gs.golden_of(G + "chat-custom_chat-direct/x.req") == ("chat-custom_chat-direct", "chat", "custom_chat")
    assert gs.golden_of(G + "MANIFEST") is None
    assert gs.golden_of("lib/x.cpp") is None
    assert gs.golden_of(G + "chat-cumulo-direct/x.req") == ("chat-cumulo-direct", None, None)


def test_a_codec_change_may_move_only_its_own_dialect():
    codec = "lib/harness/src/providers/plain_wire_chat.cpp"
    assert gs.verdict([codec, MIS], set(), set(), {}, set()) == []
    errs = gs.verdict([codec, MIS, OAI], set(), set(), {}, set())
    assert len(errs) == 1 and OAI in errs[0] and "openai" in errs[0]


def test_a_planner_change_names_nothing_until_it_lists_its_goldens():
    planner = "lib/harness/src/providers/plain_plan.cpp"
    assert len(gs.verdict([planner, MIS, OAI], set(), set(), {}, set())) == 2
    names, _ = gs.parse_names(["fix the pairing\n\nGolden-scope: mistral, responses-openai-routed\n"])
    assert gs.verdict([planner, MIS, OAI], set(), names, {}, set()) == []
    names, _ = gs.parse_names(["Golden-scope: chat-mistral-direct/one_call.req"])
    assert gs.verdict([planner, MIS], set(), names, {}, set()) == []
    assert len(gs.verdict([planner, MIS, G + "chat-mistral-direct/two.req"], set(), names, {}, set())) == 1


def test_a_releasable_slot_needs_a_ruling_even_when_named():
    codec = "lib/harness/src/providers/plain_wire_anthropic.cpp"
    errs = gs.verdict([codec, ANT], set(), set(), {}, {"anthropic"})
    assert len(errs) == 1 and "Golden-ruling: anthropic" in errs[0]
    _, rulings = gs.parse_names(["Golden-ruling: anthropic 121"])
    assert gs.verdict([codec, ANT], set(), set(), rulings, {"anthropic"}) == []


def test_an_unknown_signature_fails_closed():
    errs = gs.verdict([G + "chat-cumulo-direct/x.req"], set(gs.gen_caps.SLOTS), set(), {}, set())
    assert len(errs) == 1 and "names no known dialect" in errs[0]


def test_models_toml_names_the_slots_whose_rows_moved():
    assert gs.slots_moved(MODELS_TEXT, MODELS_TEXT) == set()
    mistral_only = MODELS_TEXT.replace('empty_text = { v = "(empty)"', 'empty_text = { v = "(blank)"', 1)
    assert gs.slots_moved(MODELS_TEXT, mistral_only) == {"mistral"}
    # A template change moves every slot that inherits it: claude5 is Anthropic's alone.
    claude = MODELS_TEXT.replace(
        "[template.claude5]\ninherit = \"failsafe\"\n", "[template.claude5]\ninherit = \"failsafe\"\nnote_x = 1\n", 1
    )
    assert gs.slots_moved(MODELS_TEXT, claude) == {"anthropic"}
    assert gs.slots_moved(None, MODELS_TEXT) == set(gs.gen_caps.SLOTS)
    # A Mistral row change cannot carry an OpenAI golden.
    assert len(gs.verdict([gs.MODELS, MIS, OAI], {"mistral"}, set(), {}, set())) == 1


def git(cwd, *args):
    return subprocess.run(["git", *args], cwd=cwd, check=True, capture_output=True, text=True).stdout


@pytest.fixture()
def repo(tmp_path, monkeypatch):
    """A scratch repository holding models.toml, the record and two goldens."""
    for rel in (gs.MODELS, "tools/router/release_record.json"):
        (tmp_path / rel).parent.mkdir(parents=True, exist_ok=True)
        shutil.copy(REPO / rel, tmp_path / rel)
    for rel in (MIS, OAI):
        (tmp_path / rel).parent.mkdir(parents=True, exist_ok=True)
        (tmp_path / rel).write_text("golden v1\n")
    git(tmp_path, "init", "-q")
    git(tmp_path, "-c", "user.name=t", "-c", "user.email=t@example.invalid", "add", "-A")
    git(
        tmp_path, "-c", "user.name=t", "-c", "user.email=t@example.invalid", "commit", "-q", "-m", "base", "--no-verify"
    )
    monkeypatch.setattr(gs, "ROOT", tmp_path)
    monkeypatch.delenv("GOLDEN_SCOPE", raising=False)
    monkeypatch.delenv("GOLDEN_RULING", raising=False)
    return tmp_path


def commit(repo, msg):
    git(repo, "-c", "user.name=t", "-c", "user.email=t@example.invalid", "commit", "-q", "-m", msg, "--no-verify")


def test_planted_cross_provider_change_fails_staged_and_in_range(repo, monkeypatch):
    models = repo / gs.MODELS
    models.write_text(models.read_text().replace('empty_text = { v = "(empty)"', 'empty_text = { v = "(blank)"', 1))
    (repo / MIS).write_text("golden v2\n")
    (repo / OAI).write_text("golden v2\n")  # the planted cross-provider move
    git(repo, "add", "-A")
    assert gs.main([]) == 1
    monkeypatch.setenv("GOLDEN_SCOPE", "openai")
    assert gs.main([]) == 0
    commit(repo, "mistral: placeholder text")
    assert gs.main(["--range", "HEAD~1..HEAD"]) == 1
    body = repo / "pr.md"
    body.write_text("Golden-scope: responses-openai-routed\n")
    assert gs.main(["--range", "HEAD~1..HEAD", "--pr-body", str(body)]) == 0
    (repo / OAI).write_text("golden v3\n")
    git(repo, "add", "-A")
    commit(repo, "openai bytes\n\nGolden-scope: openai\n")
    assert gs.main(["--range", "HEAD~1..HEAD"]) == 0


def test_only_the_releasable_route_needs_a_ruling():
    codec = "lib/harness/src/providers/plain_wire_anthropic.cpp"
    routed = G + "messages-anthropic-routed/one_call.req"
    assert gs.verdict([codec, routed], set(), set(), {}, {"anthropic-direct"}) == []
    assert len(gs.verdict([codec, ANT], set(), set(), {}, {"anthropic-direct"})) == 1
    record = {"providers": {"anthropic": {"direct": {"releasable": True}, "routed": {"releasable": False}}}}
    assert gs.releasable_slots(record) == {"anthropic-direct"}
