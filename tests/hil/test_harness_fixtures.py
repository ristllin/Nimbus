"""Host tests for the HIL harness fixtures (net, bench, secrets, esptool), plus one
bench leg.

The bench run on 19 Oct 2026 (unit A, main 348fc47) lost four legs to the harness,
not the firmware:

* a plain ``net`` leg failed with "no IP known": the ``net`` fixture never read
  ``NIMBUS_TEST_IP``;
* ``bench`` read ``wifi=0`` while the board was still rejoining after the session
  reset, re-provisioned, and failed setup on ``WIFI_DISCONNECTED reason=8`` (the
  device's own disconnect before a join);
* a failing leg's traceback printed the ``bench`` tuple, test bot token included;
* ``esptool_hard_reset`` found no esptool on a bench that has it only inside
  PlatformIO.

Each class is pinned here with scripted fakes (the real Device read path over a
scripted console, a fake clock, a fake HTTP layer); nothing touches a board. The
one ``hil``/``net`` leg at the bottom is the bench proof of the rejoin wait.
"""

from __future__ import annotations

import os
import random
import re
import string
import subprocess
import sys
import textwrap
import uuid
from pathlib import Path
from types import SimpleNamespace

import pytest

import conftest
import device as device_mod
import net as net_mod
from device import Device, ExpectTimeout, esptool_commands
from net import Join, NetError, StationDown, WifiAuthFailure, classify_reason, needs_provisioning
from redact import SHOW_TAIL, Secret, redact, scrub
from secrets import Secrets

HIL_DIR = Path(__file__).resolve().parent
REPO = HIL_DIR.parents[1]
IP = "192.0.2.50"  # TEST-NET-1, never a real host


# ---- fakes ---------------------------------------------------------------------------


class FakeClock:
    """Stands in for the ``time`` module inside net.py: sleep advances the clock."""

    def __init__(self):
        self.now = 1000.0

    def monotonic(self):
        return self.now

    def time(self):
        return self.now

    def sleep(self, seconds):
        self.now += seconds


def status_line(wifi: int, mode: int = 1) -> str:
    ip = IP if wifi else "0.0.0.0"
    return (
        f"STATUS fw=v4.5.13 build=g348fc47 mode={mode} wifi={wifi} ip={ip} rssi=-51 heap=45656 "
        "scr=tft scrok=1 want=tft minheap=30000 psram=8388608 uptime=9 board=solide_s3 clk=0"
    )


class Console:
    """A scripted test console. ``polls`` lists the lines each successive STATUS
    produces (events first, then the reply); the last entry repeats."""

    JOIN = ("WIFI provisioning ssid=bench", "WIFI_DISCONNECTED reason=8", f"WIFI_GOT_IP {IP}")

    def __init__(self, polls, known=1, join=JOIN):
        self.polls = [list(p) for p in polls] or [[status_line(0)]]
        self.known = known
        self.join = list(join)
        self.status_calls = 0
        self.provisions = 0

    def __call__(self, line: str):
        if line == "STATUS":
            lines = self.polls[min(self.status_calls, len(self.polls) - 1)]
            self.status_calls += 1
            return lines
        if line == "WIFIKNOWN?":
            return [] if self.known is None else [f'WIFIKNOWN n={self.known} ["bench"]']
        if line.startswith("WIFI "):
            self.provisions += 1
            return self.join
        if line == "PING":
            return ["PONG"]
        return []


class _Ser:
    def close(self):
        pass


class ScriptedDevice(Device):
    """The real Device request/response code over a scripted console: a sent line
    queues the console's replies; a read pops the queue and never waits."""

    def __init__(self, console):
        super().__init__(port="/dev/cu.fake")
        self._ser = _Ser()
        self.console = console
        self.sent = []
        self.queue = []

    def send(self, line):
        self.sent.append(line)
        self.queue.extend(self.console(str(line)))

    def _readline(self, deadline):
        return self.queue.pop(0) if self.queue else None


class FakeHttp:
    """Stands in for ``requests`` inside net.py. GETs fail with a connection error
    until ``down_for`` of them have been made."""

    def __init__(self, down_for=0):
        self.down_for = down_for
        self.gets = []
        self.posts = []

    def get(self, url, timeout=None, headers=None):
        self.gets.append(url)
        if len(self.gets) <= self.down_for:
            raise ConnectionError("no route to host")
        return SimpleNamespace(status_code=200, json=lambda: {})

    def post(self, url, data=None, timeout=None, headers=None):
        self.posts.append((url, dict(data or {}), dict(headers or {})))
        return SimpleNamespace(status_code=200)


@pytest.fixture
def clock(monkeypatch):
    c = FakeClock()
    monkeypatch.setattr(net_mod, "time", c)
    return c


def scripted(console):
    dev = ScriptedDevice(console)
    return dev, net_mod.Net(dev)


def wifi_lines(dev):
    return [str(s) for s in dev.sent if str(s).startswith("WIFI ")]


# ---- 1. the net fixture seeds NIMBUS_TEST_IP -----------------------------------------


