"""Host tests for the release tag shape gate and its wiring into release.yml.

Run: python3 -m pytest tools/release_gate
"""

import itertools
import re
from pathlib import Path

import pytest

import tag_shape as ts

REPO = Path(__file__).resolve().parents[2]
WORKFLOW = REPO / ".github" / "workflows" / "release.yml"
VER = "v4.5.7"


def channel(tag, version=VER):
    return ts.classify(tag, version)[0]


# --- the tag shapes the release channels are defined by ---------------------
def test_plain_release_tag_is_ota():
    assert channel("v4.5.7") == ts.OTA


def test_release_candidate_tag_is_ota():
    assert channel("v4.5.7-rc1") == ts.OTA


def test_numbered_virtual_tag_is_virtual():
    assert channel("vn-v4.5.7-1") == ts.VIRTUAL
    assert channel("vn-v4.5.7-2") == ts.VIRTUAL
    assert channel("vn-v4.5.7-12") == ts.VIRTUAL


def test_bare_virtual_tag_is_virtual():
    assert channel("vn-v4.5.7") == ts.VIRTUAL


def test_virtual_tag_for_another_version_is_refused():
    ch, why = ts.classify("vn-v9.9.9", VER)
    assert ch is None
    assert "v9.9.9" in why and VER in why


def test_numbered_virtual_tag_for_another_version_is_refused():
    assert channel("vn-v9.9.9-1") is None
    assert channel("vn-v4.5.6-1") is None


def test_release_tag_for_another_version_is_refused():
    ch, why = ts.classify("v9.9.9", VER)
    assert ch is None and "!= NIMBUS_FW_VERSION v4.5.7" in why


@pytest.mark.parametrize("suffix", ["0", "01", "rc1", "", "1a", "1-2", "-1", "a"])
def test_virtual_release_number_must_be_a_positive_integer(suffix):
    ch, why = ts.classify(f"vn-v4.5.7-{suffix}", VER)
    assert ch is None and "release number" in why


def test_version_prefix_is_not_enough():
    # v4.5.70 must not pass as v4.5.7 (a startswith without the separator would).
    assert channel("v4.5.70") is None
    assert channel("vn-v4.5.70") is None
    assert channel("vn-v4.5.7", version="v4.5.70") is None


def test_other_v_tags_are_refused():
    for tag in ("v4.5.7-1", "vn4.5.7", "vnv4.5.7", "vn-4.5.7", "vn-", "v", "vx-v4.5.7", "vn-vn-v4.5.7"):
        assert channel(tag) is None, tag


@pytest.mark.parametrize(
    "tag", ["v4.5.7-rc1+meta", "v4.5.7-rc1/x", "v4.5.7-rc" + "1" * 130, "", "v4.5.7-rc1\n", "vn-v4.5.7-1\n", "v4.5.7\n"]
)
def test_tag_that_cannot_be_an_image_tag_is_refused(tag):
    # The image job pushes nimbusd:<tag>; a tag the registry would reject must stop
    # BEFORE the firmware job publishes, not half way through the run.
    ch, why = ts.classify(tag, VER)
    assert ch is None and "not a valid image tag" in why


def test_empty_version_refuses_everything():
    for version in ("", None):
        assert ts.classify("v4.5.7", version)[0] is None
        assert ts.classify("vn-", version)[0] is None


def test_accepted_iff_the_tag_names_the_current_version():
    """The class rule: over every combination of prefix x version x suffix, a tag is
    accepted exactly when it names version.h in one of the two shapes."""
    versions = [VER, "v4.5.8", "v4.6.7", "v5.5.7", "v4.5.70", "v4.5", "4.5.7", "v14.5.7"]
    suffixes = ["", "-1", "-2", "-10", "-rc1", "-rc2", "-rc", "-rcx", "-0", "-x", "-1-1"]
    # Firmware tags keep the old shell gate's `"$VER"-rc*` exactly: anything after -rc.
    expected = {VER + s: ts.OTA for s in ("", "-rc1", "-rc2", "-rc", "-rcx")}
    expected.update({"vn-" + VER + s: ts.VIRTUAL for s in ("", "-1", "-2", "-10")})
    seen = set()
    for prefix, ver, suffix in itertools.product(["", "vn-", "v"], versions, suffixes):
        tag = prefix + ver + suffix
        want = expected.get(tag)
        assert channel(tag) == want, tag
        seen.add(want)
    assert seen == {ts.OTA, ts.VIRTUAL, None}


# --- reading include/version.h ----------------------------------------------
def test_read_version_ignores_the_build_define():
    text = '#define NIMBUS_FW_VERSION "v4.5.7"\n#ifndef NIMBUS_FW_BUILD\n#define NIMBUS_FW_BUILD NIMBUS_FW_VERSION\n#endif\n'
    assert ts.read_version(text) == "v4.5.7"


