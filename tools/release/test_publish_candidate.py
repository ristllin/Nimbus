"""Host tests for the release variant table (variants.json) and publish_candidate.py.

Runnable under pytest from the repo root; tools/release_gate/test_publish_candidate_lock.py runs
them inside the release-gate sweep (checks.yml and the battery run `pytest tools/release_gate`).
Stdlib only (no PyYAML): the workflow is read as text, the way the gates that run inside it do.
Uses openssl for the signed dry run, the same dependency make_manifest.py has.
"""

from __future__ import annotations

import base64
import copy
import hashlib
import importlib.util
import json
import os
import pathlib
import re
import shutil
import subprocess
import sys

import pytest

HERE = pathlib.Path(__file__).resolve().parent
ROOT = HERE.parent.parent
KEY = ROOT / "test" / "ota_test_key.pem"
PUB = ROOT / "test" / "ota_test_key.pub.pem"
WORKFLOW = ROOT / ".github" / "workflows" / "release.yml"


def _load():
    spec = importlib.util.spec_from_file_location("publish_candidate", HERE / "publish_candidate.py")
    mod = importlib.util.module_from_spec(spec)
    sys.modules.setdefault("publish_candidate", mod)
    spec.loader.exec_module(mod)
    return mod


pc = _load()

# --- today's literals (release.yml at rb-main 0363f60de), the stable rows must reproduce them ---
TODAY_TYPES = ["nimbus-tft", "freenove-28", "freenove-35", "freenove-40"]
TODAY_ENVS = ["esp32s3", "esp32s3-cyd", "esp32s3-cyd-35", "esp32s3-cyd-40"]
TODAY_IMAGES = [
    "firmware-nimbus-tft.bin",
    "firmware-freenove.bin",
    "firmware-freenove-35.bin",
    "firmware-freenove-40.bin",
]
TODAY_REPO = "ristllin/nimbus-fw-releases"
SOURCE = "ristllin/Nimbus"

CAND_TAG = "v9.9.0"
CAND = {"tag": CAND_TAG, "variants": [{"type": "freenove-28", "env": "esp32s3-cyd-e2"}], "hold_line": ""}


def table(candidate=None, **top):
    t = copy.deepcopy(pc.load_table())
    if candidate is not None:
        t["candidate"] = copy.deepcopy(candidate)
    t.update(top)
    return pc.validate_table(t)


def plan(tag="v5.2.0", candidate=None, repository=SOURCE, **top):
    return pc.make_plan(table(candidate, **top), tag, repository)


def env_lines(p):
    """The $GITHUB_ENV text as {name: value} (heredoc values joined by newlines)."""
    out, lines = {}, p.env_text().splitlines()
    i = 0
    while i < len(lines):
        name, sep, value = lines[i].partition("=")
        if not sep:
            name, _, delim = lines[i].partition("<<")
            end = lines.index(delim, i + 1)
            out[name] = "\n".join(lines[i + 1 : end])
            i = end + 1
            continue
        out[name] = value
        i += 1
    return out


# --- the stable plan is today's release ---------------------------------------------------------


@pytest.mark.parametrize(
    "tag,pre,webflash_on",
    [("v5.2.0", "false", "true"), ("v5.2.0-rc1", "true", "false"), ("v5.2.0-RC1", "true", "false")],
)
def test_stable_plan_reproduces_todays_literals(tag, pre, webflash_on):
    p = plan(tag)
    e = env_lines(p)
    assert p.mode == "stable" and e["REL_MODE"] == "stable"
    assert [r.type for r in p.rows] == TODAY_TYPES
    assert [r.env for r in p.rows] == TODAY_ENVS
    assert [r.image for r in p.rows] == TODAY_IMAGES
    assert e["REL_ENVS"] == " ".join(TODAY_ENVS)
    assert e["REL_COPIES"] == " ".join(f"{env}:{img}" for env, img in zip(TODAY_ENVS, TODAY_IMAGES))
    # Today's gate loop passes the type as --variant to the ELF and DRAM gates; engine 1 throughout.
    assert e["REL_GATES"] == " ".join(f"{t}:{env}:{t}:1" for t, env in zip(TODAY_TYPES, TODAY_ENVS))
    assert e["REL_MANIFEST"] == "manifest.json"
    assert e["REL_MANIFEST_PAIRS"] == " ".join(f"{t}={img}" for t, img in zip(TODAY_TYPES, TODAY_IMAGES))
    assert e["REL_URL_BASE"] == f"https://github.com/{TODAY_REPO}/releases/download/{tag}"
    assert e["REL_WEBFLASH"] == " ".join(f"{t}:{env}" for t, env in zip(TODAY_TYPES, TODAY_ENVS))
    assert e["REL_WEBFLASH_ON"] == webflash_on
    assert e["REL_PRERELEASE"] == pre  # today: contains(github.ref_name, '-rc')
    assert e["REL_RELEASES_REPO"] == TODAY_REPO
    assert e["REL_HOLD_LINE"] == ""
    assert e["REL_ASSETS"].splitlines() == [f"Nimbus/{img}" for img in TODAY_IMAGES] + ["Nimbus/manifest.json"]


