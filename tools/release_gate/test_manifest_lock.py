"""Runs tools/test_make_manifest.py inside the release-gate sweep (checks.yml and the battery
run `pytest tools/release_gate`), so the Python end of each byte lock with the device (the
signed message, holdHeads from the head qualification table) is enforced, not only the C++
end in test/test_ota_logic and test/test_head_qual."""

import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))

from test_make_manifest import *  # noqa: E402,F401,F403 - pytest collects the imported tests
