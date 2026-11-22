#!/usr/bin/env python3
"""The release plan from tools/release/variants.json, and the candidate manifest.

.github/workflows/release.yml names no variant itself: its release job runs `plan`, which reads
the table and writes the lists every later step uses (REL_* lines in $GITHUB_ENV). A tag with no
candidate entry gets today's release: every stable row built, gated, signed into manifest.json,
web-flashed (not on -rc) and published. A tag equal to candidate.tag builds only the candidate
variants, runs every gate on them, publishes a pre-release (never latest) without manifest.json
or web-flash, and `make` writes manifest-candidate.json, which the workflow attaches to the
current latest full release beside its manifest.json. A device reads that file only on the
candidate channel (docs/ota.md, "The candidate channel"), and only when it is newer than stable.

This tool only plans and writes local files. It never publishes, never calls the network and
never reads a key except as a path it hands to tools/make_manifest.py.

Subcommands:
    plan --tag T --repository OWNER/REPO --github-env FILE
        validate the table, pick the mode, append the REL_* lines to FILE, print the plan
    check-target --tag T --target DIR --latest-tag L
        DIR holds the latest full release's manifest.json; T is strictly newer than it and is not L
    make --tag T --repository R --key K --target DIR --out manifest-candidate.json
            [--notes N] [--build B] [--images-dir DIR]
        every refusal, then make_manifest.py over the candidate pairs
Exit 0 = done, 1 = refused (a ::error:: line says why).

Refusals: a malformed table; a type that is not a stable row; an env that is not a device env of
platformio.ini, or is one for another board, panel, OTA tag, partition table or a test build than
the type's stable env; an env without a static DRAM row; a run outside ristllin/Nimbus that would
publish to the public releases repository; a candidate not strictly newer than the latest stable
by the device's compareVersions (a suffix-only difference compares equal); a target without a
readable manifest.json; an output not named manifest-candidate.json; an image that is missing or
over the 6,553,600 B app slot.
"""

from __future__ import annotations

import argparse
import dataclasses
import json
import os
import re
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))
TABLE = os.path.join(HERE, "variants.json")
CEILINGS = os.path.join(REPO, "tools", "release_gate", "static_dram_ceilings.json")
MAKE_MANIFEST = os.path.join(REPO, "tools", "make_manifest.py")
sys.path.insert(0, os.path.join(REPO, "tools", "release_gate"))

import check_engine_filter  # noqa: E402  (PioIni: platformio.ini resolved like PlatformIO)

# The public repository field devices download from, and the source repository whose release
# job may publish there. Any other repository must name its own releases_repo in the table.
PUBLIC_RELEASES_REPO = "ristllin/nimbus-fw-releases"
SOURCE_REPO = "ristllin/Nimbus"
STABLE_MANIFEST = "manifest.json"
CANDIDATE_MANIFEST = "manifest-candidate.json"  # kCandidateManifestName (lib/core ota_logic.h)
OTA_SLOT_BYTES = 6_553_600  # the app0/app1 slot of default_16MB.csv, every shipped env's table
HOLD_LINES = ("", "e1", "e2")

SLUG = re.compile(r"[a-z0-9][a-z0-9-]*")
IMAGE = re.compile(r"firmware-[a-z0-9][a-z0-9.-]*\.bin")
REPO_NAME = re.compile(r"[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+")
TAG = re.compile(r"v[0-9]+\.[0-9]+\.[0-9]+(?:-[A-Za-z0-9.-]+)?")
GIT_TAG = re.compile(r"[A-Za-z0-9_][A-Za-z0-9_.-]{0,127}")  # what tag_shape.py admits (an image tag)
ROW_KEYS = {"type", "env", "image", "webflash"}
CAND_ROW_KEYS = {"type", "env"}
CAND_KEYS = {"tag", "variants", "hold_line"}
# A candidate env must build exactly what its type's stable env builds except the engine: every
# resolved platformio.ini option equal, but for these two (and -DNIMBUS_ENGINE in build_flags).
ENGINE_ONLY_OPTIONS = {"extends", "custom_nimbus_engine"}
ENGINE_FLAG = re.compile(r"-DNIMBUS_ENGINE=\S*")