def test_the_shipped_table_has_no_candidate_and_names_todays_repo():
    t = pc.load_table()
    assert t["candidate"] == {"tag": "", "variants": [], "hold_line": ""}
    assert t["releases_repo"] == TODAY_REPO


def test_the_shipped_candidate_entry_plans_when_set():
    """RB-R2's entry is checked on every push, not first on the tag: when candidate.tag is set,
    the real table plans as a candidate against the real platformio.ini and DRAM rows."""
    t = pc.load_table()
    if t["candidate"]["tag"]:
        assert pc.make_plan(t, t["candidate"]["tag"], SOURCE).mode == "candidate"
    p = pc.make_plan(table(CAND), CAND_TAG, SOURCE)  # the same check on an entry like RB-R2's
    assert p.mode == "candidate"


@pytest.mark.parametrize("tag", ["v5.2.0\nREL_MODE=candidate", "v5.2.0 x", "-v5.2.0", "x" * 129])
def test_a_tag_that_could_break_github_env_is_refused(tag):
    plan("v5.2.0-rc1.2_x")  # neighbor: every character an image tag may carry
    with pytest.raises(pc.Refused, match="bad tag"):
        plan(tag)


def test_cli_refuses_an_empty_tag(tmp_path):
    assert pc.main(["plan", "--tag=", f"--repository={SOURCE}", f"--github-env={tmp_path / 'e'}"]) == 1
    assert not (tmp_path / "e").exists()


def test_releases_repo_defaults_to_todays_value():
    t = copy.deepcopy(pc.load_table())
    del t["releases_repo"]
    assert pc.make_plan(pc.validate_table(t), "v5.2.0", SOURCE).releases_repo == TODAY_REPO


def test_candidate_entry_for_another_tag_leaves_the_stable_plan():
    assert env_lines(plan("v5.2.0", candidate=CAND)) == env_lines(plan("v5.2.0"))


# --- the candidate plan ----------------------------------------------------------------------------


def test_candidate_plan_builds_only_the_listed_envs():
    p = plan(CAND_TAG, candidate=CAND)
    e = env_lines(p)
    assert p.mode == "candidate"
    assert e["REL_ENVS"] == "esp32s3-cyd-e2"
    assert e["REL_COPIES"] == "esp32s3-cyd-e2:firmware-freenove.bin"
    assert e["REL_MANIFEST_PAIRS"] == "freenove-28=firmware-freenove.bin"
    assert e["REL_URL_BASE"] == f"https://github.com/{TODAY_REPO}/releases/download/{CAND_TAG}"


def test_candidate_is_a_prerelease_without_manifest_json_or_webflash():
    for tag in (CAND_TAG, CAND_TAG + "-rc1"):
        e = env_lines(plan(tag, candidate=dict(CAND, tag=tag)))
        assert e["REL_PRERELEASE"] == "true"  # a bare vX.Y.Z would otherwise become latest
        assert e["REL_MANIFEST"] == "manifest-candidate.json"
        assert e["REL_ASSETS"].splitlines() == ["Nimbus/firmware-freenove.bin"]
        assert e["REL_WEBFLASH"] == "" and e["REL_WEBFLASH_ON"] == "false"


def test_candidate_e2_gate_takes_the_engine2_dram_row():
    e = env_lines(plan(CAND_TAG, candidate=CAND))
    assert e["REL_GATES"] == "freenove-28:esp32s3-cyd-e2:freenove-28-e2:2"
    e1 = env_lines(plan(CAND_TAG, candidate=dict(CAND, variants=[{"type": "freenove-28", "env": "esp32s3-cyd"}])))
    assert e1["REL_GATES"] == "freenove-28:esp32s3-cyd:freenove-28:1"


def test_hold_heads_only_with_hold_line():
    assert env_lines(plan(CAND_TAG, candidate=CAND))["REL_HOLD_LINE"] == ""
    assert env_lines(plan(CAND_TAG, candidate=dict(CAND, hold_line="e2")))["REL_HOLD_LINE"] == "e2"
    assert pc.make_manifest_argv(plan(CAND_TAG, candidate=CAND), pc.Signing("k"), "o").count("--hold-line") == 0
    argv = pc.make_manifest_argv(plan(CAND_TAG, candidate=dict(CAND, hold_line="e1")), pc.Signing("k"), "o")
    assert argv[argv.index("--hold-line") + 1] == "e1"


# --- table validation: each refusal with its passing neighbor ---------------------------------------


@pytest.mark.parametrize(
    "mutate,why",
    [
        (lambda t: t["variants"].append(dict(t["variants"][0])), "twice"),
        (lambda t: t["variants"][1].update(env="esp32s3"), "twice"),
        (lambda t: t["variants"][1].update(image="firmware-nimbus-tft.bin"), "twice"),
        (lambda t: t["variants"][0].update(type="Nimbus TFT"), "type"),
        (lambda t: t["variants"][0].update(image="nimbus.bin"), "image"),
        (lambda t: t["variants"][0].update(webflash="yes"), "webflash"),
        (lambda t: t["variants"][0].update(extra=1), "keys"),
        (lambda t: t.update(variants=[]), "no stable"),
        (lambda t: t.update(releases_repo="not a repo"), "releases_repo"),
        (lambda t: t["candidate"].update(tag="v9.9.0"), "variants"),
        (lambda t: t["candidate"].update(variants=CAND["variants"]), "tag"),
        (lambda t: t["candidate"].update(tag="v9.9.0", variants=CAND["variants"] * 2), "twice"),
        (lambda t: t["candidate"].update(tag="v9.9.0", variants=CAND["variants"], hold_line="e3"), "hold_line"),
        (lambda t: t["candidate"].update(tag="v9.9.0; rm", variants=CAND["variants"]), "tag"),
    ],
)
def test_a_malformed_table_is_refused(mutate, why):
    t = copy.deepcopy(pc.load_table())
    pc.validate_table(copy.deepcopy(t))  # the neighbor: the shipped table passes
    mutate(t)
    with pytest.raises(pc.Refused, match=why):
        pc.validate_table(t)


