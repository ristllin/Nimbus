"""Host tier: every tool that opens, resets, flashes or scans a USB port refuses the forbidden boards. No board.

Each entry point is driven with a fake USB listing that shows a forbidden board on a real file node, and a fake pyserial
whose open is recorded: a refusal means no open happened, not just an error message. The class check at the end fails on
a new port-opening file that does not ask the guard, so this list cannot quietly fall behind the tree.
"""

from __future__ import annotations

import argparse
import ast
import importlib.util
import os
import re
import subprocess
import sys
import types
from pathlib import Path
from types import SimpleNamespace

import pytest

import port_guard

pytestmark = pytest.mark.host

guard = port_guard.load()
REPO = Path(__file__).resolve().parents[2]
TOOLS = REPO / "tools"
HIL = REPO / "tests" / "hil"
MACS = {mac: ":".join(mac[i : i + 2] for i in range(0, 12, 2)) for mac in guard.FORBIDDEN_MACS}
FORBIDDEN = sorted(MACS.values())
BENCH_MAC = "AA:BB:CC:00:11:22"
ESPRESSIF = guard.VID_ESPRESSIF


def tool(name: str):
    """A tools/ script loaded by file, as test_usb_reset.py does."""
    spec = importlib.util.spec_from_file_location(f"{name}_under_guard_test", TOOLS / f"{name}.py")
    module = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = module  # dataclasses look their module up here
    spec.loader.exec_module(module)
    return module


class FakeSerial:
    """pyserial's Serial with every open recorded on the class."""

    opened: list = []

    def __init__(self, port=None, *a, **k):
        self.port, self.dtr, self.rts, self.timeout = port, False, False, 0
        if port:
            self.open()

    def open(self):
        FakeSerial.opened.append(self.port)

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        pass

    def close(self):
        pass


def fake_serial_module():
    module = types.ModuleType("serial")
    module.Serial = FakeSerial
    module.SerialException = OSError
    return module


@pytest.fixture
def bus(tmp_path, monkeypatch):
    """A USB bus under test control: ``bus.set(serial)`` puts one board on a real node and returns the node."""
    FakeSerial.opened = []
    monkeypatch.delenv(guard.ALLOW_ENV, raising=False)
    monkeypatch.delenv(guard.ANON_ENV, raising=False)
    node = tmp_path / "cu.usbmodem2101"
    node.write_text("")
    rows = []

    def put(serial, vid=ESPRESSIF, device=None):
        rows.append(SimpleNamespace(device=device or str(node), serial_number=serial, vid=vid, pid=0x1001, product=""))
        return rows[-1].device

    monkeypatch.setattr(guard, "comports", lambda: list(rows))
    return SimpleNamespace(put=put, rows=rows, node=str(node))


@pytest.fixture(autouse=True)
def no_real_usb(monkeypatch):
    """Whatever a test forgets, nothing here reaches the real bus: the listing is empty and a subprocess refused."""
    monkeypatch.setattr(guard, "comports", lambda: [])

    def refuse(*a, **k):
        raise AssertionError(f"a test tried to start a process: {a[:1]}")

    monkeypatch.setattr(subprocess, "run", refuse)


# ---- the guard itself ---------------------------------------------------------------------------------


@pytest.mark.parametrize("mac", FORBIDDEN)
def test_a_forbidden_serial_is_refused_in_every_spelling(mac):
    for spelling in (mac, mac.lower(), mac.replace(":", ""), f"serial {mac} rev2"):
        with pytest.raises(guard.ForbiddenDevice, match="refused"):
            guard.check_mac(spelling, "test")
    assert guard.check_mac(BENCH_MAC, "test") is None
    assert guard.check_mac(None, "test") is None


def test_unit_a_is_not_lumi_though_it_shares_the_prefix():
    lumi = next(mac for mac in FORBIDDEN if mac.startswith("14:C1"))
    assert guard.forbidden_label(lumi)
    assert guard.forbidden_label(lumi[:12] + "2A:D0") is None  # unit A differs only in the last two octets