@pytest.fixture
def reach(monkeypatch):
    """Records every Net.wait_reachable call; ``reach.answers`` is what it returns."""
    calls = SimpleNamespace(timeouts=[], answers=True)
    monkeypatch.setattr(
        net_mod.Net, "wait_reachable", lambda self, timeout, every=2.0: calls.timeouts.append(timeout) or calls.answers
    )
    return calls


def lan_only_device():
    dev = ScriptedDevice(Console([]))
    dev.lan_only = True  # what the device fixture sets when the console is unavailable
    return dev


@pytest.mark.host
def test_net_fixture_seeds_the_ip_from_the_env_and_waits_for_it(monkeypatch, reach):
    monkeypatch.setenv("NIMBUS_TEST_IP", f" {IP} ")
    dev = lan_only_device()
    n = conftest.net.__wrapped__(dev, None)
    assert n.ip == IP
    assert reach.timeouts == [net_mod.IP_READY_WAIT_S]
    assert dev.sent == []  # LAN-only: the console is never touched


@pytest.mark.host
@pytest.mark.parametrize("value", [None, "", "   "])
def test_net_fixture_without_the_env_leaves_the_ip_to_the_other_paths(monkeypatch, reach, value):
    if value is None:
        monkeypatch.delenv("NIMBUS_TEST_IP", raising=False)
    else:
        monkeypatch.setenv("NIMBUS_TEST_IP", value)
    n = conftest.net.__wrapped__(lan_only_device(), None)
    assert n.ip is None and reach.timeouts == []
    with pytest.raises(NetError, match="no IP known"):
        n._base(None)


@pytest.mark.host
def test_net_fixture_returns_even_when_the_seeded_ip_never_answers(monkeypatch, reach, capsys):
    reach.answers = False
    monkeypatch.setenv("NIMBUS_TEST_IP", IP)
    n = conftest.net.__wrapped__(lan_only_device(), None)
    assert n.ip == IP
    assert "did not answer" in capsys.readouterr().out


@pytest.mark.host
def test_net_fixture_lets_the_station_rejoin_after_the_session_reset(monkeypatch, reach, clock):
    """With a console the fixture waits out the rejoin, so a test that reads STATUS
    next sees wifi=1 (test_l42's own bench then never provisions)."""
    monkeypatch.setenv("NIMBUS_TEST_IP", IP)
    console = Console([[status_line(0)], [status_line(0)], [status_line(1)]])
    dev = ScriptedDevice(console)
    n = conftest.net.__wrapped__(dev, None)
    assert n.ip == IP and console.status_calls == 3 and console.provisions == 0
    assert dev.status().group("wifi") == "1"
    assert reach.timeouts == [net_mod.IP_READY_WAIT_S]


@pytest.mark.host
def test_net_fixture_prefers_the_boards_own_ip_over_a_stale_seed(monkeypatch, reach, clock, capsys):
    monkeypatch.setenv("NIMBUS_TEST_IP", "192.0.2.99")  # stale: another host may answer there
    n = conftest.net.__wrapped__(ScriptedDevice(Console([[status_line(1)]])), None)
    assert n.ip == IP
    assert "not NIMBUS_TEST_IP=192.0.2.99" in capsys.readouterr().out


@pytest.mark.host
def test_net_fixture_keeps_the_seed_when_the_station_stays_down(monkeypatch, reach, clock):
    monkeypatch.setenv("NIMBUS_TEST_IP", IP)
    console = Console([[status_line(0)]])
    n = conftest.net.__wrapped__(ScriptedDevice(console), None)
    assert n.ip == IP and console.provisions == 0  # never provisions; the test decides


@pytest.mark.host
def test_net_fixture_in_notifier_mode_does_not_wait(monkeypatch, reach, clock):
    """Notifier keeps the radio off and serves no web page: no rejoin wait, no
    reachability wait."""
    monkeypatch.setenv("NIMBUS_TEST_IP", IP)
    console = Console([[status_line(0, mode=0)]])
    start = clock.now
    n = conftest.net.__wrapped__(ScriptedDevice(console), None)
    assert n.ip == IP and console.status_calls == 1 and clock.now == start
    assert reach.timeouts == []


@pytest.mark.host
def test_wait_got_ip_still_replaces_a_seeded_ip(clock):
    dev, n = scripted(Console([]))
    n.ip = "192.0.2.10"
    dev.queue = ["WIFI_GOT_IP 192.0.2.11"]
    assert n.wait_got_ip() == "192.0.2.11" and n.ip == "192.0.2.11"


@pytest.mark.host
def test_wait_reachable_polls_until_the_ip_answers(monkeypatch, clock):
    http = FakeHttp(down_for=3)
    monkeypatch.setattr(net_mod, "requests", http)
    n = net_mod.Net(ScriptedDevice(Console([])))
    n.ip = IP
    assert n.wait_reachable(timeout=60.0) is True
    assert len(http.gets) == 4 and clock.now - 1000.0 < 60.0


@pytest.mark.host
def test_wait_reachable_gives_up_at_its_deadline(monkeypatch, clock):
    monkeypatch.setattr(net_mod, "requests", FakeHttp(down_for=10**6))
    n = net_mod.Net(ScriptedDevice(Console([])))
    n.ip = IP
    assert n.wait_reachable(timeout=60.0) is False
    assert clock.now - 1000.0 == pytest.approx(60.0)