def test_a_type_not_in_the_stable_rows_is_refused():
    plan(CAND_TAG, candidate=CAND)  # neighbor
    with pytest.raises(pc.Refused, match="not a stable type"):
        plan(CAND_TAG, candidate=dict(CAND, variants=[{"type": "freenove-50", "env": "esp32s3-cyd-e2"}]))


def test_an_env_not_in_platformio_ini_is_refused():
    plan(CAND_TAG, candidate=CAND)  # neighbor
    with pytest.raises(pc.Refused, match="not a device env of platformio.ini"):
        plan(CAND_TAG, candidate=dict(CAND, variants=[{"type": "freenove-28", "env": "esp32s3-cyd-e3"}]))
    t = copy.deepcopy(pc.load_table())
    t["variants"][0]["env"] = "esp32s3-nope"
    with pytest.raises(pc.Refused, match="not a device env of platformio.ini"):
        pc.make_plan(pc.validate_table(t), "v5.2.0", SOURCE)


@pytest.mark.parametrize("env", ["esp32s3-e2", "esp32s3-cyd-35-e2", "test-cyd-e2", "test-cyd", "esp32s3"])
def test_an_env_of_another_board_is_refused(env):
    # freenove-28 is built by esp32s3-cyd: its twin passes, any other board, panel or test build does not.
    for twin in ("esp32s3-cyd", "esp32s3-cyd-e2"):
        plan(CAND_TAG, candidate=dict(CAND, variants=[{"type": "freenove-28", "env": twin}]))
    with pytest.raises(pc.Refused, match="another board"):
        plan(CAND_TAG, candidate=dict(CAND, variants=[{"type": "freenove-28", "env": env}]))


@pytest.mark.parametrize(
    "extra",
    [
        "board_build.partitions = partitions_test_16MB.csv",
        "board_build.arduino.memory_type = qio_qspi",
        "board_build.flash_mode = dio",
        "lib_deps = somelib",
        "build_src_filter = +<*>",
        "build_unflags =",
        "build_flags = ${env:esp32s3-cyd.build_flags} -DNIMBUS_ENGINE=2 -D BOARD_HAS_PSRAM=0",
    ],
)
def test_any_option_but_the_engine_differing_is_another_build(tmp_path, extra):
    """The class rule: a candidate env equal to its type's stable env in every resolved option but
    the engine passes; one more differing option of any kind is refused."""
    ini = tmp_path / "platformio.ini"
    twin = "[env:esp32s3-cyd-x]\nextends = env:esp32s3-cyd\ncustom_nimbus_engine = 2\n"
    twin += "build_flags = ${env:esp32s3-cyd.build_flags} -DNIMBUS_ENGINE=2\n"
    base_ini = (ROOT / "platformio.ini").read_text()
    ceil = json.loads((ROOT / "tools" / "release_gate" / "static_dram_ceilings.json").read_text())
    ceil["engine2"]["freenove-28-x"] = {"env": "esp32s3-cyd-x", "baseline": 1, "ceiling": 2}
    (tmp_path / "c.json").write_text(json.dumps(ceil))
    cand = dict(CAND, variants=[{"type": "freenove-28", "env": "esp32s3-cyd-x"}])
    ini.write_text(base_ini + "\n" + twin)
    p = pc.make_plan(table(cand), CAND_TAG, SOURCE, ini_path=str(ini), ceilings=str(tmp_path / "c.json"))
    assert p.rows[0].engine == 2  # the neighbor: only the engine differs
    ini.write_text(base_ini + "\n" + twin + extra + "\n")
    with pytest.raises(pc.Refused, match="another board"):
        pc.make_plan(table(cand), CAND_TAG, SOURCE, ini_path=str(ini), ceilings=str(tmp_path / "c.json"))


def test_an_env_without_a_dram_row_is_refused(tmp_path):
    ceil = json.loads((ROOT / "tools" / "release_gate" / "static_dram_ceilings.json").read_text())
    del ceil["engine2"]["freenove-28-e2"]
    path = tmp_path / "c.json"
    path.write_text(json.dumps(ceil))
    pc.make_plan(table(CAND), CAND_TAG, SOURCE)  # neighbor
    with pytest.raises(pc.Refused, match="no static DRAM row"):
        pc.make_plan(table(CAND), CAND_TAG, SOURCE, ceilings=str(path))


# --- the fork guard: the releases repository is read from one place -------------------------------