@pytest.mark.parametrize("mac", FORBIDDEN)
def test_a_port_is_refused_by_its_usb_serial_not_its_name(bus, mac):
    port = bus.put(mac)
    with pytest.raises(guard.ForbiddenDevice):
        guard.check_port(port, "test")


def test_a_bench_board_on_the_same_port_name_passes(bus):
    assert guard.check_port(bus.put(BENCH_MAC), "test").serial == BENCH_MAC


def test_the_other_macos_node_name_and_a_symlink_are_the_same_board(bus, tmp_path):
    bus.put(FORBIDDEN[0], device="/dev/cu.usbmodem2101")
    with pytest.raises(guard.ForbiddenDevice):
        guard.check_port("/dev/tty.usbmodem2101", "test")
    link = tmp_path / "friendly-name"
    link.symlink_to(bus.node)
    bus.put(FORBIDDEN[1])
    with pytest.raises(guard.ForbiddenDevice):
        guard.check_port(str(link), "test")


def test_a_native_port_with_no_serial_is_refused_until_a_person_checks(bus, monkeypatch):
    port = bus.put("")
    with pytest.raises(guard.AnonymousPort):
        guard.check_port(port, "test")
    monkeypatch.setenv(guard.ANON_ENV, "1")
    assert guard.check_port(port, "test") is not None


def test_a_uart_bridge_with_no_serial_passes_its_mac_is_checked_by_esptool(bus):
    assert guard.check_port(bus.put("", vid=0x10C4), "test") is not None


def test_an_unreadable_listing_refuses_a_real_port(bus, monkeypatch):
    def broken():
        raise ImportError("no pyserial")

    monkeypatch.setattr(guard, "comports", broken)
    with pytest.raises(guard.ForbiddenDevice, match="cannot read"):
        guard.check_port(bus.node, "test")
    assert guard.check_port("/dev/cu.gone", "test") is None  # no node: the open fails by itself


def test_the_override_lifts_one_exact_mac_and_no_other(bus, monkeypatch, capsys):
    monkeypatch.setenv(guard.ALLOW_ENV, FORBIDDEN[0])
    assert guard.forbidden_label(FORBIDDEN[0]) is None
    assert "lifted" in capsys.readouterr().err
    assert guard.forbidden_label(FORBIDDEN[1])
    monkeypatch.setenv(guard.ALLOW_ENV, FORBIDDEN[0][:8])  # a prefix is not the MAC
    assert guard.forbidden_label(FORBIDDEN[0])


def test_the_command_line_refuses_with_its_own_exit_code(bus, capsys):
    port = bus.put(FORBIDDEN[0])
    assert guard.main(["check-port", port]) == guard.REFUSED_EXIT
    assert guard.main(["check-mac", FORBIDDEN[1]]) == guard.REFUSED_EXIT
    assert guard.main(["port-for-mac", FORBIDDEN[0]]) == guard.REFUSED_EXIT
    assert guard.main(["check-mac", BENCH_MAC]) == 0
    assert guard.main(["list"]) == 0
    assert "REFUSED" in capsys.readouterr().out


def test_port_for_mac_never_needs_an_open(bus):
    port = bus.put(BENCH_MAC)
    assert guard.port_for_mac(BENCH_MAC.lower()) == port
    assert guard.port_for_mac("AA:BB:CC:00:11:23") is None
    assert FakeSerial.opened == []


# ---- usb_reset.py ------------------------------------------------------------------------------------


@pytest.fixture
def usb():
    return tool("usb_reset")


@pytest.mark.parametrize("mac", FORBIDDEN)
def test_usb_reset_skip_and_all_leave_a_forbidden_board_out(usb, mac):
    serials = [mac, BENCH_MAC]
    assert usb.select_targets(serials, all_=True) == ([1], None)
    assert usb.select_targets(serials, skip="11:22:33") == ([1], None)
    assert usb.select_targets(serials, serial=BENCH_MAC) == ([1], None)


