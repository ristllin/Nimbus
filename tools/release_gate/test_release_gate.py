"""Host tests for the release-gate host checks (so the gate logic is itself tested).

Run: python3 -m pytest tools/release_gate
"""

import check_driver_pin as pin
import check_elf_symbols as elf
import check_ota_preserves_nvs as otanvs


# --- driver-pin gate (CUM-167) ----------------------------------------------
def test_denylisted_pin_fails():
    ok, msg = pin.judge("v0.6.0")
    assert not ok and "CUM-167" in msg


def test_fixed_pin_passes():
    ok, _ = pin.judge("v0.6.1")
    assert ok


def test_missing_pin_fails():
    ok, _ = pin.judge(None)
    assert not ok


def test_find_pin_ignores_commented_lines():
    ini = "\n".join(
        [
            "lib_deps =",
            "    ; https://github.com/ristllin/solide-drivers.git#v0.6.0",
            "    https://github.com/ristllin/solide-drivers.git#v0.6.1",
            "    ; symlink://../solide-drivers",
        ]
    )
    assert pin.find_pin(ini) == "v0.6.1"


def test_find_pin_none_when_only_commented():
    ini = "    ; https://github.com/ristllin/solide-drivers.git#v0.6.0\n"
    assert pin.find_pin(ini) is None


# --- ELF symbol gate (CUM-167 size win) -------------------------------------
def test_epaper_symbol_fails():
    syms = "42007eb0 T _ZN6solide11display_tft5beginEv\n00000000 T _ZN6GxEPD2_BW7_InitEv\n"
    ok, msgs = elf.check(syms)
    assert not ok
    assert any("e-paper" in m for m in msgs)


def test_clean_tft_image_passes():
    syms = "42007eb0 T _ZN6solide11display_tft5beginEv\n42007e7c T _ZN6solide11display_tft12setBacklightEh\n"
    ok, _ = elf.check(syms)
    assert ok


def test_test_build_crash_switch_fails_a_shipped_image():
    syms = "42007eb0 T _ZN6solide11display_tft5beginEv\n42001000 T _ZN6nimbus5crash12armTurnPanicERK6String\n"
    ok, msgs = elf.check(syms)
    assert not ok
    assert any("crash switches" in m for m in msgs)


def test_empty_elf_fails_missing_plumbing():
    ok, msgs = elf.check("00000000 T some_other_symbol\n")
    assert not ok
    assert any("plumbing" in m for m in msgs)


# --- ELF identity: the archived ELF is the image's ELF (crash forensics) -------
def _image_with_sha(sha: bytes, magic: int = 0xABCD5432, first: int = 0xE9) -> bytes:
    import struct

    img = bytearray(0x200)
    img[0] = first
    struct.pack_into("<I", img, 0x20, magic)
    img[0xB0 : 0xB0 + 32] = sha
    return bytes(img)


def test_identity_matching_elf_passes():
    import hashlib

    elf_bytes = b"\x7fELF fake elf body"
    ok, msg, sha = elf.check_identity(elf_bytes, _image_with_sha(hashlib.sha256(elf_bytes).digest()))
    assert ok, msg
    assert sha == hashlib.sha256(elf_bytes).hexdigest()


def test_identity_other_elf_fails():
    import hashlib

    ok, msg, _ = elf.check_identity(b"elf A", _image_with_sha(hashlib.sha256(b"elf B").digest()))
    assert not ok and "does not match" in msg


def test_identity_not_an_app_image_fails():
    import hashlib

    sha = hashlib.sha256(b"x").digest()
    for img in (b"", _image_with_sha(sha, magic=0), _image_with_sha(sha, first=0x00)):
        ok, msg, _ = elf.check_identity(b"x", img)
        assert not ok and "app image" in msg


def _fake_nm(monkeypatch, symbols: str) -> None:
    """Stand in for xtensa nm (the gate-logic CI job has no toolchain)."""
    monkeypatch.setattr(elf, "find_nm", lambda: "xtensa-esp32s3-elf-nm")
    monkeypatch.setattr(elf, "read_symbols", lambda nm, path: symbols)