# ---- 2. the rejoin is waited out, provisioning only when it cannot help --------------


@pytest.mark.host
def test_a_rejoin_after_a_restart_is_waited_out_not_provisioned(clock):
    """The bench failure: wifi=0 right after the reset, then the device rejoins by
    itself. No WIFI line may be sent."""
    console = Console([[status_line(0)], ["WIFI_DISCONNECTED reason=8", status_line(0)], [status_line(1)]])
    dev, n = scripted(console)
    joined = n.ensure_station("bench", Secret("bench-pass-0001"))
    assert joined == Join(IP, False, (8,), 1)
    assert console.provisions == 0 and wifi_lines(dev) == []
    assert n.ip == IP
    assert console.status_calls == 3


@pytest.mark.host
def test_a_got_ip_line_during_the_wait_counts_as_joined(clock):
    console = Console([[status_line(0)], [f"WIFI_GOT_IP {IP}"]])
    dev, n = scripted(console)
    assert n.ensure_station("bench", "pw").ip == IP
    assert console.provisions == 0


@pytest.mark.host
def test_no_stored_network_provisions_exactly_once_after_the_wait(clock):
    console = Console([[status_line(0)]], known=0)
    dev, n = scripted(console)
    start = clock.now
    joined = n.ensure_station("bench", Secret("bench-pass-0001"))
    assert joined.provisioned and joined.ip == IP
    assert console.provisions == 1 and len(wifi_lines(dev)) == 1
    # It waited the whole bounded rejoin window before deciding, and no longer.
    assert net_mod.REJOIN_WAIT_S <= clock.now - start <= net_mod.REJOIN_WAIT_S + 1.0


@pytest.mark.host
@pytest.mark.parametrize("reason", sorted(net_mod.AUTH_REASONS))
def test_rejected_stored_credentials_provision_exactly_once(clock, reason):
    console = Console([[f"WIFI_DISCONNECTED reason={reason}", status_line(0)], [status_line(0)]], known=2)
    dev, n = scripted(console)
    joined = n.ensure_station("bench", "pw")
    assert joined == Join(IP, True, (reason,), 1)
    assert console.provisions == 1


@pytest.mark.host
@pytest.mark.parametrize("reasons", [(), (8,), (8, 200), (201,), (4, 203, 8)])
def test_a_station_that_stays_down_for_other_reasons_is_never_provisioned(clock, reasons):
    first = [f"WIFI_DISCONNECTED reason={r}" for r in reasons] + [status_line(0)]
    console = Console([first, [status_line(0)]], known=1)
    dev, n = scripted(console)
    with pytest.raises(StationDown, match="Not re-provisioning"):
        n.ensure_station("bench", "pw")
    assert console.provisions == 0 and wifi_lines(dev) == []


@pytest.mark.host
def test_an_unknown_stored_count_without_an_auth_failure_is_never_provisioned(clock):
    console = Console([[status_line(0)]], known=None)  # an image without WIFIKNOWN?
    dev, n = scripted(console)
    with pytest.raises(StationDown, match="stored networks: unknown"):
        n.ensure_station("bench", "pw")
    assert console.provisions == 0


@pytest.mark.host
def test_a_busy_console_is_polled_again_within_the_bound(clock):
    console = Console([[], [], [status_line(1)]])  # two polls with no reply (booting)
    dev, n = scripted(console)
    assert n.ensure_station("bench", "pw").ip == IP
    assert console.status_calls == 3 and console.provisions == 0


@pytest.mark.host
def test_the_rejoin_bound_counts_from_the_consoles_first_answer(clock):
    """An SD scan can block STATUS for tens of seconds after a restart: the 20 s
    rejoin bound starts when the console first answers, not before."""
    console = Console([[]] * 30 + [[status_line(0)]] * 5 + [[status_line(1)]])
    dev, n = scripted(console)
    start = clock.now
    assert n.ensure_station("bench", "pw").ip == IP
    assert clock.now - start > net_mod.REJOIN_WAIT_S and console.provisions == 0


@pytest.mark.host
def test_a_console_that_never_answers_is_not_provisioned(clock):
    console = Console([[]])
    dev, n = scripted(console)
    start = clock.now
    with pytest.raises(StationDown, match="did not answer STATUS"):
        n.ensure_station("bench", "pw")
    assert clock.now - start == pytest.approx(net_mod.CONSOLE_BUSY_CAP_S, abs=1.0)
    assert console.provisions == 0 and "WIFIKNOWN?" not in dev.sent


@pytest.mark.host
def test_notifier_mode_stops_the_wait_at_once(clock):
    console = Console([[status_line(0, mode=0)]], known=0)
    dev, n = scripted(console)
    start = clock.now
    assert n.wait_station() == Join(None, False, (), 0) and clock.now == start
    with pytest.raises(StationDown, match="Notifier mode"):
        n.ensure_station("bench", "pw")
    assert console.provisions == 0