def test_a_fork_run_that_would_publish_to_the_public_repo_is_refused():
    assert plan("v5.2.0", repository=SOURCE).releases_repo == TODAY_REPO  # the source repo passes
    assert plan("v5.2.0", repository="RISTLLIN/nimbus").releases_repo == TODAY_REPO  # names are case-blind
    with pytest.raises(pc.Refused, match="fork"):
        plan("v5.2.0", repository="someone/Nimbus")
    with pytest.raises(pc.Refused, match="fork"):
        plan("v5.2.0", repository="")
    # The neighbor: a fork whose table names its own releases repo plans against that repo only.
    p = plan("v5.2.0", repository="someone/Nimbus", releases_repo="someone/fw-releases")
    e = env_lines(p)
    assert e["REL_RELEASES_REPO"] == "someone/fw-releases"
    assert e["REL_URL_BASE"].startswith("https://github.com/someone/fw-releases/")


# --- version order: the device's compareVersions ----------------------------------------------------


@pytest.mark.parametrize(
    "a,b,want",
    [
        ("v5.9.0", "v5.2.0", 1),
        ("v5.2.0", "v5.9.0", -1),
        ("v6.0.0-rc1", "v6.0.0", 0),  # suffixes are ignored
        ("6.0.0", "v6.0.0", 0),
        ("v10.0.0", "v9.99.99", 1),
        ("garbage", "v1.0.0", -1),
        ("v1.0.0", "garbage", 1),
        ("v1.0", "v1.0.0", -1),
        ("v1.0.0x", "v1.0.0", -1),
        ("v100001.0.0", "v1.0.0", -1),
        ("v100000.0.0", "v99999.0.0", 1),
        ("v0005.0.0", "v5.0.0", 0),  # leading zeros read as the number
        ("v1.0.0\0junk", "v1.0.0", 0),  # a C string ends at NUL
        ("v" + "9" * 5000 + ".0.0", "v1.0.0", -1),  # refused, never a ValueError
        ("v1.0.0+build7", "v1.0.0", 0),
        ("V2.0.0", "v1.0.0", 1),
    ],
)
def test_compare_versions_mirrors_the_device(a, b, want):
    assert pc.compare_versions(a, b) == want


# --- the target and the candidate manifest: refusals with their neighbors -------------------------


def write_stable(d, version="v5.2.0"):
    d.mkdir(parents=True, exist_ok=True)
    (d / "manifest.json").write_text(json.dumps({"schema": 2, "version": version, "variants": {}}) + "\n")
    return d


@pytest.mark.parametrize("stable", ["v9.9.0", "v9.9.0-rc3", "v10.0.0", "v9.9.1"])
def test_a_candidate_not_strictly_newer_than_stable_is_refused(tmp_path, stable):
    pc.check_target(CAND_TAG, write_stable(tmp_path / "ok", "v9.8.9"), "v9.8.9")  # neighbor
    with pytest.raises(pc.Refused, match="not newer"):
        pc.check_target(CAND_TAG, write_stable(tmp_path / "t", stable), "v-latest")


def test_a_target_without_manifest_json_is_refused(tmp_path):
    pc.check_target(CAND_TAG, write_stable(tmp_path / "ok"), "v5.2.0")  # neighbor
    (tmp_path / "t").mkdir()
    with pytest.raises(pc.Refused, match="no manifest.json"):
        pc.check_target(CAND_TAG, tmp_path / "t", "v5.2.0")
    bad = tmp_path / "bad"
    bad.mkdir()
    (bad / "manifest.json").write_text("{not json")
    with pytest.raises(pc.Refused, match="manifest.json"):
        pc.check_target(CAND_TAG, bad, "v5.2.0")


def test_the_candidate_itself_as_latest_is_refused(tmp_path):
    with pytest.raises(pc.Refused, match="is the candidate"):
        pc.check_target(CAND_TAG, write_stable(tmp_path), CAND_TAG)


def images(d, size=1024 * 1024, names=("firmware-freenove.bin",)):
    d.mkdir(parents=True, exist_ok=True)
    for n in names:
        with open(d / n, "wb") as f:
            f.truncate(size)
    return d


def make(tmp_path, out_name="manifest-candidate.json", candidate=CAND, size=1024 * 1024, tag=CAND_TAG):
    target = write_stable(tmp_path / "latest")
    imgs = images(tmp_path / "imgs", size)
    p = plan(tag, candidate=candidate)
    out = target / out_name
    return pc.make_candidate(p, pc.Signing(str(KEY)), str(target), str(out), str(imgs)), target, out


def test_output_named_manifest_json_is_refused(tmp_path):
    with pytest.raises(pc.Refused, match="never replaces"):
        make(tmp_path / "a", out_name="manifest.json")
    with pytest.raises(pc.Refused, match="manifest-candidate.json"):
        make(tmp_path / "b", out_name="candidate.json")
    make(tmp_path / "c")  # neighbor


def test_an_image_over_the_app_slot_is_refused(tmp_path):
    make(tmp_path / "ok", size=pc.OTA_SLOT_BYTES)  # neighbor: exactly the slot fits
    with pytest.raises(pc.Refused, match="slot"):
        make(tmp_path / "big", size=pc.OTA_SLOT_BYTES + 1)