def test_identity_cli_writes_the_release_record(tmp_path, monkeypatch):
    import hashlib

    _fake_nm(monkeypatch, "42001000 T app_main\n")
    elf_path = tmp_path / "firmware.elf"
    bin_path = tmp_path / "firmware.bin"
    out = tmp_path / "elf-sha256.txt"
    elf_path.write_bytes(b"\x7fELF body")
    bin_path.write_bytes(_image_with_sha(hashlib.sha256(b"\x7fELF body").digest()))
    rc = elf.main(
        [
            "--elf",
            str(elf_path),
            "--bin",
            str(bin_path),
            "--identity-only",
            "--variant",
            "freenove-28",
            "--sha-out",
            str(out),
        ]
    )
    assert rc == 0
    assert out.read_text() == "freenove-28 " + hashlib.sha256(b"\x7fELF body").hexdigest() + "\n"
    bin_path.write_bytes(_image_with_sha(hashlib.sha256(b"other").digest()))
    assert elf.main(["--elf", str(elf_path), "--bin", str(bin_path), "--identity-only"]) == 1


# --- the crash-switch scan runs on every image, --identity-only included -------
# The release loop gates all four images; a run that skipped the scan let three of them
# (the Freenove boards) ship unchecked for the test console's crash switches.
def _matching_pair(tmp_path, elf_bytes: bytes = b"\x7fELF body"):
    import hashlib

    elf_path = tmp_path / "firmware.elf"
    bin_path = tmp_path / "firmware.bin"
    elf_path.write_bytes(elf_bytes)
    bin_path.write_bytes(_image_with_sha(hashlib.sha256(elf_bytes).digest()))
    return str(elf_path), str(bin_path)


def test_identity_only_run_with_arm_turn_panic_fails(tmp_path, monkeypatch, capsys):
    elf_path, bin_path = _matching_pair(tmp_path)
    _fake_nm(monkeypatch, "42001000 T _ZN6nimbus5crash12armTurnPanicERK6String\n42002000 T app_main\n")
    assert elf.main(["--elf", elf_path, "--bin", bin_path, "--identity-only"]) == 1
    out = capsys.readouterr().out
    assert "armTurnPanic" in out and "ELF matches the image" in out and "FAIL" in out


def test_identity_only_refuses_every_crash_switch(tmp_path, monkeypatch):
    elf_path, bin_path = _matching_pair(tmp_path)
    for marker in elf.TEST_ONLY_MARKERS:
        _fake_nm(monkeypatch, f"42001000 T _Z{len(marker)}{marker}v\n42002000 T app_main\n")
        assert elf.main(["--elf", elf_path, "--bin", bin_path, "--identity-only"]) == 1, marker


def test_identity_only_clean_image_passes_without_the_tft_checks(tmp_path, monkeypatch):
    elf_path, bin_path = _matching_pair(tmp_path)
    _fake_nm(monkeypatch, "42002000 T app_main\n")  # no TFT plumbing symbol: not checked here
    assert elf.main(["--elf", elf_path, "--bin", bin_path, "--identity-only"]) == 0


def test_identity_only_fails_closed_without_nm(tmp_path, monkeypatch):
    elf_path, bin_path = _matching_pair(tmp_path)
    monkeypatch.setattr(elf, "find_nm", lambda: None)
    assert elf.main(["--elf", elf_path, "--bin", bin_path, "--identity-only"]) == 1


def test_identity_only_scans_a_real_elf(tmp_path):
    """End to end with the real toolchain: an object file that defines
    nimbus::crash::armTurnPanic fails an identity-only run; the same file without it
    passes. Skipped where no xtensa toolchain is installed (the gate-logic CI job)."""
    import os
    import subprocess

    import pytest

    nm = elf.find_nm()
    gxx = nm[: -len("nm")] + "g++" if nm else ""
    if not nm or not os.path.exists(gxx):
        pytest.skip("no xtensa toolchain (build the firmware once to fetch it)")
    for name, want in (("armTurnPanic", 1), ("armTurnLater", 0)):
        src = tmp_path / f"{name}.cpp"
        obj = tmp_path / f"{name}.o"
        src.write_text(f"namespace nimbus {{ namespace crash {{ void {name}(const char* k) {{ (void)k; }} }} }}\n")
        subprocess.run([gxx, "-c", str(src), "-o", str(obj)], check=True)
        elf_path, bin_path = _matching_pair(tmp_path, obj.read_bytes())
        assert elf.main(["--elf", elf_path, "--bin", bin_path, "--identity-only"]) == want, name