@pytest.mark.host
def test_needs_provisioning_over_every_reason():
    """The class rule over every reason code: only a missing network or an auth
    failure provisions, and an unknown count alone never does."""
    for reason in range(256):
        auth = classify_reason(reason) == "auth"
        assert needs_provisioning(1, [reason]) is auth
        assert needs_provisioning(None, [8, reason]) is auth
        assert needs_provisioning(0, [reason]) is True
    assert needs_provisioning(None, []) is False


def _cpp_reason_table():
    """classifyReason() in lib/core/src/wifi_policy.cpp as {code: class}, plus the
    default class."""
    src = (REPO / "lib" / "core" / "src" / "wifi_policy.cpp").read_text()
    body = src[src.index("JoinFail classifyReason(int r)") :]
    body = body[: body.index("\n}\n")]
    names = {"NotFound": "not-found", "AuthReject": "auth", "SelfInitiated": "self", "Transient": "transient"}
    table, pending, default = {}, [], None
    for line in body.splitlines():
        pending += [int(c) for c in re.findall(r"case\s+(\d+)\s*:", line)]
        m = re.search(r"return\s+JoinFail::(\w+)", line)
        if not m:
            continue
        if "default" in line:
            default = names[m.group(1)]
        for code in pending:
            table[code] = names[m.group(1)]
        pending = []
    return table, default


@pytest.mark.host
def test_reason_classes_match_the_firmware_table():
    table, default = _cpp_reason_table()
    assert table and default == "transient", "could not read classifyReason() from wifi_policy.cpp"
    for code in range(256):
        assert classify_reason(code) == table.get(code, default), f"reason {code}"


@pytest.mark.host
def test_the_older_fixture_pattern_survives_the_rejoin_race(clock):
    """test_l42's own bench, L37's token_device and L38's stored_creds_device still
    read STATUS once and provision on wifi=0. On the bench that provision printed
    reason 8 before the join and failed setup; it now reaches the IP."""
    console = Console([[status_line(0)]], known=1)
    dev, n = scripted(console)
    st = dev.status()
    assert st.group("wifi") == "0"
    n.provision("bench", "pw")
    assert n.wait_got_ip(timeout=25.0) == IP


@pytest.mark.host
def test_wait_got_ip_skips_the_devices_own_disconnect(clock):
    dev, n = scripted(Console([]))
    dev.queue = ["WIFI provisioning ssid=bench", "WIFI_DISCONNECTED reason=8", f"WIFI_GOT_IP {IP}"]
    assert n.wait_got_ip() == IP


@pytest.mark.host
def test_wait_got_ip_still_raises_on_a_real_failure(clock):
    dev, n = scripted(Console([]))
    dev.queue = ["WIFI_DISCONNECTED reason=8", "WIFI_DISCONNECTED reason=15"]
    with pytest.raises(WifiAuthFailure) as exc:
        n.wait_got_ip()
    assert exc.value.reason == 15


@pytest.mark.host
def test_wait_got_ip_times_out_rather_than_failing_on_reason_8_alone(clock):
    dev, n = scripted(Console([]))
    dev.queue = ["WIFI_DISCONNECTED reason=8"]
    with pytest.raises(ExpectTimeout):
        n.wait_got_ip(timeout=1.0)


@pytest.mark.host
def test_wait_disconnect_reason_returns_the_reason_after_the_devices_own(clock):
    dev, n = scripted(Console([]))
    dev.queue = ["WIFI_DISCONNECTED reason=8", "WIFI_DISCONNECTED reason=15"]
    assert n.wait_disconnect_reason() == 15
    assert dev.sent[-1] == "PING"


# ---- 2 + 3. the shared bench fixture -------------------------------------------------


def _fake_values():
    """A test-bot token and chat id generated at run time (never a literal), from
    alphabets the redacted reprs never use: the body is upper case and ':' so a
    leaked character is detectable; the last 4 are lower case."""
    rng = random.Random(1019)
    body = "".join(rng.choice(string.ascii_uppercase) for _ in range(10))
    body += ":" + "".join(rng.choice(string.ascii_uppercase) for _ in range(31))
    token = body + "wxyz"
    chat = "".join(rng.choice("123456789") for _ in range(10))
    return token, chat


def assert_redacted(text: str, secret: str, template: "str | None" = None) -> None:
    """``text`` shows no character of ``secret`` beyond its last 4: never a run of 5
    secret characters. With ``template`` (the repr's own fixed text, such as a
    class name or a dict key), also none of the characters before the tail may
    survive once the template and the allowed tail are removed; the generated
    values use alphabets the redacted repr never prints, so one leaked character
    is caught."""
    for i in range(len(secret) - SHOW_TAIL):
        assert secret[i : i + SHOW_TAIL + 1] not in text, f"{text!r} leaks part of a secret"
    if template is None:
        return
    leftover = text.replace(template, "").replace(secret[-SHOW_TAIL:], "")
    hidden = set(secret[:-SHOW_TAIL])
    assert not hidden.intersection(leftover), f"{text!r} leaks a secret character"