class Refused(Exception):
    pass


# --- the table ---------------------------------------------------------------------------------


def _need(ok: bool, why: str) -> None:
    if not ok:
        raise Refused(why)


def _check_row(row, keys: set, where: str) -> None:
    _need(isinstance(row, dict) and set(row) == keys, f"{where}: keys must be {sorted(keys)}")
    _need(isinstance(row["type"], str) and SLUG.fullmatch(row["type"]), f"{where}: bad type {row['type']!r}")
    _need(isinstance(row["env"], str) and SLUG.fullmatch(row["env"]), f"{where}: bad env {row['env']!r}")


def _unique(values: list, what: str) -> None:
    dupes = sorted({v for v in values if values.count(v) > 1})
    _need(not dupes, f"{what} listed twice: {dupes}")


def _check_stable(rows) -> None:
    _need(isinstance(rows, list) and rows, "variants: no stable row")
    for i, row in enumerate(rows):
        _check_row(row, ROW_KEYS, f"variants[{i}]")
        _need(isinstance(row["image"], str) and IMAGE.fullmatch(row["image"]), f"variants[{i}]: bad image")
        _need(isinstance(row["webflash"], bool), f"variants[{i}]: webflash must be true or false")
    for key in ("type", "env", "image"):
        _unique([r[key] for r in rows], f"stable {key}")


def _check_candidate(cand) -> None:
    _need(isinstance(cand, dict) and set(cand) == CAND_KEYS, f"candidate: keys must be {sorted(CAND_KEYS)}")
    tag, rows = cand["tag"], cand["variants"]
    _need(isinstance(tag, str) and (tag == "" or TAG.fullmatch(tag)), f"candidate: bad tag {tag!r}")
    _need(isinstance(rows, list), "candidate: variants must be a list")
    _need(bool(tag) == bool(rows), "candidate: a tag needs variants and variants need a tag")
    _need(cand["hold_line"] in HOLD_LINES, f"candidate: hold_line must be one of {list(HOLD_LINES)}")
    for i, row in enumerate(rows):
        _check_row(row, CAND_ROW_KEYS, f"candidate.variants[{i}]")
    _unique([r["type"] for r in rows], "candidate type")


def validate_table(data) -> dict:
    """The table with its default filled in; Refused when any key is malformed."""
    _need(isinstance(data, dict), "the table is not a JSON object")
    data = dict(data)
    data.setdefault("releases_repo", PUBLIC_RELEASES_REPO)
    repo = data["releases_repo"]
    _need(isinstance(repo, str) and REPO_NAME.fullmatch(repo), f"releases_repo: bad name {repo!r}")
    _check_stable(data.get("variants"))
    _check_candidate(data.get("candidate"))
    return data


def load_table(path: str = TABLE) -> dict:
    try:
        with open(path, encoding="utf-8") as f:
            data = json.load(f)
    except (OSError, ValueError) as e:
        raise Refused(f"{path}: {e}") from e
    return validate_table(data)


# --- platformio.ini and the DRAM rows --------------------------------------------------------


def build_options(ini, env: str) -> dict:
    """Every option env resolves to (inherited ones included), as token lists, less the engine.

    Two envs with equal results compile the same board, panel, OTA tag, partition table,
    flash and PSRAM settings, sources and libraries; only the engine may differ."""
    section = "env:" + env
    names = set()
    for cur in ini.walk(section):
        names |= set(ini.cp.options(cur))
    out = {}
    for name in sorted(names - ENGINE_ONLY_OPTIONS):
        value = ini.get(section, name) or ""
        if name == "build_flags":
            value = ENGINE_FLAG.sub("", value)
        out[name] = value.split()
    return out


def dram_rows(path: str = CEILINGS) -> dict:
    """{env: row name} over both tables of static_dram_ceilings.json."""
    with open(path, encoding="utf-8") as f:
        data = json.load(f)
    rows = {}
    for table in ("variants", "engine2"):
        for name, row in data.get(table, {}).items():
            _need(row["env"] not in rows, f"static_dram_ceilings.json: env {row['env']} has two rows")
            rows[row["env"]] = name
    return rows


# --- the plan ----------------------------------------------------------------------------------