def test_release_workflow_runs_the_full_elf_gate_on_every_shipped_image():
    """release.yml: every env the release builds is ELF-gated, every variant it signs
    is among them, and no gate call passes --identity-only (which skips the e-paper
    and plumbing checks)."""
    import pathlib
    import re

    text = (pathlib.Path(elf.REPO) / ".github" / "workflows" / "release.yml").read_text()
    build = re.search(r"^\s*pio run((?: -e \S+)+)\s*$", text, re.M)
    assert build, "the release build line (pio run -e ...) moved: update this test"
    built = set(re.findall(r"-e (\S+)", build.group(1)))
    loop = re.search(r"for pair in ([^;]+); do\n(?P<body>.*?)\n\s*done", text, re.S)
    assert loop and "check_elf_symbols.py" in loop.group("body"), "the per-variant ELF gate loop moved"
    pairs = dict(pair.split(":") for pair in loop.group(1).split())
    assert set(pairs.values()) == built, f"built {sorted(built)} but ELF-gated {sorted(pairs.values())}"
    signed = set(re.findall(r"^\s*([a-z0-9-]+)=firmware-[a-z0-9-]+\.bin", text, re.M))
    assert signed and signed <= set(pairs), f"signed variants {sorted(signed)} not all ELF-gated"
    commands = re.sub(r"\\\n\s*", " ", text).splitlines()  # one line per shell command
    calls = [ln for ln in commands if "check_elf_symbols.py" in ln and not ln.lstrip().startswith("#")]
    assert calls, "no ELF gate call in release.yml"
    assert not any("--identity-only" in ln for ln in calls), f"an ELF gate call skips checks: {calls}"


def test_identity_only_without_a_bin_is_refused(tmp_path):
    import pytest

    elf_path = tmp_path / "firmware.elf"
    elf_path.write_bytes(b"x")
    with pytest.raises(SystemExit) as e:
        elf.main(["--elf", str(elf_path), "--identity-only"])
    assert e.value.code == 2


# --- OTA-preserves-NVS gate (CUM-237) ---------------------------------------
# A minimal key registry standing in for src/agent/agent_config.h: two OTA
# bookkeeping keys and two user-data keys.
_KEYS_HDR = "\n".join(
    [
        '#define AKEY_OTA_PENDING    "otaPend"',
        '#define AKEY_OTA_TYPE       "otaType"',
        '#define AKEY_TOUCH_CAL      "tchCal"',
        '#define AKEY_WIFI_SSID      "wifiSsid"',
    ]
)


def test_ota_writing_only_bookkeeping_passes():
    src = (
        'static const char* kSimCrashKey = "otaSimCrash";\n'
        "  nvs_set_i32(h, AKEY_OTA_PENDING, 0);\n"
        "  nvs_set_str(h, AKEY_OTA_TYPE, derived);\n"
        "  nvs_set_i32(h, kSimCrashKey, 0);\n"
    )
    ok, msgs = otanvs.audit(src, _KEYS_HDR)
    assert ok, msgs


def test_ota_writing_a_user_key_by_macro_fails():
    # The regression: an update path that clobbers the measured touch calibration.
    src = "  nvs_set_str(h, AKEY_TOUCH_CAL, buf);\n"
    ok, msgs = otanvs.audit(src, _KEYS_HDR)
    assert not ok
    assert any("tchCal" in m and "USER data key" in m for m in msgs)


def test_ota_writing_a_user_key_by_literal_fails():
    # Same regression written as a bare string literal, not the macro.
    src = '  nvs_set_str(h, "wifiSsid", ssid);\n'
    ok, msgs = otanvs.audit(src, _KEYS_HDR)
    assert not ok
    assert any("wifiSsid" in m for m in msgs)


def test_ota_erasing_a_user_key_fails():
    src = "  nvs_erase_key(h, AKEY_WIFI_SSID);\n"
    ok, msgs = otanvs.audit(src, _KEYS_HDR)
    assert not ok
    assert any("wifiSsid" in m for m in msgs)


def test_ota_unresolvable_key_fails_closed():
    # A key the guard cannot resolve to a literal must fail, not be waved through.
    src = "  nvs_set_str(h, someRuntimeKey, v);\n"
    ok, msgs = otanvs.audit(src, _KEYS_HDR)
    assert not ok
    assert any("UNRESOLVABLE" in m for m in msgs)


