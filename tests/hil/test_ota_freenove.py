"""OTA update HIL suite on a Freenove test board (env:test-cyd, OTA variant test-cyd).

The same tests as test_ota.py, run against this board family's image: a schema 2 manifest cannot cross board
families, so the Solide suite cannot prove the Freenove's path. The tests are imported, not copied, and the
``ota_board`` fixture below is the only difference; tests/hil/test_ota_twin_host.py checks no test is missing here.
The dry-run flow leaves the unit exactly as it started.

Run it on a Freenove with: pytest tests/hil/test_ota_freenove.py -m net --allow-hardware --port <port>.
"""

from __future__ import annotations

from pathlib import Path

import pytest

from test_ota import (  # noqa: F401 - pytest collects the imported tests and fixture here
    ota_server,
    test_bitflip_binary_sha_fails,
    test_check_finds_release,
    test_check_result_new_version,
    test_dry_run_verifies_end_to_end,
    test_web_api_check_token_gated,
    test_wrong_key_sig_fails,
)

pytestmark = [pytest.mark.net]

FW_BIN = Path(__file__).resolve().parents[2] / ".pio" / "build" / "test-cyd" / "firmware.bin"


@pytest.fixture
def ota_board(device):
    """This suite's board: the Freenove's image and variant. A Solide skips loudly, it has test_ota.py."""
    status = device.cmd("STATUS", "STATUS ", timeout=5.0)
    if "board=freenove_s3" not in status:
        pytest.skip(f"this suite is for a Freenove board; STATUS says: {status[-40:]}")
    return {"fw_bin": FW_BIN, "variant": "test-cyd", "env": "test-cyd"}