@dataclasses.dataclass(frozen=True)
class Row:
    type: str
    env: str
    image: str
    dram_row: str
    engine: int
    webflash: bool


@dataclasses.dataclass(frozen=True)
class Plan:
    mode: str  # "stable" or "candidate"
    tag: str
    releases_repo: str
    rows: tuple
    hold_line: str

    @property
    def candidate(self) -> bool:
        return self.mode == "candidate"

    @property
    def manifest_name(self) -> str:
        return CANDIDATE_MANIFEST if self.candidate else STABLE_MANIFEST

    @property
    def url_base(self) -> str:
        return f"https://github.com/{self.releases_repo}/releases/download/{self.tag}"

    @property
    def prerelease(self) -> bool:
        return self.candidate or self.rc  # today: contains(github.ref_name, '-rc')

    @property
    def rc(self) -> bool:
        return "-rc" in self.tag.lower()  # contains() in a workflow expression ignores case

    @property
    def webflash_rows(self) -> tuple:
        return () if self.candidate else tuple(r for r in self.rows if r.webflash)

    def env_vars(self) -> dict:
        assets = [f"Nimbus/{r.image}" for r in self.rows]
        if not self.candidate:
            assets.append(f"Nimbus/{STABLE_MANIFEST}")
        return {
            "REL_MODE": self.mode,
            "REL_RELEASES_REPO": self.releases_repo,
            "REL_ENVS": " ".join(r.env for r in self.rows),
            "REL_COPIES": " ".join(f"{r.env}:{r.image}" for r in self.rows),
            "REL_GATES": " ".join(f"{r.type}:{r.env}:{r.dram_row}:{r.engine}" for r in self.rows),
            "REL_MANIFEST": self.manifest_name,
            "REL_MANIFEST_PAIRS": " ".join(f"{r.type}={r.image}" for r in self.rows),
            "REL_URL_BASE": self.url_base,
            "REL_HOLD_LINE": self.hold_line,
            "REL_WEBFLASH": " ".join(f"{r.type}:{r.env}" for r in self.webflash_rows),
            "REL_WEBFLASH_ON": "true" if self.webflash_rows and not self.rc else "false",
            "REL_PRERELEASE": "true" if self.prerelease else "false",
            "REL_ASSETS": "\n".join(assets),
        }

    def env_text(self) -> str:
        """$GITHUB_ENV lines; a multi-line value uses the heredoc form GitHub documents."""
        out = []
        for name, value in self.env_vars().items():
            out.append(f"{name}<<{name}_EOF\n{value}\n{name}_EOF" if "\n" in value else f"{name}={value}")
        return "\n".join(out) + "\n"


def _guard_repository(releases_repo: str, repository: str) -> None:
    if releases_repo.lower() == PUBLIC_RELEASES_REPO.lower() and repository.lower() != SOURCE_REPO.lower():
        raise Refused(
            f"a run from {repository or 'an unnamed repository'} (a fork or a copy) would publish to "
            f"{PUBLIC_RELEASES_REPO}, which field devices read: set releases_repo in tools/release/variants.json "
            "to the fork's own releases repository"
        )


def _resolve(spec: dict, stable: dict, ini, drams: dict) -> Row:
    """One row of the plan: the stable row of spec's type, built from spec's env."""
    base = stable.get(spec["type"])
    _need(base is not None, f"{spec['type']}: not a stable type of tools/release/variants.json")
    env = spec["env"]
    _need(env in ini.device_envs(), f"{env}: not a device env of platformio.ini")
    if env != base["env"]:
        try:
            same = build_options(ini, env) == build_options(ini, base["env"])
        except check_engine_filter.IniError as e:
            raise Refused(f"{env}: platformio.ini unreadable here: {e}") from e
        _need(
            same,
            f"{spec['type']}: env {env} builds for another board or another build than its stable env "
            f"{base['env']} (every platformio.ini option but the engine must be equal)",
        )
    _need(env in drams, f"{env}: no static DRAM row in tools/release_gate/static_dram_ceilings.json")
    try:
        engine = ini.engine(env)
    except Exception as e:  # noqa: BLE001 - IniError or EngineFilterError, both a refusal
        raise Refused(f"{env}: engine unreadable: {e}") from e
    return Row(spec["type"], env, base["image"], drams[env], engine, base["webflash"])


