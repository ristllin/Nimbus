"""Network helper (the HIL test spec).

Provisions WiFi creds over serial, waits for the device to get a LAN IP, and drives
HTTP GET/POST against that IP (matching the AsyncWebServer routes in
``src/net/webui.cpp``: ``/``, ``/api/state``, ``/api/config``, ``/savewifi``, ``/scan``).

Nothing here contacts the network at import time - the helper is only exercised from
inside ``net``-marked tests, which are hardware-gated.
"""

from __future__ import annotations

import os
from typing import Optional

try:
    import requests
except ImportError:  # pragma: no cover - collection must still succeed
    requests = None  # net tests raise a clear error before use

from device import Device

IP_RE = r"(\d{1,3}\.\d{1,3}\.\d{1,3}\.\d{1,3})"
AP_IP = "192.168.4.1"  # NIMBUS_AP_SSID "Nimbus-setup" default softAP address


class NetError(RuntimeError):
    pass


class WifiAuthFailure(NetError):
    """STA failed to associate; carries the disconnect reason code (F9)."""

    def __init__(self, reason: int):
        self.reason = reason
        super().__init__(f"WiFi disconnected reason={reason}")


class Net:
    def __init__(self, device: Device, secrets=None):
        self.device = device
        self.secrets = secrets
        self.ip: Optional[str] = None  # cached LAN IP once GOT_IP arrives
        self._token: Optional[str] = None  # per-device web auth token (see _auth)

    def _require_requests(self):
        if requests is None:
            raise NetError("requests not installed; `pip install requests`")
        return requests

    # -- web auth token -------------------------------------------------------
    # Since owner-batch-2 ALL /api GETs are token-gated (401 without it), so the
    # harness attaches X-Nimbus-Token to every request. Sources, in order:
    # env NIMBUS_TEST_TOKEN (no serial churn - pairs with NIMBUS_TEST_IP), else
    # the serial `WEBTOK?` console command. Cached for the session.
    def token(self) -> Optional[str]:
        if self._token:
            return self._token
        env = os.environ.get("NIMBUS_TEST_TOKEN", "").strip()
        if env:
            self._token = env
            return self._token
        try:
            self._token = self.device.webtok()
        except Exception:  # noqa: BLE001 - token stays None; requests go bare
            self._token = None
        return self._token

    def _auth(self) -> dict:
        t = self.token()
        return {"X-Nimbus-Token": t} if t else {}

    # -- provisioning over serial -------------------------------------------
    def provision(self, ssid: str, password: str) -> None:
        """Push creds over serial. Uses the NIMBUS_TEST ``WIFI ssid|pass`` command
        (main firmware); the ``provision`` env speaks ``CONNECT ssid|pass`` instead,
        so drive that env via ``provision_legacy``. STA connects async."""
        self.device.wifi(ssid, password)

    def provision_legacy(self, ssid: str, password: str) -> None:
        """The standalone ``provision`` env's protocol (provision.cpp)."""
        self.device.send(f"CONNECT {ssid}|{password}")

    def wait_got_ip(self, timeout: float = 25.0) -> str:
        """Wait for ``WIFI_GOT_IP <ip>``. If a ``WIFI_DISCONNECTED reason=<n>``
        arrives first, raise WifiAuthFailure(reason) (feeds F9). On success caches
        + returns the LAN IP."""
        deadline_pat = (
            rf"WIFI_GOT_IP\s+{IP_RE}"
            r"|WIFI_DISCONNECTED\s+reason=(\d+)"
        )
        m = self.device.expect_re(deadline_pat, timeout=timeout)
        if m.group(1):  # GOT_IP
            self.ip = m.group(1)
            return self.ip
        raise WifiAuthFailure(int(m.group(2)))

    def wait_disconnect_reason(self, timeout: float = 25.0) -> int:
        """Wait for ``WIFI_DISCONNECTED reason=<n>`` and return the int reason
        (15/2/201/...). Asserts the device did NOT hang by confirming a subsequent
        PING->PONG still answers (F9)."""
        m = self.device.expect_re(r"WIFI_DISCONNECTED\s+reason=(\d+)", timeout=timeout)
        reason = int(m.group(1))
        if not self.device.ping():
            raise NetError(f"device stopped responding after reason={reason} (hung - the exact F9 failure)")
        return reason

    # -- HTTP ---------------------------------------------------------------
    def _base(self, ip: Optional[str]) -> str:
        target = ip or self.ip
        if not target:
            raise NetError("no IP known; call wait_got_ip() or pass ip=")
        return f"http://{target}"

    def get(self, path: str, ip: Optional[str] = None, timeout: float = 5.0, auth: bool = True):
        req = self._require_requests()
        return req.get(self._base(ip) + path, timeout=timeout, headers=self._auth() if auth else {})

    def get_json(self, path: str, ip: Optional[str] = None, timeout: float = 5.0):
        return self.get(path, ip=ip, timeout=timeout).json()

    def post(self, path: str, data: dict, ip: Optional[str] = None, timeout: float = 5.0, auth: bool = True):
        """Form-encoded POST (matches the /api/config form parser)."""
        req = self._require_requests()
        return req.post(self._base(ip) + path, data=data, timeout=timeout, headers=self._auth() if auth else {})

    def reachable(self, ip: Optional[str] = None, timeout: float = 3.0) -> bool:
        """Quick ``GET /`` with a short timeout (ap_up / web_get_root)."""
        try:
            return self.get("/", ip=ip, timeout=timeout).status_code == 200
        except Exception:  # noqa: BLE001 - any transport error == unreachable
            return False
