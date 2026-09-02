#!/usr/bin/env python3
"""THE RELEASE GATE runner (firmware side).

Runs the host-checkable gates and prints the full release battery with the exact
commands a releaser must run and paste green. No firmware release tag is cut until
every section here is green (CUM-174 #6). The hardware and cloud sections cannot
run in CI, so they are printed as an explicit, non-skippable checklist rather than
silently omitted.

Usage:
    python3 tools/release_gate/run_gate.py            # host checks + print the battery
    python3 tools/release_gate/run_gate.py --host-only
Exit 0 = host-checkable gates passed; exit 1 = a host gate failed (release blocked).
"""

from __future__ import annotations

import argparse
import sys

import check_driver_pin
import check_elf_symbols
import check_ota_preserves_nvs
import check_sram_staging

HARDWARE_BATTERY = """\
HARDWARE (bench, --allow-hardware; paste the real output):
  # solide_s3 (Nimbus-4) and freenove_s3 (Freenove CYD), per board family:
  pio run -e esp32s3     -t upload                 # flash the release candidate (solide)
  pio run -e esp32s3-cyd -t upload                 # flash the release candidate (freenove)
  # Render reaches the glass (CUM-167) - INCLUDES a mandatory human glance:
  pytest tests/hil/test_l29_release_gate.py -m hil --allow-hardware   # RenderToGlass, TouchCorrectness, CrashLoop, OTA
  # Device tunnel serves its own page, not a 502 (CUM-173):
  pytest tests/hil/test_l29_release_gate.py -m net --allow-hardware   # TunnelLoopback (CLOUDLOOP)
  # Full HIL battery on each board:
  pytest tests/hil -m "hil and not manual"  --allow-hardware
  pytest tests/hil -m "net and not manual"  --allow-hardware
  # The human-glance + physical-tap steps (recorded):
  pytest tests/hil -m manual --allow-hardware --manual-yes=false
"""

CLOUD_BATTERY = """\
CLOUD (in the cumulo repo; paste the real output):
  pnpm test                                        # unit + integration
  pnpm e2e                                         # relay/tunnel e2e incl. the release-gate legs:
                                                   #   release-gate-tunnel-502, release-gate-reconnect-storm
  cd e2e-browser && npx playwright test            # portal/admin browser e2e
  # Post-deploy, against the live stack (read-only, gated):
  node tools/release_gate/staging_smoke.mjs        # portal login + tunnel probe + admin loads
"""


def run_host_checks() -> bool:
    print("=" * 72)
    print("RELEASE GATE - host checks")
    print("=" * 72)
    results = []
    for name, fn in (
        ("driver-pin (CUM-167)", check_driver_pin.main),
        ("elf-symbols (CUM-167)", check_elf_symbols.main),
        ("sram-staging (CUM-24)", check_sram_staging.main),
        ("ota-nvs (CUM-237)", check_ota_preserves_nvs.main),
    ):
        rc = fn([])
        results.append((name, rc == 0))
        print()
    print("-" * 72)
    for name, ok in results:
        print(f"  {'PASS' if ok else 'FAIL'}  {name}")
    all_ok = all(ok for _, ok in results)
    print("-" * 72)
    return all_ok


def main(argv: "list[str] | None" = None) -> int:
    ap = argparse.ArgumentParser(description="Firmware release gate runner (CUM-174)")
    ap.add_argument("--host-only", action="store_true", help="run only the host checks")
    args = ap.parse_args(argv)

    host_ok = run_host_checks()

    if not args.host_only:
        print()
        print(HARDWARE_BATTERY)
        print(CLOUD_BATTERY)
        print(
            "BLOCK CONDITION: do not cut a release tag or flash clients until every\n"
            "section above is green and pasted into the release record (CUM-174 #6)."
        )

    if not host_ok:
        print("\nRELEASE BLOCKED: a host gate failed (see above).")
        return 1
    print("\nHost gates passed. Complete the hardware + cloud batteries before releasing.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
