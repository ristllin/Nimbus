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
import pathlib
import re
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


@pytest.mark.parametrize("tag,pre,webflash_on", [("v5.2.0", "false", "true"), ("v5.2.0-rc1", "true", "false")])
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
    with pytest.raises(pc.Refused, match="manifest.json"):
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
    "Candidate - the latest full release and its manifest.json": ["env.REL_MODE == 'candidate'", "check-target"],
    "Candidate - attach manifest-candidate.json to the latest full release": [
        "env.REL_MODE == 'candidate'",
        "gh release upload",
        "--clobber",
        "secrets.RELEASE_PAT",
        "$REL_STABLE_SHA",
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


def workflow_errors(text):
    errs = []
    steps = release_job_steps(text)
    names = [n for n, _ in steps]
    for name, needles in STEP_READS.items():
        body = next((b for n, b in steps if n == name), None)
        if body is None:
            errs.append(f"step {name!r} is gone")
            continue
        errs += [f"{name}: does not read {n!r}" for n in needles if n not in body]
    if errs:
        return errs
    order = [names.index(n) for n in STEP_READS]
    plan_i, target_i, build_i = order[0], order[8], order[1]
    if not plan_i < target_i < build_i:
        errs.append("the plan and the candidate target must come before the build")
    if not names.index("Publish release") < order[9]:
        errs.append("the candidate is attached after its images are published")
    errs += [f"literal {w!r} left in release.yml" for w in literals(text)]
    return errs


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