def make_plan(table: dict, tag: str, repository: str, ini_path: str = "", ceilings: str = CEILINGS) -> Plan:
    """The release this tag makes. Refused when the table cannot be built as written.

    tag "" plans the stable rows (what check_config_mapping.py reads); the CLI needs a real tag."""
    _need(tag == "" or GIT_TAG.fullmatch(tag), f"bad tag {tag!r}")
    _guard_repository(table["releases_repo"], repository)
    ini = check_engine_filter.PioIni(ini_path or os.path.join(REPO, "platformio.ini"))
    drams = dram_rows(ceilings)
    stable = {r["type"]: r for r in table["variants"]}
    cand = table["candidate"]
    is_cand = bool(cand["tag"]) and cand["tag"] == tag
    specs = cand["variants"] if is_cand else table["variants"]
    rows = tuple(_resolve(s, stable, ini, drams) for s in specs)
    return Plan(
        "candidate" if is_cand else "stable",
        tag,
        table["releases_repo"],
        rows,
        cand["hold_line"] if is_cand else "",
    )


# --- versions (nimbus::ota::parseVersion / compareVersions, lib/core/src/ota_logic.cpp) --------


def parse_version(s: str):
    """(major, minor, patch) or None, exactly as the device parses it (a C string: NUL ends it)."""
    s = (s or "").split("\0", 1)[0]
    m = re.fullmatch(r"[vV]?([0-9]+)\.([0-9]+)\.([0-9]+)([-+].*)?", s, re.S)
    if not m:
        return None
    digits = [p.lstrip("0") or "0" for p in m.groups()[:3]]
    if any(len(d) > 6 for d in digits):  # the device stops past 100000; never int() a huge string
        return None
    parts = tuple(int(d) for d in digits)
    return None if any(p > 100000 for p in parts) else parts


def compare_versions(a: str, b: str) -> int:
    pa, pb = parse_version(a), parse_version(b)
    if pa is None or pb is None:
        return 0 if pa is pb else (-1 if pa is None else 1)
    return (pa > pb) - (pa < pb)


# --- the target and the candidate manifest -----------------------------------------------------


def check_target(tag: str, target: str, latest_tag: str) -> dict:
    """The latest full release's manifest.json, Refused unless the candidate is strictly newer."""
    _need(latest_tag != tag, f"the latest full release {latest_tag} is the candidate itself")
    path = os.path.join(target, STABLE_MANIFEST)
    _need(os.path.isfile(path), f"{target}: no manifest.json (the target must be a full release)")
    try:
        with open(path, encoding="utf-8") as f:
            stable = json.load(f)
        version = stable["version"]
    except (OSError, ValueError, KeyError, TypeError) as e:
        raise Refused(f"{path}: unreadable manifest.json: {e}") from e
    _need(parse_version(tag) is not None, f"candidate {tag}: not a version the device can read")
    _need(parse_version(version) is not None, f"stable {version!r}: not a version the device can read")
    _need(
        compare_versions(tag, version) > 0,
        f"candidate {tag} is not newer than stable {version!r} by the device's version order "
        "(suffixes are ignored: number the candidate higher)",
    )
    return stable


@dataclasses.dataclass(frozen=True)
class Signing:
    key: str  # a path; the key itself is never read here
    notes: str = ""
    build: str = ""


def make_manifest_argv(plan: Plan, signing: Signing, out: str) -> list:
    argv = [sys.executable, MAKE_MANIFEST, "--version", plan.tag, f"--notes={signing.notes}"]
    if signing.build:
        argv += ["--build", signing.build]
    argv += ["--key", os.path.abspath(signing.key), "--url-base", plan.url_base, "--out", out]
    if plan.hold_line:
        argv += ["--hold-line", plan.hold_line]
    return argv + [f"{r.type}={r.image}" for r in plan.rows]