def test_a_missing_image_is_refused(tmp_path):
    target = write_stable(tmp_path / "latest")
    (tmp_path / "imgs").mkdir()
    with pytest.raises(pc.Refused, match="not built"):
        pc.make_candidate(
            plan(CAND_TAG, candidate=CAND),
            pc.Signing(str(KEY)),
            str(target),
            str(target / "manifest-candidate.json"),
            str(tmp_path / "imgs"),
        )


def test_make_refuses_a_stable_plan(tmp_path):
    target = write_stable(tmp_path / "latest")
    with pytest.raises(pc.Refused, match="no candidate entry"):
        pc.make_candidate(
            plan("v5.2.0"), pc.Signing(str(KEY)), str(target), str(target / "manifest-candidate.json"), str(tmp_path)
        )


def sha(path):
    return hashlib.sha256(pathlib.Path(path).read_bytes()).hexdigest()


def test_local_dry_run_signs_with_the_test_key_and_leaves_manifest_json(tmp_path):
    """The fork run's local stand-in: the latest release as a folder (manifest.json), a dummy image;
    the candidate names one variant, is signed with the test key and verifies; manifest.json is
    byte-identical before and after."""
    target = write_stable(tmp_path / "latest")
    before = sha(target / "manifest.json")
    imgs = images(tmp_path / "imgs")
    out = target / "manifest-candidate.json"
    man = pc.make_candidate(
        plan(CAND_TAG, candidate=CAND), pc.Signing(str(KEY), "", "g1234"), str(target), str(out), str(imgs)
    )
    assert sha(target / "manifest.json") == before
    assert json.loads(out.read_text()) == man
    assert man["version"] == CAND_TAG and man["schema"] == 2 and "holdHeads" not in man
    assert list(man["variants"]) == ["freenove-28"]
    v = man["variants"]["freenove-28"]
    assert v["url"] == f"https://github.com/{TODAY_REPO}/releases/download/{CAND_TAG}/firmware-freenove.bin"
    assert v["sha256"] == sha(imgs / "firmware-freenove.bin")
    msg = tmp_path / "msg.bin"
    msg.write_bytes(f"nimbus-ota-v2\n{CAND_TAG}\nfreenove-28\n{v['sha256']}\n".encode())
    sig = tmp_path / "sig.der"
    sig.write_bytes(base64.b64decode(v["sig"]))
    r = subprocess.run(
        ["openssl", "dgst", "-sha256", "-verify", str(PUB), "-signature", str(sig), str(msg)], capture_output=True
    )
    assert r.returncode == 0, r.stdout + r.stderr
    # Counter: the same signature does not verify for the stable type's neighbor.
    msg.write_bytes(f"nimbus-ota-v2\n{CAND_TAG}\nfreenove-35\n{v['sha256']}\n".encode())
    r = subprocess.run(
        ["openssl", "dgst", "-sha256", "-verify", str(PUB), "-signature", str(sig), str(msg)], capture_output=True
    )
    assert r.returncode != 0


def test_hold_line_rides_the_candidate(tmp_path):
    man, _, _ = make(tmp_path, candidate=dict(CAND, hold_line="e2"))
    assert isinstance(man["holdHeads"], list) and man["holdHeads"]


# --- the CLI writes $GITHUB_ENV and refuses with exit 1 ---------------------------------------------


def test_cli_plan_writes_github_env(tmp_path, capsys):
    env = tmp_path / "env"
    assert pc.main(["plan", "--tag=v5.2.0", f"--repository={SOURCE}", f"--github-env={env}"]) == 0
    assert env.read_text() == plan("v5.2.0").env_text()
    assert "stable" in capsys.readouterr().out
    assert pc.main(["plan", "--tag=v5.2.0", "--repository=someone/Nimbus", f"--github-env={tmp_path / 'e2'}"]) == 1
    assert not (tmp_path / "e2").exists()


def test_env_text_values_are_one_safe_word_each():
    for p in (plan("v5.2.0"), plan("v5.2.0-rc1"), plan(CAND_TAG, candidate=CAND)):
        for name, value in env_lines(p).items():
            assert re.fullmatch(r"REL_[A-Z_]+", name)
            assert re.fullmatch(r"[A-Za-z0-9_.:/=\n -]*", value), (name, value)


# --- release.yml reads every list from the table (stdlib-only parser) -------------------------------