def test_read_version_none_when_missing_or_empty():
    assert ts.read_version("#pragma once\n") is None
    assert ts.read_version('#define NIMBUS_FW_VERSION ""\n') is None
    assert ts.read_version('// #define NIMBUS_FW_VERSION "v1.0.0"\n') is None


def test_read_version_matches_the_smoke_steps_sed_on_the_real_header():
    # The image smoke step reads VER with sed; the gate must read the same value.
    text = (REPO / "include" / "version.h").read_text(encoding="utf-8")
    sed = [m.group(1) for line in text.splitlines() if (m := re.search(r'#define NIMBUS_FW_VERSION "(.*)"', line))]
    assert sed == [ts.read_version(text)]
    assert re.fullmatch(r"v\d+\.\d+\.\d+", sed[0])


# --- the CLI the workflow calls ---------------------------------------------
@pytest.fixture
def version_h(tmp_path):
    p = tmp_path / "version.h"
    p.write_text(f'#define NIMBUS_FW_VERSION "{VER}"\n', encoding="utf-8")
    return p


@pytest.fixture
def gh_output(tmp_path, monkeypatch):
    out = tmp_path / "github_output"
    out.write_text("", encoding="utf-8")
    monkeypatch.setenv("GITHUB_OUTPUT", str(out))
    monkeypatch.setenv("GITHUB_ACTIONS", "true")
    return out


@pytest.mark.parametrize("tag,want", [("v4.5.7", "ota"), ("v4.5.7-rc1", "ota"), ("vn-v4.5.7-1", "virtual")])
def test_cli_writes_the_channel_output(version_h, gh_output, capsys, tag, want):
    assert ts.main(["--tag", tag, "--version-h", str(version_h)]) == 0
    assert gh_output.read_text(encoding="utf-8") == f"channel={want}\n"
    assert f"channel={want}" in capsys.readouterr().out


def test_cli_refusal_exits_1_and_writes_no_channel(version_h, gh_output, capsys):
    assert ts.main(["--tag", "vn-v9.9.9", "--version-h", str(version_h)]) == 1
    assert gh_output.read_text(encoding="utf-8") == ""
    assert "::error::" in capsys.readouterr().out


def test_cli_tag_defaults_to_github_ref_name(version_h, gh_output, monkeypatch):
    monkeypatch.setenv("GITHUB_REF_NAME", "vn-v4.5.7")
    assert ts.main(["--version-h", str(version_h)]) == 0
    assert gh_output.read_text(encoding="utf-8") == "channel=virtual\n"


def test_cli_fails_closed_in_a_workflow_without_github_output(version_h, monkeypatch, capsys):
    # Without the output every job would skip and the run would read green.
    monkeypatch.delenv("GITHUB_OUTPUT", raising=False)
    monkeypatch.setenv("GITHUB_ACTIONS", "true")
    assert ts.main(["--tag", "v4.5.7", "--version-h", str(version_h)]) == 1
    assert "GITHUB_OUTPUT" in capsys.readouterr().out


def test_cli_outside_a_workflow_just_prints(version_h, monkeypatch, capsys):
    monkeypatch.delenv("GITHUB_OUTPUT", raising=False)
    monkeypatch.delenv("GITHUB_ACTIONS", raising=False)
    assert ts.main(["--tag", "vn-v4.5.7-3", "--version-h", str(version_h)]) == 0
    assert "channel=virtual" in capsys.readouterr().out


def test_cli_missing_or_versionless_header_refuses(tmp_path, gh_output):
    assert ts.main(["--tag", "v4.5.7", "--version-h", str(tmp_path / "nope.h")]) == 1
    blank = tmp_path / "blank.h"
    blank.write_text("#pragma once\n", encoding="utf-8")
    assert ts.main(["--tag", "v4.5.7", "--version-h", str(blank)]) == 1
    assert gh_output.read_text(encoding="utf-8") == ""


# --- release.yml wiring: every job is gated on the channel tag-shape decides --
CLASSIFIER = "tag-shape"
# The channels each release.yml job may run on. A new job must be declared here AND
# gated in the workflow, or test_release_workflow_gates_every_job_on_its_channels fails.
JOB_CHANNELS = {"release": {ts.OTA}, "nimbusd-image": {ts.OTA, ts.VIRTUAL}}

_JOB_RE = re.compile(r"^  ([A-Za-z0-9_-]+):\s*(?:#.*)?$")
_KEY_RE = re.compile(r"^    (if|needs):\s*(.*?)\s*$")
_GATE_RE = re.compile(r"needs\.tag-shape\.outputs\.channel == '([a-z]+)'")


