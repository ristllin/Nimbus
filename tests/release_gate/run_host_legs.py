#!/usr/bin/env python3
"""THE HOST-RUNNABLE RELEASE GATE - one verdict, every lose-every-customer class.

CUM-174. This runs the half of the release gate that needs NO hardware and NO
cloud: the host unit/logic legs that pin each 2026-08-24-class failure so it
cannot regress silently, plus the source-guard gates. It prints one PASS/FAIL
verdict and exits non-zero if any host leg fails, so CI (and a releaser) has a
single "is the host gate green?" command.

It does NOT replace the full battery. The hardware and cloud legs - flashing a
board, pixels-reach-the-glass, the physical-tap check, the tunnel probe, the
provision -> OTA -> power-cycle -> read-NVS proof - are printed by
`tools/release_gate/run_gate.py` and specified leg-by-leg in
`tests/release_gate/MANIFEST.md`. This runner is their always-on host backstop;
the MANIFEST says exactly what each class still owes the bench.

Usage:
    python3 tests/release_gate/run_host_legs.py           # run every host leg
    python3 tests/release_gate/run_host_legs.py --list     # list legs, run nothing
Exit 0 = every host leg passed; exit 1 = a host leg failed (release blocked) or a
prerequisite (the native toolchain / sibling driver) is missing.
"""

from __future__ import annotations

import argparse
import os
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

# Each leg: (failure class, short id, argv, what it pins). argv runs from ROOT.
# The native legs are grouped into ONE `pio test` call (below) so the toolchain
# spins up once; the python legs run directly.
NATIVE_SUITES = [
    (
        "white-screen (CUM-167/231)",
        "test_panel_health",
        "flip-aware MADCTL healthy() compare: catches the partial MY/MX loss the "
        "v0.7.1 mask waved through, no thrash at flip=1",
    ),
    (
        "white-screen (CUM-167/231)",
        "test_panel_heal",
        "the rearm guard: past the trust window the panel is re-armed "
        "UNCONDITIONALLY; healthy() never gates it (the pre-CUM-231 white screen)",
    ),
    (
        "touch breakage (CUM-203/189)",
        "test_touch_cal",
        "boardDefaultCal per TouchKind (resistive Solide swap-only vs capacitive "
        "Freenove swap+invertY); the single-source 180 reconciliation",
    ),
    (
        "crash/boot loop (CUM-174 #4)",
        "test_ota_logic",
        "shouldRollback(pending, bootCount) rolls back after kOtaMaxBootAttempts; "
        "bootHealthy() self-validates - the boot-loop rollback decision",
    ),
    (
        "crash/boot loop (CUM-174 #4)",
        "test_fault",
        "the runtime capability-fault registry the resilience suite drives degraded paths with",
    ),
]

# (class, id, argv, what it pins) - the python host gates.
PYTHON_LEGS = [
    (
        "settings-lost-across-OTA (CUM-237)",
        "check_ota_preserves_nvs",
        [sys.executable, "tools/release_gate/check_ota_preserves_nvs.py"],
        "source guard: the OTA flow writes only AKEY_OTA_* / otaSimCrash bookkeeping "
        "keys - never a user key (Wi-Fi, provider, tchCal, scrModel, theme, ...)",
    ),
    (
        "gate logic (CUM-167/237 #6)",
        "gate_selftest",
        [sys.executable, "-m", "pytest", "tools/release_gate", "-q"],
        "the release-gate checks are themselves tested: each goes RED on its "
        "pre-fix input and GREEN on the fix (retroactive proof)",
    ),
]


def _print_legs() -> None:
    print("HOST-RUNNABLE RELEASE-GATE LEGS")
    print("=" * 72)
    for cls, name, why in NATIVE_SUITES:
        print(f"  [native] {name:<20} {cls}")
        print(f"           {why}")
    for cls, name, _argv, why in PYTHON_LEGS:
        print(f"  [python] {name:<20} {cls}")
        print(f"           {why}")


def _run(argv: "list[str]") -> bool:
    proc = subprocess.run(argv, cwd=ROOT)
    return proc.returncode == 0


def run_all() -> int:
    results: "list[tuple[str, str, bool]]" = []

    # One native invocation for every failure-class unit suite.
    native_filters: "list[str]" = []
    for _cls, name, _why in NATIVE_SUITES:
        native_filters += ["-f", name]
    print("-" * 72)
    print("NATIVE host suites (pio test -e native)")
    print("-" * 72)
    native_ok = _run(["pio", "test", "-e", "native", *native_filters])
    for cls, name, _why in NATIVE_SUITES:
        results.append((cls, name, native_ok))

    for cls, name, argv, _why in PYTHON_LEGS:
        print("-" * 72)
        print(f"PYTHON leg: {name}")
        print("-" * 72)
        results.append((cls, name, _run(argv)))

    print()
    print("=" * 72)
    print("HOST RELEASE-GATE VERDICT")
    print("=" * 72)
    for cls, name, ok in results:
        print(f"  {'PASS' if ok else 'FAIL':<4}  {name:<22} {cls}")
    all_ok = all(ok for _cls, _name, ok in results)
    print("-" * 72)
    if all_ok:
        print(
            "HOST GATE GREEN. The hardware + cloud legs still apply - see "
            "tests/release_gate/MANIFEST.md and tools/release_gate/run_gate.py."
        )
        return 0
    print("HOST GATE FAILED - release blocked. Fix the FAILing leg above.")
    return 1


def main(argv: "list[str] | None" = None) -> int:
    ap = argparse.ArgumentParser(description="Host-runnable release gate (CUM-174)")
    ap.add_argument("--list", action="store_true", help="list legs and exit")
    args = ap.parse_args(argv)
    if args.list:
        _print_legs()
        return 0
    return run_all()


if __name__ == "__main__":
    sys.exit(main())
