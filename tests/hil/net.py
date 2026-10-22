"""Network helper (the HIL test spec).

Provisions WiFi creds over serial, waits for the device to get a LAN IP, and drives
HTTP GET/POST against that IP (matching the AsyncWebServer routes in
``src/net/webui.cpp``: ``/``, ``/api/state``, ``/api/config``, ``/savewifi``, ``/scan``).

Nothing here contacts the network at import time - the helper is only exercised from
inside ``net``-marked tests, which are hardware-gated.
"""

from __future__ import annotations

import os
import time
from typing import List, Mapping, NamedTuple, Optional, Sequence, Tuple

try:
    import requests
except ImportError:  # pragma: no cover - collection must still succeed
    requests = None  # net tests raise a clear error before use

from device import STATUS_PATTERN, Device, ExpectTimeout
from redact import Secret

IP_RE = r"(\d{1,3}\.\d{1,3}\.\d{1,3}\.\d{1,3})"
AP_IP = "192.168.4.1"  # NIMBUS_AP_SSID "Nimbus-setup" default softAP address

# How long ensure_station waits for the station to rejoin on its own after a restart
# before it decides whether provisioning is needed. The bench board rejoins in 5-7 s.
REJOIN_WAIT_S = 20.0
# How long the `net` fixture waits for NIMBUS_TEST_IP to answer after the session
# reset (boot, SD scan, Wi-Fi rejoin). Best effort: a test still fails loudly on its
# own if the IP never answers.
IP_READY_WAIT_S = 60.0
STATUS_REPLY_S = 3.0  # one STATUS round trip; longer means the console is busy booting
# A console still busy after a restart (an SD card's episodic scan blocks STATUS for
# tens of seconds) gets this long to answer before the rejoin bound starts counting.
CONSOLE_BUSY_CAP_S = 60.0
JOIN_TIMEOUT_S = 25.0  # WIFI_GOT_IP after a provisioning command

# Wi-Fi disconnect reasons by class, mirrored from classifyReason() in
# lib/core/src/wifi_policy.cpp (a host test pins the two tables together).
AUTH_REASONS = frozenset({2, 15, 202, 204, 205})  # credentials rejected: retrying cannot fix it
NOT_FOUND_REASONS = frozenset({201})  # NO_AP_FOUND: the stored network is not in range
SELF_REASONS = frozenset({8})  # ASSOC_LEAVE: the device's own disconnect, never a failure

_GOT_IP = rf"WIFI_GOT_IP\s+(?P<got>{IP_RE})"
_DISCONNECTED = r"WIFI_DISCONNECTED\s+reason=(?P<reason>\d+)"
_STATION_EVENT = rf"{_GOT_IP}|{_DISCONNECTED}|{STATUS_PATTERN}"


def classify_reason(reason: int) -> str:
    """The firmware's class for a disconnect reason: ``auth``, ``not-found``,
    ``self`` or ``transient``."""
    if reason in AUTH_REASONS:
        return "auth"
    if reason in NOT_FOUND_REASONS:
        return "not-found"
    if reason in SELF_REASONS:
        return "self"
    return "transient"


def needs_provisioning(known: Optional[int], reasons: Sequence[int]) -> bool:
    """Whether the harness may push Wi-Fi credentials to a station that did not
    rejoin: only when the device reports no stored network (``known == 0``) or its
    stored credentials were rejected (an ``auth`` reason during the wait). A
    transient, self-initiated or not-found disconnect, or an unknown count, never
    does: provisioning makes the device drop the join it is making."""
    if known == 0:
        return True
    return any(classify_reason(r) == "auth" for r in reasons)


class NetError(RuntimeError):
    pass


class WifiAuthFailure(NetError):
    """STA failed to associate; carries the disconnect reason code (F9)."""

    def __init__(self, reason: int):
        self.reason = reason
        super().__init__(f"WiFi disconnected reason={reason}")


class StationDown(NetError):
    """The station did not rejoin and nothing says provisioning would help."""


class Join(NamedTuple):
    """What ensure_station / wait_station saw: the LAN IP (None if the station is
    still down), whether the harness provisioned, the disconnect reasons, and the
    mode the last STATUS reported (None if the console never answered)."""

    ip: Optional[str]
    provisioned: bool
    reasons: Tuple[int, ...]
    mode: Optional[int] = None


def _station_ip(m) -> Optional[str]:
    """The LAN IP a STATUS / WIFI_GOT_IP match proves, else None."""
    if m is None:
        return None
    if m.group("got"):
        return m.group("got")
    up = m.group("wifi") == "1" and m.group("ip") != "0.0.0.0"
    return m.group("ip") if up else None


