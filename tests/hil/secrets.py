"""Runtime secrets reader for the HIL harness (the HIL test spec).

Reads credentials from a local ``.env`` file (repo root by default) at RUNTIME and never commits or
logs their values. Nothing here is imported at module import time that touches the
filesystem - the reader only opens the file when a fixture actually asks for a
secret, so ``pytest --collect-only`` stays clean with no ``.env`` present.

Provider keys (OpenAI/Anthropic) are NOT used host-side: the harness pushes them
onto the DEVICE over serial so the key lands in NVS exactly the way the product
reads it (agent/store.cpp -> solide::memory). Only the values are read here.

Telegram: the harness deliberately does NOT read a production ``TELEGRAM_BOT_TOKEN``.
Using the production bot would collide on Telegram long-poll (getUpdates is single-owner
per token - F18). A DEDICATED test bot token/chat must be supplied via the
environment: ``NIMBUS_TEST_TG_TOKEN`` / ``NIMBUS_TEST_TG_CHAT``.
"""

from __future__ import annotations

import os
from pathlib import Path
from typing import Dict, Optional

# The secrets env file. NIMBUS_ENV_FILE overrides; default is the repo-root .env
# (gitignored, never committed).
DEFAULT_ENV_PATH = Path(os.environ.get("NIMBUS_ENV_FILE") or str(Path(__file__).resolve().parents[2] / ".env"))


class SecretsUnavailable(RuntimeError):
    """Raised when a required secret source is missing. Tests turn this into a
    LOUD pytest.skip with a reason - never a silent pass."""


def _parse_env_file(path: Path) -> Dict[str, str]:
    """Parse a simple ``KEY=VALUE`` .env file. Ignores blank lines and ``#``
    comments, strips surrounding quotes. Never logs values."""
    out: Dict[str, str] = {}
    for raw in path.read_text(encoding="utf-8", errors="replace").splitlines():
        line = raw.strip()
        if not line or line.startswith("#"):
            continue
        if line.startswith("export "):
            line = line[len("export ") :].strip()
        if "=" not in line:
            continue
        key, _, val = line.partition("=")
        key = key.strip()
        val = val.strip().strip('"').strip("'")
        if key:
            out[key] = val
    return out


class Secrets:
    """Lazy, value-hiding view over the secrets ``.env`` plus test-only env vars.

    Construction does NO IO; ``_load()`` is deferred to first access so importing
    this module (and collecting the suite) never requires the file to exist.
    """

    def __init__(self, env_path: Path = DEFAULT_ENV_PATH):
        self._env_path = Path(env_path)
        self._env: Optional[Dict[str, str]] = None

    # -- lazy loader ---------------------------------------------------------
    def _load(self) -> Dict[str, str]:
        if self._env is None:
            if not self._env_path.exists():
                raise SecretsUnavailable(
                    f"secrets file not found: {self._env_path} (set NIMBUS_ENV_FILE or create a .env in the repo root)"
                )
            self._env = _parse_env_file(self._env_path)
        return self._env

    def get(self, key: str, default: str = "") -> str:
        """Raw lookup from the .env (does not fall back to os.environ)."""
        return self._load().get(key, default)

    # -- provider keys (pushed to device, never used host-side) --------------
    @property
    def openai_key(self) -> str:
        return self.get("OPENAI_API_KEY")

    @property
    def anthropic_key(self) -> str:
        return self.get("ANTHROPIC_API_KEY")

    def require_provider_keys(self) -> None:
        """Raise SecretsUnavailable unless at least one provider key is present.
        The agent turn test needs one live provider to round-trip + a second to
        prove failover; callers decide how strict to be."""
        if not (self.openai_key or self.anthropic_key):
            raise SecretsUnavailable(
                "no provider key in the secrets .env (need OPENAI_API_KEY and/or ANTHROPIC_API_KEY for @agent tests)"
            )

    # -- STA WiFi creds (known-good network for sta_connect_ok) --------------
    # These are test-network specifics the operator supplies via env (they are
    # NOT in the secrets .env). Left as env vars so no SSID/PSK is committed.
    @property
    def sta_ssid(self) -> str:
        return os.environ.get("NIMBUS_TEST_STA_SSID", "")

    @property
    def sta_pass(self) -> str:
        return os.environ.get("NIMBUS_TEST_STA_PASS", "")

    def require_sta(self) -> None:
        if not self.sta_ssid:
            raise SecretsUnavailable(
                "set NIMBUS_TEST_STA_SSID / NIMBUS_TEST_STA_PASS (a known-good 2.4 GHz network the device can join)"
            )

    # -- SECOND STA network, for the CUM-207 multi-network failover leg -------
    # A second known-good 2.4 GHz network the device can ALSO join. The two-AP
    # failover test seeds both, drops the first, and asserts automatic migration.
    @property
    def sta_ssid2(self) -> str:
        return os.environ.get("NIMBUS_TEST_STA_SSID2", "")

    @property
    def sta_pass2(self) -> str:
        return os.environ.get("NIMBUS_TEST_STA_PASS2", "")

    def require_sta2(self) -> None:
        self.require_sta()
        if not self.sta_ssid2:
            raise SecretsUnavailable(
                "set NIMBUS_TEST_STA_SSID2 / NIMBUS_TEST_STA_PASS2 (a SECOND 2.4 GHz "
                "network the device can join) for the multi-network failover leg"
            )

    # -- Telegram: DEDICATED test bot ONLY -----------------------------------
    # Read from the environment, NOT from a production bot's token - sharing a
    # production bot's token collides on getUpdates long-poll (F18).
    @property
    def tg_test_token(self) -> str:
        return os.environ.get("NIMBUS_TEST_TG_TOKEN", "")

    @property
    def tg_test_chat(self) -> str:
        return os.environ.get("NIMBUS_TEST_TG_CHAT", "")

    def require_test_telegram(self) -> None:
        if not self.tg_test_token or not self.tg_test_chat:
            raise SecretsUnavailable(
                "set NIMBUS_TEST_TG_TOKEN and NIMBUS_TEST_TG_CHAT (a DEDICATED "
                "test bot + chat - never a production bot's token, which "
                "would collide on long-poll)"
            )