# Each step of the release job and the REL_ variables it must read.
STEP_READS = {
    "Release plan (tools/release/variants.json)": ["publish_candidate.py plan", "${GITHUB_ENV}"],
    "Build firmware (every image of the plan)": ["$REL_ENVS", "$REL_COPIES"],
    "Release gate - every map links one engine": ["check_engine_filter.py --require-maps", "$REL_ENVS"],
    "Release gate - ELF symbols + identity per variant, stage the ELF archive": ["$REL_GATES"],
    "Sign + build typed OTA manifest (schema 2)": ["$REL_MANIFEST_PAIRS", "$REL_URL_BASE", "$REL_MANIFEST"],
    "Build per-variant web-flash images + manifests": ["$REL_WEBFLASH", "${REL_RELEASES_REPO}"],
    "Publish release": ["${{ env.REL_RELEASES_REPO }}", "${{ env.REL_ASSETS }}", "${{ env.REL_PRERELEASE }}"],
    "Publish web-flash latest (webflash branch)": [
        "env.REL_WEBFLASH_ON == 'true'",
        "$REL_WEBFLASH",
        "${REL_RELEASES_REPO}",
    ],
    "Candidate - the latest full release and its manifest.json": [
        "env.REL_MODE == 'candidate'",
        'check-target --tag="${GITHUB_REF_NAME}" --target candidate-target --latest-tag "$latest"',
        'echo "REL_LATEST_TAG=$latest" >> "$GITHUB_ENV"',
        'echo "REL_STABLE_SHA=${sha%% *}" >> "$GITHUB_ENV"',
    ],
    "Candidate - attach manifest-candidate.json to the latest full release": [
        "env.REL_MODE == 'candidate'",
        # The comparisons themselves, each ending the step (the final echo also names the variables).
        'if [ "$latest" != "$REL_LATEST_TAG" ]; then',
        'if [ "${sha%% *}" != "$REL_STABLE_SHA" ]; then',
        "gh release upload",
        "--clobber",
        "secrets.RELEASE_PAT",
    ],
}


def release_job_steps(text):
    """[(name, body)] of the release job's steps, in order."""
    job = re.search(r"^  release:\n(.*?)(?=^  [A-Za-z0-9_-]+:\s*$)", text, re.M | re.S)
    assert job, "release job not found"
    chunks = re.split(r"^      - ", job.group(1), flags=re.M)[1:]
    steps = []
    for c in chunks:
        m = re.search(r"^(?:name: |\s+name: )(.+)$", c, re.M)
        steps.append((m.group(1).strip() if m else "", c))
    return steps


def literals(text):
    """Every type, env, image and releases-repo literal of the table (and today's) in the text."""
    t = pc.load_table()
    words = set(TODAY_TYPES + TODAY_ENVS + TODAY_IMAGES + [TODAY_REPO, "nimbus-fw-releases"])
    words |= {r[k] for r in t["variants"] for k in ("type", "env", "image")}
    found = []
    for w in sorted(words):
        if re.search(rf"(?<![\w.-]){re.escape(w)}(?![\w-])", text):
            found.append(w)
    return found


def read_errors(steps):
    """Each step of STEP_READS exists and reads its variables (comment lines do not count)."""
    bodies = {n: "\n".join(ln for ln in b.splitlines() if not ln.lstrip().startswith("#")) for n, b in steps}
    errs = []
    for name, needles in STEP_READS.items():
        if name not in bodies:
            errs.append(f"step {name!r} is gone")
            continue
        errs += [f"{name}: does not read {n!r}" for n in needles if n not in bodies[name]]
    return errs


def workflow_errors(text):
    steps = release_job_steps(text)
    errs = read_errors(steps)
    if errs:
        return errs
    names = [n for n, _ in steps]
    order = [names.index(n) for n in STEP_READS]
    if not order[0] < order[8] < order[1]:
        errs.append("the plan and the candidate target must come before the build")
    if not names.index("Publish release") < order[9]:
        errs.append("the candidate is attached after its images are published")
    return errs + [f"literal {w!r} left in release.yml" for w in literals(text)]


def test_workflow_reads_every_list_from_the_table():
    assert workflow_errors(WORKFLOW.read_text(encoding="utf-8")) == []


@pytest.mark.parametrize(
    "old,new",
    [
        ('pio run "${pio_args[@]}"', "pio run -e esp32s3"),
        ("${{ env.REL_RELEASES_REPO }}", "ristllin/nimbus-fw-releases"),
        ("${{ env.REL_ASSETS }}", "Nimbus/firmware-freenove.bin"),
        ("${{ env.REL_PRERELEASE }}", "${{ contains(github.ref_name, '-rc') }}"),
        ("env.REL_WEBFLASH_ON == 'true'", "${{ !contains(github.ref_name, '-rc') }}"),
        ('read -r -a gates <<< "$REL_GATES"', "gates=(nimbus-tft:esp32s3:nimbus-tft:1)"),
        ("--clobber", ""),
        ("check-target", "true"),
    ],
)
def test_the_workflow_check_catches_a_literal_or_a_lost_read(old, new):
    text = WORKFLOW.read_text(encoding="utf-8")
    assert text.count(old) >= 1, old
    assert workflow_errors(text.replace(old, new)) != []


# --- the step scripts, executed: every tool stubbed to print its command line -------------------

STUBS = (
    'for c in python3 pio pip cp xz git; do eval "$c() { echo \\"$c \\$*\\"; }"; done\n'
    "cd() { :; }\n"
    'gh() { if [ "$2" = view ]; then echo v5.2.0; else echo "gh $*"; fi; }\n'
    'sha256sum() { echo "5417d1f4  $1"; }\n'
)


def step_script(name):
    body = dict(release_job_steps(WORKFLOW.read_text(encoding="utf-8")))[name]
    m = re.search(r"^        run: \|\n((?:          .*\n|\s*\n)+)", body, re.M)
    return "\n".join(ln[10:] for ln in m.group(1).splitlines())