class Net:
    def __init__(self, device: Device, secrets=None):
        self.device = device
        self.secrets = secrets
        self.ip: Optional[str] = None  # cached LAN IP once GOT_IP arrives
        self._token: Optional[str] = None  # per-device web auth token (see _auth)

    def __repr__(self) -> str:
        return f"<Net ip={self.ip}>"

    @classmethod
    def from_env(cls, device: Device, secrets=None, environ: Optional[Mapping[str, str]] = None) -> "Net":
        """A Net seeded with ``NIMBUS_TEST_IP`` when it is set, so a plain ``net``
        leg runs standalone. Without it ``ip`` stays None until wait_got_ip(),
        ensure_station() or the test sets it."""
        net = cls(device, secrets)
        env = os.environ if environ is None else environ
        net.ip = env.get("NIMBUS_TEST_IP", "").strip() or None
        return net

    def _require_requests(self):
        if requests is None:
            raise NetError("requests not installed; `pip install requests`")
        return requests

    # -- web auth token -------------------------------------------------------
    # Since owner-batch-2 ALL /api GETs are token-gated (401 without it), so the
    # harness attaches X-Nimbus-Token to every request. Sources, in order:
    # env NIMBUS_TEST_TOKEN (no serial churn - pairs with NIMBUS_TEST_IP), else
    # the serial `WEBTOK?` console command. Cached for the session, as a Secret.
    def token(self) -> Optional[str]:
        if self._token:
            return self._token
        env = os.environ.get("NIMBUS_TEST_TOKEN", "").strip()
        if env:
            self._token = Secret(env)
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
        self.device.send(Secret(f"CONNECT {ssid}|{password}"))

    def _next_reason(self, pattern: str, deadline: float):
        """The next line matching ``pattern`` that is not the device's own
        disconnect (reason 8). Provisioning over serial disconnects first
        (``saveAndConnect``), so that line precedes every join and is never the
        answer. Raises ExpectTimeout at ``deadline`` (monotonic)."""
        while True:
            m = self.device.expect_re(pattern, timeout=max(0.0, deadline - time.monotonic()))
            reason = m.group("reason")
            if reason is None or classify_reason(int(reason)) != "self":
                return m

    def wait_got_ip(self, timeout: float = JOIN_TIMEOUT_S) -> str:
        """Wait for ``WIFI_GOT_IP <ip>``; on success cache + return the LAN IP. A
        ``WIFI_DISCONNECTED reason=<n>`` on the way is not the verdict: re-joining an
        access point right after a provisioning command often draws one refusal
        (reason 2 or 203 seen on the bench) before the IP arrives a second later.
        Only when no IP arrives by ``timeout`` does the last reason become the
        failure, WifiAuthFailure(reason) (feeds F9); with no reason at all it is a
        timeout. The device's own disconnect (reason 8) never counts."""
        deadline = time.monotonic() + timeout
        last = None
        while True:
            try:
                m = self._next_reason(rf"{_GOT_IP}|{_DISCONNECTED}", deadline)
            except ExpectTimeout:
                if last is None:
                    raise
                raise WifiAuthFailure(last) from None
            if m.group("got"):
                self.ip = m.group("got")
                return self.ip
            last = int(m.group("reason"))

    def wait_disconnect_reason(self, timeout: float = 25.0) -> int:
        """Wait for ``WIFI_DISCONNECTED reason=<n>`` and return the int reason
        (15/2/201/...), past the device's own disconnect (reason 8) that a
        provisioning command prints first. Asserts the device did NOT hang by
        confirming a subsequent PING->PONG still answers (F9)."""
        m = self._next_reason(_DISCONNECTED, time.monotonic() + timeout)
        reason = int(m.group("reason"))
        if not self.device.ping():
            raise NetError(f"device stopped responding after reason={reason} (hung - the exact F9 failure)")
        return reason

    # -- joining without racing the device's own rejoin ------------------------
    def _status_poll(self, window: float, reasons: List[int]):
        """Send one STATUS and read until its reply or a ``WIFI_GOT_IP`` line (at
        most ``window`` s), keeping every disconnect reason on the way. Returns that
        match, or None when the console did not answer (busy booting)."""
        self.device.send("STATUS")
        end = time.monotonic() + window
        while True:
            try:
                m = self.device.expect_re(_STATION_EVENT, timeout=max(0.0, end - time.monotonic()))
            except ExpectTimeout:
                return None
            if not m.group("reason"):
                return m
            reasons.append(int(m.group("reason")))

    def wait_station(self, timeout: float = REJOIN_WAIT_S, every: float = 1.0) -> Join:
        """Poll STATUS until the station is up (``wifi=1``, or a ``WIFI_GOT_IP``
        line), at most ``timeout`` s counted from the console's first answer (a
        console still busy booting gets up to CONSOLE_BUSY_CAP_S to answer). Stops
        at once in Notifier mode, which keeps the radio off. Never provisions.
        Caches the IP when up."""
        reasons: List[int] = []
        mode: Optional[int] = None
        deadline = time.monotonic() + CONSOLE_BUSY_CAP_S
        while True:
            m = self._status_poll(min(STATUS_REPLY_S, max(0.1, deadline - time.monotonic())), reasons)
            ip = _station_ip(m)
            if ip:
                self.ip = ip
                return Join(ip, False, tuple(reasons), mode)
            if m is not None and m.group("mode") is not None:
                if mode is None:  # the console answered: the rejoin bound starts now
                    deadline = time.monotonic() + timeout
                mode = int(m.group("mode"))
            left = deadline - time.monotonic()
            if mode == 0 or left <= 0:
                return Join(None, False, tuple(reasons), mode)
            time.sleep(min(every, left))

    def ensure_station(self, ssid: str, password: str, wait: float = REJOIN_WAIT_S) -> Join:
        """Get the station on the LAN without fighting the device's own rejoin.

        After a restart the board rejoins from its stored networks within seconds.
        A fixture that read ``wifi=0`` in that window and provisioned made the
        device drop the join it was making, and setup failed (bench, 19 Oct 2026).
        So this waits up to ``wait`` s for the rejoin and provisions only when
        needs_provisioning() says the device cannot rejoin by itself; otherwise it
        raises StationDown with what it saw. Returns a Join; caches the IP."""
        seen = self.wait_station(wait)
        if seen.ip:
            return seen
        if seen.mode is None:
            raise StationDown(
                f"the console did not answer STATUS within {CONSOLE_BUSY_CAP_S:.0f} s after the restart; "
                "not provisioning a device whose state is unknown"
            )
        if seen.mode == 0:
            raise StationDown("the device is in Notifier mode, which keeps Wi-Fi off; call ensure_mode(1) first")
        known = self.device.wifi_known_count()
        if not needs_provisioning(known, seen.reasons):
            stored = "unknown" if known is None else known
            raise StationDown(
                f"Wi-Fi did not rejoin within {wait:.0f} s and nothing says provisioning would help "
                f"(stored networks: {stored}, disconnect reasons: {list(seen.reasons) or 'none'}). "
                "Not re-provisioning: check the access point, or provision the device by hand."
            )
        self.provision(ssid, password)
        return Join(self.wait_got_ip(timeout=JOIN_TIMEOUT_S), True, seen.reasons, seen.mode)

    def settle_after_reset(self) -> Join:
        """Best effort, at session start: the ``device`` fixture has just
        restarted the board, so let a station with a stored network rejoin before
        any test reads STATUS (the bench run on 19 Oct 2026 read ``wifi=0`` in that
        window and re-provisioned). Never provisions; a station that stays down is
        left for the test to handle. When the console reports a joined IP that
        differs from a seeded ``NIMBUS_TEST_IP``, the console wins: the seeded
        address would reach some other host."""
        seeded = self.ip
        seen = self.wait_station(REJOIN_WAIT_S)
        if seen.ip is None:
            self.ip = seeded
        elif seeded and seen.ip != seeded:
            print(f"\n[net] the board reports {seen.ip}, not NIMBUS_TEST_IP={seeded}; using {seen.ip}")
        return seen

    # -- HTTP ---------------------------------------------------------------
    def _base(self, ip: Optional[str]) -> str:
        target = ip or self.ip
        if not target:
            raise NetError("no IP known; set NIMBUS_TEST_IP, call wait_got_ip() or ensure_station(), or pass ip=")
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

    def reachable(self, ip: Optional[str] = None, timeout: float = 3.0, auth: bool = True) -> bool:
        """Quick ``GET /`` with a short timeout (ap_up / web_get_root)."""
        try:
            return self.get("/", ip=ip, timeout=timeout, auth=auth).status_code == 200
        except Exception:  # noqa: BLE001 - any transport error == unreachable
            return False

    def wait_reachable(self, timeout: float = IP_READY_WAIT_S, every: float = 2.0) -> bool:
        """Poll ``GET /`` (served without the token) until the known IP answers,
        at most ``timeout`` s. False on the deadline."""
        deadline = time.monotonic() + timeout
        while not self.reachable(auth=False):
            left = deadline - time.monotonic()
            if left <= 0:
                return False
            time.sleep(min(every, left))
        return True