def _check_images(plan: Plan, images_dir: str) -> None:
    for r in plan.rows:
        path = os.path.join(images_dir, r.image)
        _need(os.path.isfile(path), f"{r.type}: {path} is not built")
        size = os.path.getsize(path)
        _need(size <= OTA_SLOT_BYTES, f"{r.type}: {r.image} is {size} B, over the {OTA_SLOT_BYTES} B app slot")


def make_candidate(plan: Plan, signing: Signing, target: str, out: str, images_dir: str = ".") -> dict:
    """Write manifest-candidate.json for a candidate plan; the target's manifest.json is only read."""
    _need(plan.candidate, f"{plan.tag}: no candidate entry in tools/release/variants.json")
    name = os.path.basename(out)
    _need(name != STABLE_MANIFEST, f"refusing to write {STABLE_MANIFEST}: a candidate never replaces it")
    _need(name == CANDIDATE_MANIFEST, f"the candidate manifest is named {CANDIDATE_MANIFEST}, not {name}")
    check_target(plan.tag, target, "")
    _check_images(plan, images_dir)
    fd, tmp = tempfile.mkstemp(suffix=".json", dir=os.path.dirname(os.path.abspath(out)))
    os.close(fd)
    try:
        subprocess.run(make_manifest_argv(plan, signing, tmp), check=True, cwd=images_dir)
        with open(tmp, encoding="utf-8") as f:
            man = json.load(f)
        _need(man.get("version") == plan.tag, "make_manifest.py wrote another version")
        _need(list(man.get("variants", {})) == [r.type for r in plan.rows], "make_manifest.py wrote other variants")
        _need(("holdHeads" in man) == bool(plan.hold_line), "holdHeads without a hold_line, or missing")
        os.replace(tmp, out)
    except subprocess.CalledProcessError as e:
        raise Refused(f"make_manifest.py failed (exit {e.returncode})") from e
    finally:
        if os.path.exists(tmp):
            os.unlink(tmp)
    return man


# --- the CLI -----------------------------------------------------------------------------------


def _args(argv):
    ap = argparse.ArgumentParser(description=__doc__.split("\n", 1)[0])
    sub = ap.add_subparsers(dest="cmd", required=True)
    p = sub.add_parser("plan")
    p.add_argument("--github-env", required=True, help="the $GITHUB_ENV file to append to")
    t = sub.add_parser("check-target")
    t.add_argument("--target", required=True)
    t.add_argument("--latest-tag", required=True)
    m = sub.add_parser("make")
    m.add_argument("--key", required=True, help="path to the signing key (never printed)")
    m.add_argument("--target", required=True)
    m.add_argument("--out", required=True)
    m.add_argument("--notes", default="")
    m.add_argument("--build", default="")
    m.add_argument("--images-dir", default=".")
    for s in (p, t, m):
        s.add_argument("--tag", required=True)
    for s in (p, m):
        s.add_argument("--repository", required=True, help="$GITHUB_REPOSITORY")
    return ap.parse_args(argv)


def _print_plan(plan: Plan) -> None:
    print(f"release plan for {plan.tag}: {plan.mode}, releases repository {plan.releases_repo}")
    for r in plan.rows:
        print(f"  {r.type:<12} env {r.env:<18} image {r.image:<26} DRAM row {r.dram_row}, engine {r.engine}")
    print(f"  manifest {plan.manifest_name}, pre-release {plan.prerelease}, web-flash {len(plan.webflash_rows)}")


def _run(a) -> None:
    _need(bool(a.tag), "--tag is empty")
    if a.cmd == "check-target":
        stable = check_target(a.tag, a.target, a.latest_tag)
        print(f"candidate {a.tag} is newer than the latest full release {a.latest_tag} ({stable['version']!r})")
        return
    plan = make_plan(load_table(), a.tag, a.repository)
    if a.cmd == "plan":
        _print_plan(plan)
        with open(a.github_env, "a", encoding="utf-8") as f:
            f.write(plan.env_text())
        return
    man = make_candidate(plan, Signing(a.key, a.notes, a.build), a.target, a.out, a.images_dir)
    print(f"wrote {a.out}: {man['version']}, variants {', '.join(man['variants'])}")


def main(argv=None) -> int:
    a = _args(argv)
    try:
        _run(a)
    except Refused as e:
        print(f"::error::{e}")
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
