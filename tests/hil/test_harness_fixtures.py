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
from pathlib import Path
from types import SimpleNamespace

import pytest

import conftest
import device as device_mod
import net as net_mod
from device import Device, ExpectTimeout, esptool_commands
from net import Join, NetError, StationDown, WifiAuthFailure, classify_reason, needs_provisioning
from redact import SHOW_TAIL, Secret, redact
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


@pytest.mark.host
def test_net_fixture_seeds_the_ip_from_the_env_and_waits_for_it(monkeypatch):
    waits = []
    monkeypatch.setattr(net_mod.Net, "wait_reachable", lambda self, timeout, every=2.0: waits.append(timeout) or True)
    monkeypatch.setenv("NIMBUS_TEST_IP", f" {IP} ")
    n = conftest.net.__wrapped__(ScriptedDevice(Console([])), None)
    assert n.ip == IP
    assert waits == [net_mod.IP_READY_WAIT_S]


@pytest.mark.host
@pytest.mark.parametrize("value", [None, "", "   "])
def test_net_fixture_without_the_env_leaves_the_ip_to_the_other_paths(monkeypatch, value):
    waits = []
    monkeypatch.setattr(net_mod.Net, "wait_reachable", lambda self, timeout, every=2.0: waits.append(timeout) or True)
    if value is None:
        monkeypatch.delenv("NIMBUS_TEST_IP", raising=False)
    else:
        monkeypatch.setenv("NIMBUS_TEST_IP", value)
    n = conftest.net.__wrapped__(ScriptedDevice(Console([])), None)
    assert n.ip is None and waits == []
    with pytest.raises(NetError, match="no IP known"):
        n._base(None)


@pytest.mark.host
def test_net_fixture_returns_even_when_the_seeded_ip_never_answers(monkeypatch, capsys):
    monkeypatch.setattr(net_mod.Net, "wait_reachable", lambda self, timeout, every=2.0: False)
    monkeypatch.setenv("NIMBUS_TEST_IP", IP)
    n = conftest.net.__wrapped__(ScriptedDevice(Console([])), None)
    assert n.ip == IP
    assert "did not answer" in capsys.readouterr().out


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
    assert joined == Join(IP, False, (8,))
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
    assert joined == Join(IP, True, (reason,))
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


_LEAK_PROBE = textwrap.dedent(
    """
    import importlib.util
    import os
    import sys

    import pytest

    sys.path.insert(0, os.environ["N2_HIL_DIR"])
    from redact import Secret

    _spec = importlib.util.spec_from_file_location("hil_secrets", os.path.join(os.environ["N2_HIL_DIR"], "secrets.py"))
    hil_secrets = importlib.util.module_from_spec(_spec)
    _spec.loader.exec_module(hil_secrets)


    @pytest.fixture
    def bench():
        return hil_secrets.BenchBot(Secret(os.environ["N2_TOKEN"]), Secret(os.environ["N2_CHAT"]))


    @pytest.fixture
    def old_bench():
        return Secret(os.environ["N2_TOKEN"]), Secret(os.environ["N2_CHAT"])


    def test_fails_with_the_bot_in_scope(bench, old_bench):
        token, chat = bench
        assert token.startswith("nope")
    """
)


@pytest.mark.host
def test_a_failing_leg_never_prints_the_bot_in_its_traceback(tmp_path):
    """End to end through pytest itself: a failing test that takes both bench shapes,
    run with the long traceback and every local shown, prints neither value."""
    token, chat = _fake_values()
    (tmp_path / "test_probe.py").write_text(_LEAK_PROBE, encoding="utf-8")
    env = dict(os.environ, N2_HIL_DIR=str(HIL_DIR), N2_TOKEN=token, N2_CHAT=chat)
    r = subprocess.run(
        [sys.executable, "-m", "pytest", "-p", "no:cacheprovider", "--tb=long", "--showlocals", "-q", str(tmp_path)],
        capture_output=True,
        text=True,
        timeout=60,
        cwd=tmp_path,
        env=env,
    )
    out = r.stdout + r.stderr
    assert r.returncode == 1 and "1 failed" in out, out[-2000:]
    assert "BenchBot(token=<secret ...wxyz>" in out  # the args line was printed, redacted
    for i in range(len(token) - SHOW_TAIL):
        assert token[i : i + SHOW_TAIL + 1] not in out, "the token reached the traceback"
    assert chat not in out, "the chat id reached the traceback"


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
    monkeypatch.setattr(device_mod.importlib.util, "find_spec", lambda name: None)
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
    monkeypatch.setattr(device_mod.importlib.util, "find_spec", lambda name: object())
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
    monkeypatch.setattr(device_mod.importlib.util, "find_spec", lambda name: None)
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