@pytest.fixture
def bot_env(monkeypatch, tmp_path):
    token, chat = _fake_values()
    monkeypatch.setenv("NIMBUS_TEST_TG_TOKEN", token)
    monkeypatch.setenv("NIMBUS_TEST_TG_CHAT", chat)
    monkeypatch.setenv("NIMBUS_TEST_STA_SSID", "bench")
    monkeypatch.setenv("NIMBUS_TEST_STA_PASS", "QWERTYUIOPASDFwxyz")
    monkeypatch.setenv("NIMBUS_TEST_TOKEN", "00112233445566778899aabb")
    env = tmp_path / ".env"
    env.write_text("ANTHROPIC_API_KEY=" + "ZXCVBNMLKJHGFDSAPOIUYTREWQ" + "wxyz\n", encoding="utf-8")
    return SimpleNamespace(token=token, chat=chat, secrets=Secrets(env_path=env))


@pytest.mark.host
def test_bench_fixture_waits_out_the_rejoin_and_yields_a_redacted_bot(monkeypatch, clock, bot_env):
    http = FakeHttp()
    monkeypatch.setattr(net_mod, "requests", http)
    # ensure_mode's own STATUS, then the rejoin wait: wifi=0, wifi=0, wifi=1.
    console = Console(
        [[status_line(0)], [status_line(0)], ["WIFI_DISCONNECTED reason=8", status_line(0)], [status_line(1)]]
    )
    dev, n = scripted(console)
    gen = conftest.bench.__wrapped__(dev, n, bot_env.secrets, lambda guard: guard())
    bot = next(gen)
    assert console.provisions == 0 and wifi_lines(dev) == []
    token, chat = bot  # unpacks like the old (token, chat) tuple
    assert (str(token), str(chat)) == (bot_env.token, bot_env.chat)
    assert http.posts[0][1] == {"tgToken": bot_env.token, "tgAllow": bot_env.chat}
    assert_redacted(repr(bot), bot_env.token, template="BenchBot")
    assert_redacted(repr(bot), bot_env.chat, template="BenchBot")
    assert next(gen, None) is None  # teardown clears the token
    assert http.posts[-1][1] == {"clr_tgToken": "1"}


@pytest.mark.host
def test_bench_fixture_provisions_once_when_the_station_stays_down(monkeypatch, clock, bot_env):
    monkeypatch.setattr(net_mod, "requests", FakeHttp())
    console = Console([[status_line(0)]], known=0)
    dev, n = scripted(console)
    gen = conftest.bench.__wrapped__(dev, n, bot_env.secrets, lambda guard: guard())
    next(gen)
    assert console.provisions == 1
    gen.close()


# ---- 3. no secret reaches a repr -----------------------------------------------------


@pytest.mark.host
def test_secret_repr_shows_at_most_the_last_4():
    token, chat = _fake_values()
    assert repr(Secret(token)) == "<secret ...wxyz>"
    assert repr(Secret(chat)) == "<secret>"  # short values show nothing
    assert repr(Secret("")) == "<secret empty>"
    assert redact(None) == "<secret none>"
    assert_redacted(repr((Secret(token), Secret(chat))), token, template="")
    assert_redacted(repr((Secret(token), Secret(chat))), chat, template="")
    assert_redacted(repr([Secret(token)]), token, template="")
    assert_redacted(repr({"tgToken": Secret(token)}), token, template="{'tgToken': ")


@pytest.mark.host
def test_secret_still_works_as_the_plain_value():
    token, _ = _fake_values()
    s = Secret(token)
    assert s == token and str(s) == token and f"bot{s}/getMe" == f"bot{token}/getMe"
    assert s.encode() == token.encode() and len(s) == len(token)
    # A value built from a secret stays redacted (the wrong-password legs do this).
    assert_redacted(repr(s + "_WRONG"), token + "_WRONG", template="")
    assert_redacted(repr("prefix-" + s), "prefix-" + token)


@pytest.mark.host
def test_the_secrets_reader_hands_out_redacted_values(bot_env):
    s = bot_env.secrets
    key = "ZXCVBNMLKJHGFDSAPOIUYTREWQ" + "wxyz"
    for value, raw in (
        (s.tg_test_token, bot_env.token),
        (s.tg_test_chat, bot_env.chat),
        (s.anthropic_key, key),
        (s.get("ANTHROPIC_API_KEY"), key),
        (s.sta_pass, "QWERTYUIOPASDFwxyz"),
    ):
        assert value == raw
        assert_redacted(repr(value), raw, template="")
    # The tuple test_l42's own bench fixture yields, built from the reader.
    assert_redacted(repr((s.tg_test_token, s.tg_test_chat)), bot_env.token, template="")
    assert_redacted(repr((s.tg_test_token, s.tg_test_chat)), bot_env.chat, template="")
    assert_redacted(repr(s.bench_bot()), bot_env.token, template="BenchBot")
    for raw in (bot_env.token, bot_env.chat, key):
        assert raw not in repr(s) and raw[:-SHOW_TAIL] not in repr(s)
    assert_redacted(repr(s._load()), key, template="{'ANTHROPIC_API_KEY': ")


