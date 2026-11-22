"""Runs tools/release/test_publish_candidate.py inside the release-gate sweep (checks.yml's gate-logic
job and the battery run `pytest tools/release_gate`), so the release variant table, the candidate
manifest's refusals and the release.yml wiring check are enforced on every push, not only when
someone runs tools/release by hand. tools/release has no __init__.py: the same pattern as
test_manifest_lock.py."""

import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "release"))

from test_publish_candidate import *  # noqa: E402,F401,F403 - pytest collects the imported tests