def exec_step(name, p, tmp_path, tag, stubs=STUBS, extra_env=None):
    """(exit code, command lines) of the named step's run: block under plan p (bash -e, as GitHub)."""
    env = {
        "PATH": os.path.dirname(sys.executable) + ":/usr/bin:/bin",
        "GITHUB_REF_NAME": tag,
        "GITHUB_REPOSITORY": SOURCE,
        "GITHUB_ENV": str(tmp_path / "ge"),
    }
    env.update(
        env_lines(p), OTA_SIGNING_KEY="stub", RELEASE_PAT="stub", REL_LATEST_TAG="v5.2.0", REL_STABLE_SHA="5417d1f4"
    )
    env.update(extra_env or {})
    script = step_script(name).replace("/tmp/ota_priv.pem", str(tmp_path / "k"))
    r = subprocess.run(["bash", "-e", "-c", stubs + script], cwd=tmp_path, env=env, capture_output=True, text=True)
    out = r.stdout.splitlines() + r.stderr.splitlines()
    return r.returncode, [
        ln.replace(str(tmp_path), "<tmp>") for ln in out if not ln.startswith(("pip ", "cat ", "ls "))
    ]


def run_step(name, p, tmp_path, tag, stubs=STUBS):
    rc, lines = exec_step(name, p, tmp_path, tag, stubs)
    assert rc == 0, lines
    return lines


SIGN = "Sign + build typed OTA manifest (schema 2)"
GATE = "Release gate - ELF symbols + identity per variant, stage the ELF archive"
NOTES_CMD = "python3 tools/release_gate/check_release_notes.py --tag {tag} --print"
BUILD_CMD = "git describe --tags --always --match v[0-9]*"


@pytest.mark.skipif(not shutil.which("bash"), reason="needs bash")
def test_the_stable_steps_run_todays_command_lines(tmp_path):
    tag = "v5.2.0"
    p = plan(tag)
    build = run_step("Build firmware (every image of the plan)", p, tmp_path, tag)
    assert build == ["pio run " + " ".join(f"-e {e}" for e in TODAY_ENVS)] + [
        f"cp .pio/build/{e}/firmware.bin {i}" for e, i in zip(TODAY_ENVS, TODAY_IMAGES)
    ]
    gates = [ln for ln in run_step(GATE, p, tmp_path, tag) if ln.startswith("python3")]
    want = []
    for t, e in zip(TODAY_TYPES, TODAY_ENVS):
        elf = f".pio/build/{e}/firmware.elf"
        want += [
            f"python3 tools/release_gate/check_elf_symbols.py --elf {elf} --bin .pio/build/{e}/firmware.bin --variant {t} --sha-out elf-archive/elf-sha256.txt",
            f"python3 tools/release_gate/check_static_dram.py --elf {elf} --variant {t}",
            f"python3 tools/release_gate/check_tg_poll_stack.py --elf {elf} --quiet",
        ]
    assert gates == want
    sign = run_step(SIGN, p, tmp_path, tag)
    pairs = " ".join(f"{t}={i}" for t, i in zip(TODAY_TYPES, TODAY_IMAGES))
    assert sign == [
        f"python3 tools/make_manifest.py --version {tag} --notes={NOTES_CMD.format(tag=tag)} --build {BUILD_CMD} "
        f"--key <tmp>/k --url-base https://github.com/{TODAY_REPO}/releases/download/{tag} --out manifest.json {pairs}"
    ]
    web = run_step("Build per-variant web-flash images + manifests", p, tmp_path, tag)
    raw = f"https://raw.githubusercontent.com/{TODAY_REPO}/webflash/latest"
    assert web == [
        f"python3 tools/release/make_webflash_manifest.py --variant {t} --build-dir .pio/build/{e} --version {tag} "
        f"--out-dir webflash --base-url {raw}/{t}"
        for t, e in zip(TODAY_TYPES, TODAY_ENVS)
    ]
    push = run_step("Publish web-flash latest (webflash branch)", p, tmp_path, tag)
    assert push[-1] == f"git push --force https://x-access-token:stub@github.com/{TODAY_REPO}.git webflash:webflash"
    assert [ln for ln in push if ln.startswith("cp ")] == [
        f"cp ../Nimbus/webflash/{t}/{f} latest/{t}/"
        for t in TODAY_TYPES
        for f in ("nimbus-webflash.bin", "nimbus-webflash.bin.sha256", "manifest.json")
    ]


@pytest.mark.skipif(not shutil.which("bash"), reason="needs bash")
def test_the_candidate_steps_build_gate_sign_and_attach_only_the_candidate(tmp_path):
    p = plan(CAND_TAG, candidate=CAND)
    assert run_step("Build firmware (every image of the plan)", p, tmp_path, CAND_TAG) == [
        "pio run -e esp32s3-cyd-e2",
        "cp .pio/build/esp32s3-cyd-e2/firmware.bin firmware-freenove.bin",
    ]
    elf = ".pio/build/esp32s3-cyd-e2/firmware.elf"
    assert [ln for ln in run_step(GATE, p, tmp_path, CAND_TAG) if ln.startswith("python3")] == [
        f"python3 tools/release_gate/check_elf_symbols.py --elf {elf} --bin .pio/build/esp32s3-cyd-e2/firmware.bin --variant freenove-28 --sha-out elf-archive/elf-sha256.txt",
        f"python3 tools/release_gate/check_elf_symbols.py --engine1-absent --elf {elf}",
        f"python3 tools/release_gate/check_static_dram.py --elf {elf} --variant freenove-28-e2",
        f"python3 tools/release_gate/check_tg_poll_stack.py --elf {elf} --quiet",
    ]
    sign = run_step(SIGN, p, tmp_path, CAND_TAG)
    assert sign == [
        f"python3 tools/release/publish_candidate.py make --tag={CAND_TAG} --repository={SOURCE} --key <tmp>/k "
        f"--notes={NOTES_CMD.format(tag=CAND_TAG)} --build {BUILD_CMD} --target candidate-target --out manifest-candidate.json"
    ]
    attach = run_step("Candidate - attach manifest-candidate.json to the latest full release", p, tmp_path, CAND_TAG)
    assert attach[0] == f"gh release upload v5.2.0 manifest-candidate.json --repo {TODAY_REPO} --clobber"
    assert attach[-1].endswith("its manifest.json is unchanged (sha256 5417d1f4)")