@pytest.mark.parametrize("mac", FORBIDDEN)
def test_usb_reset_refuses_when_only_a_forbidden_board_is_chosen(usb, mac):
    for kwargs in ({"serial": mac}, {"all_": True}, {"skip": BENCH_MAC}, {}):
        sel, err = usb.select_targets([mac], **kwargs)
        assert sel == [] and "refused" in err, kwargs
    sel, err = usb.select_targets([mac, BENCH_MAC], serial=mac[:5])  # a loose match reaching it
    assert sel == [] and "refused" in err


class FakeUsbDevice:
    def __init__(self, serial):
        self.serial, self.idVendor, self.idProduct, self.bus, self.address = serial, 0x303A, 0x1001, 1, 1
        self.was_reset = False
        self.iSerialNumber = 3

    def reset(self):
        self.was_reset = True


def run_usb_main(usb, monkeypatch, serials, argv):
    devices = [FakeUsbDevice(s) for s in serials]
    core = SimpleNamespace(find=lambda **k: iter(devices))
    monkeypatch.setattr(usb, "load_usb_backend", lambda platform: (core, "backend"))
    monkeypatch.setattr(usb, "_serial", lambda d: d.serial)
    monkeypatch.setattr(usb.time, "sleep", lambda s: None)
    return usb.main(argv), devices


@pytest.mark.parametrize("mac", FORBIDDEN)
@pytest.mark.parametrize(
    "argv",
    [[], ["--all"], ["--all", "--any"], ["--skip", BENCH_MAC], ["--skip", BENCH_MAC, "--any"]],
)
def test_usb_reset_main_never_touches_a_forbidden_board(usb, monkeypatch, mac, argv):
    rc, devices = run_usb_main(usb, monkeypatch, [mac], argv)
    assert not devices[0].was_reset
    assert rc != 0


def test_usb_reset_main_with_serial_of_a_forbidden_board(usb, monkeypatch):
    rc, devices = run_usb_main(usb, monkeypatch, [FORBIDDEN[0], BENCH_MAC], ["--serial", FORBIDDEN[0]])
    assert rc != 0 and not any(d.was_reset for d in devices)


def test_usb_reset_all_resets_the_bench_board_beside_a_forbidden_one(usb, monkeypatch):
    rc, devices = run_usb_main(usb, monkeypatch, [FORBIDDEN[0], BENCH_MAC], ["--all"])
    assert rc == 0 and [d.was_reset for d in devices] == [False, True]


def test_usb_reset_refuses_a_board_with_no_serial_until_a_person_checks(usb, monkeypatch):
    rc, devices = run_usb_main(usb, monkeypatch, ["", BENCH_MAC], ["--skip", BENCH_MAC])
    assert rc != 0 and not devices[0].was_reset
    rc, devices = run_usb_main(usb, monkeypatch, ["", BENCH_MAC], ["--skip", BENCH_MAC, "--anonymous-ok"])
    assert rc == 0 and devices[0].was_reset and not devices[1].was_reset
    monkeypatch.setenv(guard.ANON_ENV, "1")
    rc, devices = run_usb_main(usb, monkeypatch, ["", BENCH_MAC], ["--skip", BENCH_MAC])
    assert rc == 0 and devices[0].was_reset


# ---- board_info.py -------------------------------------------------------------------------------------


@pytest.fixture
def board_info(monkeypatch):
    module = tool("board_info")
    monkeypatch.setattr(module, "serial", fake_serial_module())
    return module


@pytest.mark.parametrize("mac", FORBIDDEN)
def test_board_info_refuses_a_named_forbidden_port(bus, board_info, mac):
    assert board_info.main(["--port", bus.put(mac), "--seconds", "0"]) == guard.REFUSED_EXIT
    assert FakeSerial.opened == []


