"""Tests for the release notes gate (check_release_notes.py, owner decision D5).

Every OTA release carries one plain line in docs/release-notes/<tag>.txt; the
release workflow writes it into the manifest's `notes`, which the device keeps in a
96-byte field and shows on the Software update panel and to the assistant after the
update. The gate refuses a line that is missing or would not arrive intact.
"""

from __future__ import annotations

import pathlib
import re

import check_release_notes as rn
import run_gate

REPO = pathlib.Path(rn.REPO)


def test_every_committed_line_passes():
    files = sorted(rn.NOTES_DIR.glob("v*.txt"))
    assert files, "docs/release-notes holds no release line"
    for f in files:
        line, why = rn.check_line(f.read_bytes())
        assert line, f"{f.name}: {why}"


def test_v5_0_0_has_its_line():
    line, why = rn.notes_for("v5.0.0")
    assert line and len(line.encode()) <= rn.MAX_BYTES, why


def test_the_rules():
    ok, why = rn.check_line(b"x" * 95 + b"\n")
    assert ok and not why  # 95 bytes, one trailing newline: fine
    for raw, needle in (
        (b"x" * 96, "96 bytes"),
        (b"", "empty"),
        (b"   \n", "empty"),
        (b"one\ntwo\n", "single line"),
        (b"one\r\n", "single line"),
        ("café au lait".encode(), "ASCII"),
        (("a " + chr(0x2014) + " b").encode(), "ASCII"),
        (b"tab\there", "ASCII"),
        (b" leading space", "spaces"),
        (b"Fixes CUM-412 and more", "ticket"),
    ):
        line, why = rn.check_line(raw)
        assert line is None and needle in why, (raw, why)


def test_a_release_candidate_falls_back_to_its_version(tmp_path, monkeypatch):
    monkeypatch.setattr(rn, "NOTES_DIR", tmp_path)
    monkeypatch.setattr(rn, "REPO", tmp_path)
    (tmp_path / "v5.1.0.txt").write_text("A line.\n")
    assert rn.notes_for("v5.1.0-rc2") == ("A line.", "")
    (tmp_path / "v5.1.0-rc2.txt").write_text("The rc's own line.\n")
    assert rn.notes_for("v5.1.0-rc2") == ("The rc's own line.", "")
    line, why = rn.notes_for("v5.2.0")
    assert line is None and "v5.2.0.txt" in why


def test_cli_print_writes_the_line_alone(capsys):
    assert rn.main(["--tag", "v5.0.0", "--print"]) == 0
    out = capsys.readouterr().out
    assert out == rn.notes_for("v5.0.0")[0] + "\n"
    assert rn.main(["--tag", "v0.0.1"]) == 1


def _job(text, name):
    """The body of one top-level job of release.yml (up to the next job)."""
    lines = text.splitlines()
    start = next(i for i, ln in enumerate(lines) if ln == f"  {name}:")
    end = next((i for i in range(start + 1, len(lines)) if re.match(r"^  [A-Za-z0-9_-]+:$", lines[i])), len(lines))
    return "\n".join(lines[start:end])


def test_release_workflow_gates_the_line_before_any_build_or_publish():
    """release.yml: the gate runs in tag-shape, the job every build and publish job
    needs (the nimbusd image is pushed by a job that needs only tag-shape, and image
    tags are immutable), unconditionally, and the manifest step passes the line on."""
    text = (REPO / ".github" / "workflows" / "release.yml").read_text()
    shape = _job(text, "tag-shape")
    assert 'check_release_notes.py --tag="${GITHUB_REF_NAME}"' in shape
    assert "continue-on-error" not in shape and "|| true" not in shape
    for job in ("release", "nimbusd-image"):
        body = _job(text, job)
        assert "needs: tag-shape" in body, job
        assert "check_release_notes.py --tag=" not in body, f"{job} must not be where the gate runs"
    sign = text[text.find("Sign + build typed OTA manifest") :]
    assert re.search(
        r'NOTES="\$\(python3 tools/release_gate/check_release_notes\.py --tag "\$\{GITHUB_REF_NAME\}" --print\)"', sign
    )
    assert '--notes="${NOTES}"' in sign  # = form: a line starting with '-' is never an option
    assert sign.find("NOTES=") < sign.find("ota_priv.pem"), "read the line before the key hits the disk"


def test_the_policy_follows_the_tag_channels(tmp_path, monkeypatch, capsys):
    """Final OTA tags need a line; a release candidate may go without one (a line it
    has is still checked); a Virtual Nimbus image tag needs none."""
    monkeypatch.setattr(rn, "NOTES_DIR", tmp_path)
    monkeypatch.setattr(rn, "REPO", tmp_path)
    assert rn.policy("v5.1.0") == rn.REQUIRED
    assert rn.policy("v5.1.0-rc2") == rn.OPTIONAL
    assert rn.policy("vn-v5.1.0-3") == rn.NONE
    assert rn.main(["--tag", "v5.1.0"]) == 1
    capsys.readouterr()
    assert rn.main(["--tag", "vn-v5.1.0", "--print"]) == 0
    assert capsys.readouterr().out == "\n"  # no line: --notes= gets an empty value
    assert rn.main(["--tag", "v5.1.0-rc1", "--print"]) == 0
    assert capsys.readouterr().out == "\n"
    (tmp_path / "v5.1.0.txt").write_text("x" * 96 + "\n")
    assert rn.main(["--tag", "v5.1.0-rc1"]) == 1  # a line it has is still checked
    (tmp_path / "v5.1.0.txt").write_text("A good line.\n")
    assert rn.main(["--tag", "v5.1.0-rc1"]) == 0 and rn.main(["--tag", "v5.1.0"]) == 0


def test_the_current_version_has_its_line():
    """The per-push guard (checks.yml runs pytest tools/release_gate): a bump of
    include/version.h without docs/release-notes/<version>.txt fails CI before anyone
    can tag it."""
    version = rn.current_version(rn.VERSION_H.read_text())
    assert version, "include/version.h has no NIMBUS_FW_VERSION"
    line, why = rn.line_for(version)
    assert line is not None and (line or rn.policy(version) != rn.REQUIRED), why


def test_run_gate_host_checks_include_the_line():
    src = (REPO / "tools" / "release_gate" / "run_gate.py").read_text()
    assert "check_release_notes.main" in src
    assert hasattr(run_gate, "check_release_notes")


def test_the_95_byte_limit_is_the_device_field():
    """The gate, make_manifest.py and the device must agree on the bound: the device's
    ManifestInfo::notes is char[N], so N-1 bytes survive."""
    import importlib.util

    h = (REPO / "lib" / "core" / "include" / "nimbus" / "ota" / "ota_logic.h").read_text()
    m = re.search(r"char\s+notes\[(\d+)\]", h)
    assert m, "ota_logic.h no longer declares char notes[N]"
    spec = importlib.util.spec_from_file_location("make_manifest", REPO / "tools" / "make_manifest.py")
    mm = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mm)
    assert rn.MAX_BYTES == mm.NOTES_MAX_BYTES == int(m.group(1)) - 1