# --- the candidate's two guard steps refuse, and the job stops there ---------------------------

ATTACH = "Candidate - attach manifest-candidate.json to the latest full release"
TARGET = "Candidate - the latest full release and its manifest.json"


def gh_stubs(view="v5.2.0", after_sha="5417d1f4"):
    """STUBS with `gh release view` printing `view` and sha256sum printing `after_sha`."""
    return (
        STUBS
        + f'gh() {{ if [ "$2" = view ]; then echo {view}; else echo "gh $*"; fi; }}\n'
        + f'sha256sum() {{ echo "{after_sha}  $1"; }}\n'
    )


@pytest.mark.skipif(not shutil.which("bash"), reason="needs bash")
def test_the_attach_step_stops_when_the_latest_release_moved(tmp_path):
    p = plan(CAND_TAG, candidate=CAND)
    rc, lines = exec_step(ATTACH, p, tmp_path, CAND_TAG, gh_stubs(view="v5.3.0"))
    assert rc != 0
    assert not any(ln.startswith("gh release upload") for ln in lines), lines  # nothing attached
    assert any("moved from v5.2.0 to v5.3.0" in ln for ln in lines)
    rc, lines = exec_step(ATTACH, p, tmp_path, CAND_TAG, gh_stubs())  # the neighbor
    assert rc == 0 and lines[0].startswith("gh release upload v5.2.0 ")


@pytest.mark.skipif(not shutil.which("bash"), reason="needs bash")
def test_the_attach_step_fails_when_manifest_json_changed(tmp_path):
    p = plan(CAND_TAG, candidate=CAND)
    rc, lines = exec_step(ATTACH, p, tmp_path, CAND_TAG, gh_stubs(after_sha="deadbeef"))
    assert rc != 0
    assert any("manifest.json of v5.2.0 changed" in ln for ln in lines), lines
    assert not any("is unchanged" in ln for ln in lines)


def target_stubs(stable_version):
    """The real python3 (check-target runs for real); gh download writes the latest's manifest.json,
    or nothing when stable_version is empty; every build or upload tool only prints."""
    write = (
        f'printf \'{{"schema": 2, "version": "{stable_version}", "variants": {{}}}}\' > candidate-target/manifest.json; '
        if stable_version
        else ""
    )
    return (
        'for c in pio pip cp xz git; do eval "$c() { echo \\"$c \\$*\\"; }"; done\n'
        'gh() { case "$2" in view) echo v5.2.0 ;; download) ' + write + 'echo "gh $*" ;; *) echo "gh $*" ;; esac; }\n'
        'sha256sum() { echo "5417d1f4  $1"; }\n'
    )


def run_job(steps, p, tmp_path, tag, stubs):
    """Run steps in order as the job does: the first nonzero step ends it. (last rc, all lines)."""
    lines = []
    for name in steps:
        rc, out = exec_step(name, p, tmp_path, tag, stubs)
        lines += out
        if rc != 0:
            return rc, lines
    return 0, lines


@pytest.mark.skipif(not shutil.which("bash"), reason="needs bash")
@pytest.mark.parametrize(
    "stable,ok", [("v5.2.0", True), ("", False), (CAND_TAG, False), (CAND_TAG + "-rc2", False), ("v10.0.0", False)]
)
def test_the_candidate_target_step_stops_the_job_before_any_build_or_upload(tmp_path, stable, ok):
    (tmp_path / "tools").symlink_to(ROOT / "tools")
    p = plan(CAND_TAG, candidate=CAND)
    steps = [TARGET, "Build firmware (every image of the plan)", ATTACH]
    rc, lines = run_job(steps, p, tmp_path, CAND_TAG, target_stubs(stable))
    built = any(ln.startswith("pio run") for ln in lines)
    uploaded = any(ln.startswith("gh release upload") for ln in lines)
    ge = (tmp_path / "ge").read_text() if (tmp_path / "ge").exists() else ""
    if ok:  # the neighbor: a newer candidate over a full release goes on to build and attach
        assert rc == 0 and built and uploaded and "REL_LATEST_TAG=v5.2.0" in ge, lines
    else:
        assert rc != 0 and not built and not uploaded and "REL_LATEST_TAG" not in ge, lines
        assert any(ln.startswith("::error::") for ln in lines), lines