def test_board_info_with_no_port_lists_and_opens_nothing(bus, board_info, capsys):
    bus.put(FORBIDDEN[0])
    bus.put(BENCH_MAC, device="/dev/cu.usbmodem1101")
    assert board_info.main([]) == 0
    out = capsys.readouterr().out
    assert FakeSerial.opened == []
    assert "REFUSED" in out and BENCH_MAC in out and "Nothing was opened" in out


def test_board_info_reads_a_bench_board(bus, board_info, monkeypatch):
    port = bus.put(BENCH_MAC)
    monkeypatch.setattr(FakeSerial, "read", lambda self, n: b"", raising=False)
    monkeypatch.setattr(FakeSerial, "write", lambda self, data: None, raising=False)
    assert board_info.main(["--port", port, "--seconds", "0"]) == 0
    assert FakeSerial.opened == [port]


# ---- setup_device.py -----------------------------------------------------------------------------------


@pytest.fixture
def setup_device(monkeypatch):
    module = tool("setup_device")
    fake = fake_serial_module()
    tools_pkg = types.ModuleType("serial.tools")
    listing = types.ModuleType("serial.tools.list_ports")
    listing.comports = lambda: guard.comports()
    tools_pkg.list_ports = listing
    monkeypatch.setitem(sys.modules, "serial", fake)
    monkeypatch.setitem(sys.modules, "serial.tools", tools_pkg)
    monkeypatch.setitem(sys.modules, "serial.tools.list_ports", listing)
    return module


@pytest.mark.parametrize("mac", FORBIDDEN)
def test_setup_device_scan_leaves_a_forbidden_board_out(bus, setup_device, mac, capsys):
    bus.put(mac)
    bench = bus.put(BENCH_MAC, device="/dev/cu.usbmodem1101")
    assert [b["port"] for b in setup_device.enumerate_esp_ports()] == [bench]
    assert "Leaving out" in capsys.readouterr().err


@pytest.mark.parametrize("mac", FORBIDDEN)
def test_setup_device_refuses_a_named_forbidden_port(bus, setup_device, mac):
    port = bus.put(mac)
    args = argparse.Namespace(port=port, yes=True)
    with pytest.raises(guard.ForbiddenDevice):
        setup_device.resolve_port(args)


def test_setup_device_every_open_asks_the_guard(bus, setup_device):
    port = bus.put(FORBIDDEN[0])
    for call in (
        lambda: setup_device.identify(port),
        lambda: setup_device.verify_panel_after_flash(port),
        lambda: setup_device.probe_for_freenove(port),
        lambda: setup_device._upload("pio", "esp32s3", port),
    ):
        with pytest.raises(guard.ForbiddenDevice):
            call()
    assert setup_device.serial_bootstrap(port, None, None, False) == 1
    assert FakeSerial.opened == []


def test_setup_device_refuses_the_mac_esptool_reads_behind_a_uart_bridge(bus, setup_device, monkeypatch):
    port = bus.put("", vid=0x10C4)
    done = SimpleNamespace(stdout=f"MAC: {FORBIDDEN[0].lower()}\n", returncode=0)
    monkeypatch.setattr(setup_device, "run_checked", lambda *a, **k: done)
    with pytest.raises(guard.ForbiddenDevice):
        setup_device.inspect_board(["esptool"], port)


# ---- the other tools that open a port ---------------------------------------------------------------------


@pytest.mark.parametrize("mac", FORBIDDEN)
def test_push_firmware_refuses(bus, monkeypatch, mac):
    monkeypatch.setitem(sys.modules, "serial", fake_serial_module())  # it stops at import without pyserial
    module = tool("push_firmware")
    with pytest.raises(guard.ForbiddenDevice):
        module._open_port(bus.put(mac), 115200)
    assert FakeSerial.opened == []


@pytest.mark.parametrize("mac", FORBIDDEN)
def test_fsput_stream_refuses(bus, monkeypatch, mac):
    module = tool("fsput_stream")
    monkeypatch.setattr(module, "serial", fake_serial_module())
    with pytest.raises(guard.ForbiddenDevice):
        module._open_and_boot(argparse.Namespace(port=bus.put(mac), baud=115200))
    assert FakeSerial.opened == []