@pytest.mark.host
def test_tokens_from_the_device_or_the_env_are_redacted(monkeypatch):
    monkeypatch.setenv("NIMBUS_TEST_TOKEN", "00112233445566778899aabb")
    assert repr(net_mod.Net(ScriptedDevice(Console([]))).token()) == "<secret ...aabb>"
    monkeypatch.delenv("NIMBUS_TEST_TOKEN")
    dev = ScriptedDevice(Console([]))
    dev.queue = ["WEBTOK 0123456789abcdef01234567"]
    assert repr(net_mod.Net(dev).token()) == "<secret ...4567>"
    dev.wifi("bench", Secret("QWERTYUIOPASDFwxyz"))
    assert isinstance(dev.sent[-1], Secret)  # the WIFI line carries the password


_LEAK_CONFTEST = textwrap.dedent(
    """
    # Loads the real HIL conftest under another name and uses its report hook.
    import importlib.util
    import os
    import sys

    HIL = os.environ["N2_HIL_DIR"]
    sys.path.insert(0, HIL)
    _spec = importlib.util.spec_from_file_location("hil_conftest", os.path.join(HIL, "conftest.py"))
    hil_conftest = importlib.util.module_from_spec(_spec)
    _spec.loader.exec_module(hil_conftest)
    pytest_addoption = hil_conftest.pytest_addoption
    pytest_configure = hil_conftest.pytest_configure
    pytest_unconfigure = hil_conftest.pytest_unconfigure
    pytest_runtest_makereport = hil_conftest.pytest_runtest_makereport
    """
)

_LIVE_PROBE = textwrap.dedent(
    """
    import os

    from redact import Secret


    def test_prints_live():
        print("bench bot", str(Secret(os.environ["NIMBUS_TEST_TG_TOKEN"])))
        print("raw env", os.environ["NIMBUS_LINEAR_MCP_TOKEN"])
    """
)

_LEAK_PROBE = textwrap.dedent(
    """
    import os

    import pytest

    from redact import Secret
    from secrets import BenchBot


    @pytest.fixture
    def bench():
        return BenchBot(Secret(os.environ["NIMBUS_TEST_TG_TOKEN"]), Secret(os.environ["NIMBUS_TEST_TG_CHAT"]))


    @pytest.fixture
    def old_bench():
        return Secret(os.environ["NIMBUS_TEST_TG_TOKEN"]), Secret(os.environ["NIMBUS_TEST_TG_CHAT"])


    def test_repr_path(bench, old_bench):
        token, chat = bench
        assert token.startswith("nope")


    def test_string_diff_path(bench):
        assert bench.token == "nope" + bench.token[4:-4] + "nope"


    def test_url_in_a_library_error(bench):
        url = f"https://api.telegram.org/bot{bench.token}/getMe?chat_id={bench.chat}"
        raise ConnectionError(f"Max retries exceeded with url: {url}")


    def test_skip_reason(bench):
        pytest.skip(f"bot {bench.token} for chat {bench.chat} is unavailable")


    def test_captured_output(bench):
        print("token is", str(bench.token), "chat is", str(bench.chat))
        assert False


    def test_raw_env_value_never_wrapped():
        raw = os.environ["NIMBUS_TEST_TG_TOKEN"]
        assert raw == "nope"
    """
)


@pytest.mark.host
def test_a_failing_leg_never_prints_the_bot(tmp_path):
    """End to end through pytest and the real report hook: failing legs that hold
    the bot in every shape the harness has seen (fixture args, the old tuple, a
    string diff, an f-string URL in a library error, a skip reason, captured
    output, a raw env value) are run with the long traceback and every local
    shown. Neither value may reach the output."""
    token, chat = _fake_values()
    (tmp_path / "conftest.py").write_text(_LEAK_CONFTEST, encoding="utf-8")
    (tmp_path / "test_probe.py").write_text(_LEAK_PROBE, encoding="utf-8")
    env = dict(os.environ, N2_HIL_DIR=str(HIL_DIR), NIMBUS_TEST_TG_TOKEN=token, NIMBUS_TEST_TG_CHAT=chat)
    argv = [sys.executable, "-m", "pytest", "-p", "no:cacheprovider", "--tb=long", "--showlocals", "-rA", str(tmp_path)]
    r = subprocess.run(argv, capture_output=True, text=True, timeout=60, cwd=tmp_path, env=env)
    out = r.stdout + r.stderr
    assert "5 failed, 1 skipped" in out, out[-3000:]
    assert "BenchBot(token=<secret ...wxyz>" in out  # the args line was printed, redacted
    for i in range(len(token) - SHOW_TAIL):
        assert token[i : i + SHOW_TAIL + 1] not in out, "the token reached the test output"
    assert chat not in out, "the chat id reached the test output"


@pytest.mark.host
def test_live_output_under_dash_s_is_scrubbed(tmp_path):
    """With -s a print reaches the terminal before any report exists (the lane A
    fill legs run that way): the conftest scrubs stdout itself."""
    token, _ = _fake_values()
    linear = token.lower()[::-1]  # a second fake, read raw from a credential-named variable
    (tmp_path / "conftest.py").write_text(_LEAK_CONFTEST, encoding="utf-8")
    (tmp_path / "test_live.py").write_text(_LIVE_PROBE, encoding="utf-8")
    env = dict(os.environ, N2_HIL_DIR=str(HIL_DIR), NIMBUS_TEST_TG_TOKEN=token, NIMBUS_LINEAR_MCP_TOKEN=linear)
    argv = [sys.executable, "-m", "pytest", "-p", "no:cacheprovider", "-s", "-q", str(tmp_path)]
    r = subprocess.run(argv, capture_output=True, text=True, timeout=60, cwd=tmp_path, env=env)
    out = r.stdout + r.stderr
    assert "1 passed" in out and "bench bot <secret ...wxyz>" in out, out[-2000:]
    assert token not in out and linear not in out and linear[:-SHOW_TAIL] not in out