# A helper that forwards its key parameter to nvs_set_* (the OTA glue reports every
# write through notePersist this way): each call is audited as the write it is.
_WRAPPERS = (
    "static bool rawSetI32(nvs_handle_t h, const char* k, int32_t v) {\n"
    "  return nimbus::sys::notePersist(nvs_set_i32(h, k, v) == ESP_OK, k);\n"
    "}\n"
    "static bool rawSetStr(nvs_handle_t h, const char* k, const char* v) {\n"
    "  return nimbus::sys::notePersist(nvs_set_str(h, k, v) == ESP_OK, k);\n"
    "}\n"
)


def test_ota_wrapper_writing_only_bookkeeping_passes():
    src = _WRAPPERS + "  rawSetI32(h, AKEY_OTA_PENDING, 0);\n  rawSetStr(h, AKEY_OTA_TYPE, t);\n"
    ok, msgs = otanvs.audit(src, _KEYS_HDR)
    assert ok, msgs
    assert '"otaPend"' in msgs[0] and '"otaType"' in msgs[0]


def test_ota_wrapper_writing_a_user_key_fails():
    src = _WRAPPERS + "  rawSetI32(h, AKEY_OTA_PENDING, 0);\n  rawSetStr(h, AKEY_TOUCH_CAL, buf);\n"
    ok, msgs = otanvs.audit(src, _KEYS_HDR)
    assert not ok
    assert any("tchCal" in m and "USER data key" in m for m in msgs)


def test_ota_wrapper_with_an_unresolvable_key_fails_closed():
    src = _WRAPPERS + "  rawSetStr(h, someRuntimeKey, v);\n"
    ok, msgs = otanvs.audit(src, _KEYS_HDR)
    assert not ok
    assert any("UNRESOLVABLE" in m and "someRuntimeKey" in m for m in msgs)


def test_ota_helper_with_its_own_fixed_key_is_still_audited():
    # Only a helper forwarding its OWN key parameter is a wrapper; a fixed key inside a
    # helper is a direct write like any other.
    src = (
        "static bool stash(nvs_handle_t h, const char* v) {\n"
        "  return nvs_set_str(h, AKEY_WIFI_SSID, v) == ESP_OK;\n"
        "}\n"
        "  stash(h, AKEY_OTA_TYPE);\n"
    )
    ok, msgs = otanvs.audit(src, _KEYS_HDR)
    assert not ok
    assert any("wifiSsid" in m for m in msgs)


def test_ota_no_writes_is_a_stale_guard():
    ok, msgs = otanvs.audit("// nothing here\n", _KEYS_HDR)
    assert not ok
    assert any("stale" in m for m in msgs)


def test_ota_real_source_passes():
    # The guard must pass against the actual shipping OTA glue.
    import os

    root = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
    with open(os.path.join(root, otanvs.OTA_SRC_REL), encoding="utf-8") as fh:
        src = fh.read()
    with open(os.path.join(root, otanvs.KEYS_HDR_REL), encoding="utf-8") as fh:
        hdr = fh.read()
    ok, msgs = otanvs.audit(src, hdr)
    assert ok, msgs


# --- storm suite evidence (STORM-E2E) -----------------------------------------
import json as _json  # noqa: E402
import os as _os  # noqa: E402

import check_storm_evidence as storm  # noqa: E402

_ROOT = _os.path.dirname(_os.path.dirname(_os.path.dirname(_os.path.abspath(__file__))))
_CANDIDATE = "eb50beb8c0ffee00"
_COMMIT_TIME = 1_792_000_000.0


def _record(leg, role="candidate", commit=_CANDIDATE, finished=_COMMIT_TIME + 600, grade="PASS"):
    return {
        "schema": storm.SCHEMA,
        "leg": leg,
        "role": role,
        "commit": commit if role == "candidate" else "c1ab939e",
        "finished": finished,
        "grade": grade if role == "candidate" else "RECORDED",
        "verdicts": {"usable": {"status": "PASS" if grade == "PASS" else grade}},
    }