@pytest.mark.parametrize("mac", FORBIDDEN)
def test_tcal_wizard_refuses(bus, monkeypatch, mac):
    module = tool("tcal_wizard")
    monkeypatch.setattr(module, "serial", fake_serial_module())
    with pytest.raises(guard.ForbiddenDevice):
        module.Console(bus.put(mac))
    assert FakeSerial.opened == []


@pytest.mark.parametrize("mac", FORBIDDEN)
def test_ring_check_refuses(bus, monkeypatch, mac):
    monkeypatch.setitem(sys.modules, "serial", fake_serial_module())
    module = tool("ring_check")
    monkeypatch.setattr(sys, "argv", ["ring_check.py", "--port", bus.put(mac)])
    with pytest.raises(SystemExit, match="refused"):
        module.main()
    assert FakeSerial.opened == []


def test_nsn_send_asks_the_guard_before_its_diagnostic_open():
    source = (TOOLS / "nsn_send.py").read_text(encoding="utf-8")  # it imports bleak, so it is read, not run
    assert source.index('bench_guard.check_port(PORT, "nsn_send")') < source.index("serial.Serial()")


def test_run_gate_stays_port_free_so_it_needs_no_refusal_leg():
    """run_gate.py prints the bench battery but opens no port itself, so the command-line parametrize below has no
    row for it. If it ever gains a port-opening step, this fails and the step must ask bench_guard first."""
    source = (TOOLS / "release_gate" / "run_gate.py").read_text(encoding="utf-8")
    offenders = []
    for node in ast.walk(ast.parse(source)):
        if (
            isinstance(node, ast.Call)
            and isinstance(node.func, ast.Attribute)
            and node.func.attr in ("Serial", "list_ports")
        ):
            offenders.append(node.lineno)
        if (
            isinstance(node, ast.Constant)
            and isinstance(node.value, str)
            and re.search(r"/dev/(cu|tty)\.|usbmodem|esptool", node.value)
        ):
            offenders.append(node.lineno)
    assert not offenders, (
        f"run_gate.py gained a port-touching step at lines {offenders}; wire bench_guard.check_port first"
    )


# ---- the HIL harness ---------------------------------------------------------------------------------------


@pytest.fixture
def device_mod(monkeypatch):
    import device as module

    monkeypatch.setattr(module, "serial", fake_serial_module())
    return module


@pytest.mark.parametrize("mac", FORBIDDEN)
def test_harness_open_refuses_a_pinned_forbidden_port(bus, device_mod, mac):
    with pytest.raises(guard.ForbiddenDevice):
        device_mod.Device(port=bus.put(mac)).open()
    assert FakeSerial.opened == []


def test_harness_open_refuses_the_sole_port_it_would_have_guessed(bus, device_mod, monkeypatch):
    port = bus.put(FORBIDDEN[0])
    monkeypatch.setattr(device_mod, "list_ports", lambda: [port])
    with pytest.raises(guard.ForbiddenDevice):
        device_mod.Device().open()
    assert FakeSerial.opened == []


def test_harness_treats_a_port_with_no_serial_as_lost_so_its_reopen_loop_waits(bus, device_mod):
    with pytest.raises(device_mod.DeviceLostError):
        device_mod.Device._open_quiet(bus.put(""), 115200)
    assert FakeSerial.opened == []


@pytest.mark.parametrize("mac", FORBIDDEN)
def test_harness_esptool_reset_refuses_before_closing_or_starting_anything(bus, device_mod, mac):
    dev = device_mod.Device(port=bus.put(mac))
    with pytest.raises(guard.ForbiddenDevice):
        dev.esptool_hard_reset()  # the autouse fixture fails the test if a process starts


@pytest.mark.parametrize("mac", FORBIDDEN)
def test_harness_bus_reset_refuses_a_forbidden_target(device_mod, mac):
    with pytest.raises(guard.ForbiddenDevice):
        device_mod.Device.bus_reset(target_serial=mac)