@pytest.mark.host
@pytest.mark.parametrize(
    "name",
    [
        "NIMBUS_TEST_TG_TOKEN",
        "NIMBUS_TEST_TG_CHAT",
        "NIMBUS_TEST_OWNER_CHAT",
        "NIMBUS_TEST_STA_PASS",
        "NIMBUS_TEST_STA_PASS2",
        "NIMBUS_TEST_TOKEN",
        "NIMBUS_LINEAR_MCP_TOKEN",
        "ANTHROPIC_API_KEY",
        "GITHUB_PAT",
        "GOOGLE_OAUTH_CLIENT_SECRET",
        "nimbus_test_tg_token",
    ],
)
def test_credential_env_names_are_scrubbed(name):
    assert conftest.SECRET_ENV_NAME.search(name.upper()), name


@pytest.mark.host
@pytest.mark.parametrize(
    "name", ["PATH", "HOME", "NIMBUS_TEST_IP", "NIMBUS_TEST_STA_SSID", "NIMBUS_PORT", "KEYCHAIN_HOME", "PATHEXT"]
)
def test_other_env_names_are_left_alone(name):
    assert not conftest.SECRET_ENV_NAME.search(name), name


@pytest.mark.host
def test_short_secrets_scrub_whole_and_numbers_are_not_masked_by_accident():
    short = "Q" + uuid.uuid4().hex[:6]  # 7 characters, unique per run
    assert scrub(f"pass={short} end") == f"pass={short} end"  # not a Secret yet: left alone
    Secret(short)  # remembered, scrubbed whole
    assert scrub(f"pass={short} end") == "pass=<secret> end"
    assert scrub(f"pass={short[:5]} end") == f"pass={short[:5]} end"
    chat = str(uuid.uuid4().int)[:10]
    Secret(chat)  # a chat id: whole only, never by fragment
    assert scrub(f"uptime={chat[:8]} ms", fragments=True) == f"uptime={chat[:8]} ms"
    assert scrub(f"chat {chat}", fragments=True) == "chat <secret>"


@pytest.mark.host
def test_scrub_report_fails_closed(monkeypatch):
    def boom(*a, **k):
        raise ValueError("bad")

    monkeypatch.setattr(conftest, "scrub", boom)
    report = SimpleNamespace(longrepr="E   token in here", sections=[("Captured stdout call", "token")])
    conftest.scrub_report(report)
    assert "report withheld" in report.longrepr and "token" not in report.longrepr
    assert report.sections == []


@pytest.mark.host
def test_scrub_report_cleans_text_skips_and_sections(monkeypatch):
    token, chat = _fake_values()
    monkeypatch.setenv("NIMBUS_TEST_TG_TOKEN", token)  # known only through the env
    secret = Secret("ZXCVBNMLKJHGFDSAPOIUYTREWQ" + "wxyz")  # known because a Secret was built
    report = SimpleNamespace(
        longrepr=f"E   ConnectionError: url: /bot{token}/getMe key={secret}",
        sections=[("Captured stdout call", f"chat {chat} token {token}"), ("Captured log call", "nothing secret")],
    )
    conftest.scrub_report(report)
    assert report.longrepr == "E   ConnectionError: url: /bot<secret ...wxyz>/getMe key=<secret ...wxyz>"
    assert token not in report.sections[0][1] and report.sections[1] == ("Captured log call", "nothing secret")
    skipped = SimpleNamespace(longrepr=("x.py", 3, f"Skipped: bot {token}"), sections=[])
    conftest.scrub_report(skipped)
    assert skipped.longrepr == ("x.py", 3, "Skipped: bot <secret ...wxyz>")
    untouched = object()
    clean = SimpleNamespace(longrepr=untouched, sections=[])
    conftest.scrub_report(clean)
    assert clean.longrepr is untouched  # nothing to scrub: pytest's own object stays


@pytest.mark.host
def test_public_env_keys_stay_plain_and_are_never_scrubbed(tmp_path):
    env = tmp_path / ".env"
    env.write_text("NIMBUS_BOARD1_IP=192.0.2.123\nTESTING_WIFI_SSID=BenchNetwork\n", encoding="utf-8")
    s = Secrets(env_path=env)
    assert type(s.get("NIMBUS_BOARD1_IP")) is str and type(s.get("TESTING_WIFI_SSID")) is str
    assert (
        scrub("ORCH alive sta=1 ip=192.0.2.123 ssid=BenchNetwork")
        == "ORCH alive sta=1 ip=192.0.2.123 ssid=BenchNetwork"
    )


# ---- 4. esptool is found inside PlatformIO -------------------------------------------