def workflow_jobs(text):
    """{job id: {"if": expr or None, "needs": [ids], "body": [lines]}} of the top-level jobs."""
    jobs, cur, in_jobs = {}, None, False
    for line in text.splitlines():
        if not in_jobs:
            in_jobs = line.rstrip() == "jobs:"
            continue
        if line[:1] not in ("", " ", "#"):
            break  # the next top-level key ends the jobs map
        job = _JOB_RE.match(line)
        if job:
            cur = jobs.setdefault(job.group(1), {"if": None, "needs": [], "body": []})
            continue
        if cur is None:
            continue
        cur["body"].append(line)
        key = _KEY_RE.match(line)
        if key is None:
            continue
        name, value = key.groups()
        if name == "if":
            cur["if"] = value
        else:
            cur["needs"] = [n.strip() for n in value.strip("[]").split(",") if n.strip()]
    return jobs


def admitted_channels(expr):
    """The channels a job-level `if:` admits; None unless it is only ORed channel tests."""
    if expr is None:
        return None
    expr = expr.strip()
    if expr.startswith("${{") and expr.endswith("}}"):
        expr = expr[3:-2].strip()
    chans = set()
    for part in expr.split("||"):
        m = _GATE_RE.fullmatch(part.strip())
        if not m:
            return None
        chans.add(m.group(1))
    return chans


def check_workflow(text):
    jobs = workflow_jobs(text)
    shape = jobs.pop(CLASSIFIER, None)
    if shape is None:
        return [f"no {CLASSIFIER} job"]
    body = "\n".join(shape["body"])
    errs = []
    if shape["if"] is not None or shape["needs"]:
        errs.append(f"{CLASSIFIER} must run first and unconditionally")
    for needle in (
        "id: shape",
        "python3 tools/release_gate/tag_shape.py",
        "channel: ${{ steps.shape.outputs.channel }}",
    ):
        if needle not in body:
            errs.append(f"{CLASSIFIER} lacks {needle!r}")
    if set(jobs) != set(JOB_CHANNELS):
        errs.append(f"jobs {sorted(jobs)} != declared {sorted(JOB_CHANNELS)}")
    for name, job in jobs.items():
        if CLASSIFIER not in job["needs"]:
            errs.append(f"{name} does not need {CLASSIFIER}")
        got = admitted_channels(job["if"])
        if got != JOB_CHANNELS.get(name):
            errs.append(f"{name} runs on {got}, declared {JOB_CHANNELS.get(name)}")
    return errs


def release_yml():
    return WORKFLOW.read_text(encoding="utf-8")


def mutate(text, old, new):
    assert text.count(old) == 1, f"mutation anchor not unique: {old!r}"
    return text.replace(old, new)


def test_release_workflow_gates_every_job_on_its_channels():
    assert check_workflow(release_yml()) == []


def test_every_channel_runs_something_and_names_a_real_channel():
    # A channel no job runs on would be a green run that shipped nothing.
    assert set().union(*JOB_CHANNELS.values()) == set(ts.CHANNELS)


def test_firmware_job_is_ota_only():
    assert JOB_CHANNELS["release"] == {ts.OTA}


# The checker is not vacuous: each way of breaking the wiring is caught.
@pytest.mark.parametrize(
    "old,new",
    [
        # firmware job loses its gate -> it would run (and publish) on a virtual tag
        ("    if: needs.tag-shape.outputs.channel == 'ota'\n", ""),
        # firmware job widened to the virtual channel
        (
            "    if: needs.tag-shape.outputs.channel == 'ota'\n",
            "    if: needs.tag-shape.outputs.channel == 'ota' || needs.tag-shape.outputs.channel == 'virtual'\n",
        ),
        # image job narrowed to OTA -> a virtual tag would build nothing
        (
            "    if: needs.tag-shape.outputs.channel == 'ota' || needs.tag-shape.outputs.channel == 'virtual'\n",
            "    if: needs.tag-shape.outputs.channel == 'ota'\n",
        ),
        # an unanalyzable gate
        (
            "    if: needs.tag-shape.outputs.channel == 'ota'\n",
            "    if: startsWith(github.ref_name, 'v')\n",
        ),
        # a job no longer waits for the gate
        ("  nimbusd-image:\n    needs: tag-shape\n", "  nimbusd-image:\n"),
        # the classifier stops running the script
        ("python3 tools/release_gate/tag_shape.py", "true"),
        # the classifier stops exporting the channel
        ("channel: ${{ steps.shape.outputs.channel }}", "channel: ota"),
        # a new, ungated job
        ("  release:\n", "  webflash-extra:\n    runs-on: ubuntu-latest\n  release:\n"),
    ],
)
def test_wiring_check_catches_a_broken_gate(old, new):
    assert check_workflow(mutate(release_yml(), old, new)) != []