def test_harness_flash_names_the_port_and_refuses_a_forbidden_one(bus, device_mod, monkeypatch):
    monkeypatch.setenv("NIMBUS_HIL_FLASH_OK", "1")
    with pytest.raises(device_mod.DeviceError, match="--port"):
        device_mod.Device().__class__.flash_env(device_mod.Device(), "test-cyd", allow_hardware=True)
    dev = device_mod.Device(port=bus.put(FORBIDDEN[0]))
    with pytest.raises(guard.ForbiddenDevice):
        device_mod.Device.flash_env(dev, "test-cyd", allow_hardware=True)


def test_prov_board_refuses(bus, monkeypatch, tmp_path):
    import prov_board

    monkeypatch.setitem(sys.modules, "serial", fake_serial_module())
    port = bus.put(FORBIDDEN[0])
    with pytest.raises(guard.ForbiddenDevice):
        prov_board.Console(port)
    with pytest.raises(guard.ForbiddenDevice):
        prov_board.main(["--label", "x", "--port", port, "--mac", FORBIDDEN[1], "--out-dir", str(tmp_path)])
    with pytest.raises(guard.ForbiddenDevice):
        prov_board.main(["--label", "x", "--port", port, "--out-dir", str(tmp_path)])
    assert FakeSerial.opened == []


def _bench_driver_console(port, tmp_path):
    import bench_driver

    return bench_driver.Console(port, str(tmp_path / "console.log"))


@pytest.mark.parametrize("mac", FORBIDDEN)
def test_the_new_drivers_console_refuses(bus, monkeypatch, tmp_path, mac):
    monkeypatch.setitem(sys.modules, "serial", fake_serial_module())
    with pytest.raises(guard.ForbiddenDevice):
        _bench_driver_console(bus.put(mac), tmp_path)
    assert FakeSerial.opened == []


@pytest.mark.parametrize(
    "module,argv",
    [
        (
            "soak_24h",
            ["--ip", "192.0.2.1", "--token-file", "{tok}", "--out", "{out}", "--port", "{port}", "--mac", "{mac}"],
        ),
        ("soak_24h", ["--ip", "192.0.2.1", "--token-file", "{tok}", "--out", "{out}", "--port", "{port}"]),
        (
            "ota_rehearsal",
            ["--ip", "192.0.2.1", "--token-file", "{tok}", "--out", "{out}", "--port", "{port}", "--mac", "{mac}"]
            + ["--url", "https://192.0.2.2/m.json", "--want-build", "v5"],
        ),
        (
            "bench_rows",
            [
                "A07",
                "--ip",
                "192.0.2.1",
                "--token-file",
                "{tok}",
                "--out",
                "{out}",
                "--port",
                "{port}",
                "--mac",
                "{mac}",
            ],
        ),
        (
            "onboarding_gate",
            ["--port", "{port}", "--mac", "{mac}", "--out", "{out}", "--env-file", "{env}"],
        ),
    ],
)
def test_the_new_drivers_refuse_on_their_command_lines(bus, tmp_path, module, argv):
    mod = __import__(module)
    token = tmp_path / "tok"
    token.write_text("t" * 24)
    env = tmp_path / "env"
    env.write_text("NIMBUS_TEST_STA_SSID=x\n")
    port = bus.put(FORBIDDEN[0])
    filled = [a.format(tok=token, out=tmp_path / "ev", port=port, mac=FORBIDDEN[0], env=env) for a in argv]
    assert mod.main(filled) == guard.REFUSED_EXIT
    assert FakeSerial.opened == []


# ---- the pio upload hook ------------------------------------------------------------------------------------


def _upload_hook():
    """Run tools/pio_upload_guard.py the way SCons does, with a fake env."""
    actions = {}
    env = SimpleNamespace(
        __getitem__=None,
        AddPreAction=lambda target, fn: actions.__setitem__(target, fn),
    )

    class Env(dict):
        AddPreAction = staticmethod(env.AddPreAction)

    fake_env = Env(PROJECT_DIR=str(REPO))
    source = (TOOLS / "pio_upload_guard.py").read_text(encoding="utf-8")
    exec(
        compile(source, "pio_upload_guard.py", "exec"), {"Import": lambda name: None, "env": fake_env, "__name__": "x"}
    )
    return actions["upload"]