def _full_set(**over):
    out = []
    for leg in storm.LEGS:
        out.append(_record(leg, role="control", finished=_COMMIT_TIME - 86400))
        out.append(_record(leg, **over))
    return out


def _write(tmp_path, records):
    for i, r in enumerate(records):
        (tmp_path / f"{r['leg']}-{r['role']}-{i}.json").write_text(_json.dumps(r))
    return str(tmp_path)


def _gate(tmp_path, records):
    d = _write(tmp_path, records)
    return storm.main(["--evidence", d, "--candidate", _CANDIDATE, "--candidate-time", str(_COMMIT_TIME)])


def test_storm_gate_fails_with_no_evidence(tmp_path):
    assert _gate(tmp_path, []) == 1


def test_storm_gate_fails_without_an_evidence_directory(tmp_path):
    missing = str(tmp_path / "nowhere")
    assert storm.main(["--evidence", missing, "--candidate", _CANDIDATE, "--candidate-time", "1"]) == 1


def test_storm_gate_fails_on_evidence_older_than_the_candidate_commit(tmp_path):
    assert _gate(tmp_path, _full_set(finished=_COMMIT_TIME - 60)) == 1
    verdict = storm.check(_full_set(finished=_COMMIT_TIME - 60), _CANDIDATE, _COMMIT_TIME)
    assert all("older than its commit" in v for v in verdict.values())


def test_storm_gate_fails_with_one_leg_missing(tmp_path):
    records = [r for r in _full_set() if not (r["leg"] == "e_low_heap" and r["role"] == "candidate")]
    assert _gate(tmp_path, records) == 1
    verdict = storm.check(records, _CANDIDATE, _COMMIT_TIME)
    assert [leg for leg, why in verdict.items() if why] == ["e_low_heap"]


def test_storm_gate_fails_on_another_commit_a_failed_leg_or_no_control(tmp_path):
    assert storm.check(_full_set(commit="0123456789ab"), _CANDIDATE, _COMMIT_TIME)["a_settings_full"]
    failed = _full_set()
    failed[3] = _record(storm.LEGS[1], grade="FAIL")
    assert "graded FAIL" in storm.check(failed, _CANDIDATE, _COMMIT_TIME)[storm.LEGS[1]]
    no_control = [r for r in _full_set() if not (r["leg"] == "h_false_claim" and r["role"] == "control")]
    assert "control image" in storm.check(no_control, _CANDIDATE, _COMMIT_TIME)["h_false_claim"]


def test_storm_gate_reads_the_newest_fresh_run(tmp_path):
    records = _full_set()
    records.append(_record("b_page_reserved", grade="FAIL", finished=_COMMIT_TIME + 300))  # older failure
    assert _gate(tmp_path, records) == 0
    records.append(_record("b_page_reserved", grade="FAIL", finished=_COMMIT_TIME + 900))  # newer failure
    assert storm.check(records, _CANDIDATE, _COMMIT_TIME)["b_page_reserved"]


def test_storm_gate_passes_with_fresh_evidence_for_every_leg(tmp_path):
    assert _gate(tmp_path, _full_set()) == 0


def test_storm_gate_commit_names_match_by_prefix():
    assert storm.same_commit("eb50beb8", _CANDIDATE)
    assert not storm.same_commit("eb50beb", "eb50be")  # under 7 digits never matches
    assert not storm.same_commit("eb50beb9", _CANDIDATE)


def test_storm_gate_legs_match_the_suite():
    """The gate's leg list, the suite's grading list and the leg files are one set: a
    new leg with no gate entry, or a gate entry with no leg, fails here."""
    import sys

    sys.path.insert(0, _os.path.join(_ROOT, "tests", "hil", "storm"))
    import grading

    assert tuple(storm.LEGS) == tuple(grading.LEGS)
    files = sorted(
        n[len("test_storm_") : -len(".py")]
        for n in _os.listdir(_os.path.join(_ROOT, "tests", "hil", "storm"))
        if n.startswith("test_storm_") and n.endswith(".py") and n != "test_storm_grading_host.py"
    )
    assert files == sorted(storm.LEGS)


def test_run_gate_storm_step_needs_both_arguments(capsys):
    import run_gate

    assert run_gate.main(["--host-only", "--candidate", _CANDIDATE]) == 1
    assert "pass both --storm-evidence and --candidate" in capsys.readouterr().out