def _exe(path: Path) -> Path:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text("#!/bin/sh\nexit 0\n", encoding="utf-8")
    path.chmod(0o755)
    return path


@pytest.fixture
def pio_home(tmp_path, monkeypatch):
    """A fake home with PlatformIO's penv scripts and the tool-esptoolpy package, an
    empty PATH directory, and no esptool module in this interpreter."""
    home = tmp_path / "home"
    penv = home / ".platformio" / "penv" / "bin"
    _exe(penv / "esptool.py")
    _exe(penv / "esptool")
    _exe(penv / "python")
    package = home / ".platformio" / "packages" / "tool-esptoolpy" / "esptool.py"
    package.parent.mkdir(parents=True)
    package.write_text("# esptool\n", encoding="utf-8")
    empty = tmp_path / "bin"
    empty.mkdir()
    monkeypatch.setattr(device_mod, "_esptool_module_available", lambda: False)
    real = os.path.realpath
    return SimpleNamespace(
        home=str(home),
        empty=str(empty),
        tmp=tmp_path,
        penv_py=real(penv / "esptool.py"),
        penv_bin=real(penv / "esptool"),
        python=str(penv / "python"),
        package=str(package),
    )


@pytest.mark.host
def test_esptool_falls_back_to_platformio_when_path_has_none(pio_home):
    assert esptool_commands(path=pio_home.empty, home=pio_home.home) == [
        [pio_home.penv_py],
        [pio_home.penv_bin],
        [pio_home.python, pio_home.package],
    ]


@pytest.mark.host
def test_esptool_on_path_comes_first(pio_home, monkeypatch):
    on_path = os.path.realpath(_exe(pio_home.tmp / "bin" / "esptool.py"))
    monkeypatch.setattr(device_mod, "_esptool_module_available", lambda: True)
    cmds = esptool_commands(path=pio_home.empty, home=pio_home.home)
    assert cmds[0] == [on_path]
    assert cmds[1] == [sys.executable, "-m", "esptool"]  # this interpreter's module, still before PlatformIO
    assert cmds[2:] == [[pio_home.penv_py], [pio_home.penv_bin], [pio_home.python, pio_home.package]]


@pytest.mark.host
def test_esptool_package_runs_under_this_python_without_a_penv_python(pio_home):
    os.remove(pio_home.python)
    assert esptool_commands(path=pio_home.empty, home=pio_home.home)[-1] == [sys.executable, pio_home.package]


@pytest.mark.host
def test_esptool_found_twice_is_listed_once(pio_home):
    penv_dir = os.path.dirname(pio_home.penv_py)
    cmds = esptool_commands(path=penv_dir, home=pio_home.home)
    assert cmds == [[pio_home.penv_py], [pio_home.penv_bin], [pio_home.python, pio_home.package]]


@pytest.mark.host
def test_no_esptool_anywhere_is_an_empty_list(tmp_path, monkeypatch):
    monkeypatch.setattr(device_mod, "_esptool_module_available", lambda: False)
    assert esptool_commands(path=str(tmp_path), home=str(tmp_path)) == []


@pytest.mark.host
def test_hard_reset_tries_path_then_platformio_in_order(pio_home, monkeypatch):
    """esptool_hard_reset with a fake PATH and home: every PlatformIO copy is tried
    in order until one succeeds (here, only the package does)."""
    monkeypatch.setenv("PATH", pio_home.empty)
    monkeypatch.setenv("HOME", pio_home.home)
    tried = []

    def run(argv, **kwargs):
        tried.append(argv)
        return SimpleNamespace(returncode=0 if pio_home.package in argv else 2, stdout="", stderr="")

    monkeypatch.setattr(device_mod.subprocess, "run", run)

    class Rig(Device):
        def __init__(self):
            super().__init__(port="/dev/cu.fake")
            self._ser = _Ser()

        def reopen_after_reenumerate(self, drain_boot=True):
            self._ser = _Ser()
            return self

    assert Rig().esptool_hard_reset() is True
    assert [argv[: argv.index("--chip")] for argv in tried] == [
        [pio_home.penv_py],
        [pio_home.penv_bin],
        [pio_home.python, pio_home.package],
    ]
    assert all(argv[argv.index("--port") + 1] == "/dev/cu.fake" for argv in tried)


# ---- bench leg: the rejoin wait on a real unit ---------------------------------------


@pytest.mark.hil
@pytest.mark.net
def test_station_rejoins_after_a_restart_without_provisioning(device, net, secrets, require_secret):
    """Restart a unit that has a stored network; ensure_station must find it back on
    the LAN by itself, with no WIFI line sent (the bench failure, inverted)."""
    require_secret(secrets.require_sta)
    device.ensure_mode(1)
    if not device.wifi_known_count():
        pytest.skip("needs a unit with a stored Wi-Fi network (WIFIKNOWN n>=1)")
    device.reset()
    joined = net.ensure_station(secrets.sta_ssid, secrets.sta_pass)
    assert joined.ip and not joined.provisioned, f"the harness re-provisioned a unit that rejoins by itself: {joined}"
    assert net.reachable(auth=False), f"STATUS reported {joined.ip}, but GET / did not answer"