def test_pio_upload_refuses_a_named_forbidden_port(bus):
    hook = _upload_hook()
    port = bus.put(FORBIDDEN[0])
    assert hook(None, None, SimpleNamespace(subst=lambda name: port)) == 1


def test_pio_upload_with_no_port_refuses_while_a_forbidden_board_is_on_the_bus(bus):
    hook = _upload_hook()
    bus.put(FORBIDDEN[1])
    assert hook(None, None, SimpleNamespace(subst=lambda name: "")) == 1


def test_pio_upload_passes_a_bench_board(bus):
    hook = _upload_hook()
    port = bus.put(BENCH_MAC)
    assert hook(None, None, SimpleNamespace(subst=lambda name: port)) == 0
    assert hook(None, None, SimpleNamespace(subst=lambda name: "")) == 0


def test_every_firmware_env_carries_the_upload_hook():
    ini = (REPO / "platformio.ini").read_text(encoding="utf-8")
    assert "pre:tools/pio_upload_guard.py" in ini
    assert len(re.findall(r"^extra_scripts", ini, re.M)) == 1, "an env's own extra_scripts would drop the hook"


# ---- the class check: nothing new goes round the guard ---------------------------------------------------------


def _sources():
    for root in (TOOLS, HIL):
        for path in sorted(root.rglob("*.py")):
            if "__pycache__" in path.parts or "node_modules" in path.parts:
                continue
            yield path


def _is_host_test(path: Path) -> bool:
    """A host test's fake serial is not a port open."""
    return path.name.startswith("test_") and (
        path.parent == TOOLS or path.stem.endswith("_host") or "release_gate" in path.parts
    )


def port_opening_files_without_the_guard(sources):
    opens = re.compile(r"\bserial\.Serial\(")
    asks = re.compile(r"\bcheck_port\(")
    return [
        path.name
        for path in sources
        if not _is_host_test(path)
        and opens.search(path.read_text(encoding="utf-8"))
        and not asks.search(path.read_text(encoding="utf-8"))
    ]


def test_every_file_that_opens_a_port_asks_the_guard():
    assert port_opening_files_without_the_guard(_sources()) == []


def test_the_class_check_catches_a_new_opener(tmp_path):
    bad = tmp_path / "new_tool.py"
    bad.write_text("import serial\ns = serial.Serial('/dev/x')\n")
    good = tmp_path / "good_tool.py"
    good.write_text("bench_guard.check_port(p)\ns = serial.Serial(p)\n")
    assert port_opening_files_without_the_guard([bad, good]) == ["new_tool.py"]


def test_nothing_in_the_tree_sets_the_override():
    offenders = []
    for path in _sources():
        if path.name in ("bench_guard.py", "test_bench_guard_host.py"):
            continue
        if "NIMBUS_BENCH_ALLOW_MAC" in path.read_text(encoding="utf-8"):
            offenders.append(path.name)
    assert offenders == []


def test_no_runner_or_doc_names_a_forbidden_mac():
    """The list lives in one place: a MAC in a runner or a how-to is how it gets pasted into a command."""
    patterns = [re.compile(":?".join(mac[i : i + 2] for i in range(0, 12, 2)), re.I) for mac in guard.FORBIDDEN_MACS]
    offenders = []
    roots = [p for p in _sources() if p.name not in ("bench_guard.py",)]
    roots += sorted((REPO / "docs").rglob("*.md")) + sorted(HIL.glob("*.md"))
    for path in roots:
        text = path.read_text(encoding="utf-8", errors="replace")
        if any(p.search(text) for p in patterns):
            offenders.append(os.path.relpath(path, REPO))
    assert offenders == []
