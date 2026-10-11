#!/usr/bin/env python3
"""Host-only checks for the guarded device installer.

Covers the decision logic a board owner never sees fail loudly: which family a
port belongs to, what gets seeded, and the identify-and-confirm gate. The actual
flashing is hardware and is not exercised here."""

from __future__ import annotations

import argparse
import contextlib
import importlib.util
import io
import os
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock


def _load():
    path = Path(__file__).with_name("setup_device.py")
    spec = importlib.util.spec_from_file_location("setup_device", path)
    module = importlib.util.module_from_spec(spec)
    # Register before exec so a @dataclass in the module can resolve its own module
    # namespace (under `from __future__ import annotations` the decorator looks the
    # module up in sys.modules); without this the load fails at collection time.
    sys.modules["setup_device"] = module
    spec.loader.exec_module(module)
    return module


SETUP = _load()


class _Input:
    """Feed a scripted sequence of answers to the module's input()."""

    def __init__(self, *answers):
        self.answers = list(answers)

    def __call__(self, _prompt=""):
        return self.answers.pop(0)


def _args(**kw):
    base = dict(display=None, mode=None, board=None, size=None, yes=False, port=None)
    base.update(kw)
    return argparse.Namespace(**base)


@contextlib.contextmanager
def _patched(**values):
    """Temporarily replace SETUP attributes, restoring the originals afterwards."""
    saved = {name: getattr(SETUP, name) for name in values}
    for name, value in values.items():
        setattr(SETUP, name, value)
    try:
        yield
    finally:
        for name, value in saved.items():
            setattr(SETUP, name, value)


# The first 256 bytes of the NVS page inside the published v4.5.8 freenove-35 web-flash
# image (ESP-IDF nvs_partition_gen output; the rest of the 4 KB page is 0xFF). It holds
# solide=1, scrModel='tft', nimbus_mode=1 and otaType='freenove-35'.
_REAL_SEED_PAGE = bytes.fromhex(
    "feffffff00000000feffffffffffffffffffffffffffffffffffffff842dbab9aafaffffffffffffffffffffffff"
    "ffffffffffffffffffffffffffffffffffff000101ff3e1e0100736f6c6964650000000000000000000001ffffff"
    "ffffffff012102ff98cdde227363724d6f64656c00000000000000000400ffff54c3ab1c74667400ffffffffffff"
    "ffffffffffffffffffffffffffffffffffffffffffff011401ffd9cc72726e696d6275735f6d6f64650000000000"
    "01000000ffffffff012102ffee4925946f7461547970650000000000000000000c00ffff02a0965d667265656e6f"
    "76652d333500ffffffffffffffffffffffffffffffffffffffff"
) + b"\xff" * (4096 - 256)
_BLANK_PAGE = b"\xff" * 4096


def _nvs_page(*items):
    """One NVS page of string items: (key, value, 'written' | 'erased'), laid out the way
    ESP-IDF writes them (header entry, then the value in the following entries)."""
    page = bytearray(_BLANK_PAGE)
    page[0:4] = (0xFFFFFFFE).to_bytes(4, "little")  # active page
    index = 0
    for key, value, state in items:
        data = value.encode() + b"\0"
        span = 1 + (len(data) + 31) // 32
        header = bytes([1, SETUP.NVS_TYPE_STRING, span, 0xFF]) + bytes(4) + key.encode().ljust(16, b"\0")
        header += len(data).to_bytes(2, "little") + b"\xff\xff" + bytes(4)
        page[64 + index * 32 : 96 + index * 32] = header
        page[96 + index * 32 : 64 + (index + span) * 32] = data.ljust((span - 1) * 32, b"\xff")
        bits = SETUP.NVS_ENTRY_WRITTEN if state == "written" else 0b00
        for entry in range(index, index + span):
            byte, shift = 32 + entry // 4, (entry % 4) * 2
            page[byte] = (page[byte] & ~(0b11 << shift)) | (bits << shift)
        index += span
    return bytes(page)


def _partition(*pages):
    """A 5-page (0x5000) NVS partition dump: the given pages, then blank ones."""
    return b"".join(pages) + _BLANK_PAGE * (5 - len(pages))


# ---- NVS + MAC (unchanged surface) ----------------------------------------


def test_nvs_classification_does_not_need_to_decode_values():
    assert SETUP.classify_nvs(b"\xff" * 20480) == "blank"
    assert SETUP.classify_nvs(b"\xff" * 40 + b"nimbus_mode\x00scrModel" + b"\xff" * 40) == "nimbus"
    assert SETUP.classify_nvs(b"solide\x00staSsid") == "other"


def test_extracts_the_factory_mac_from_esptool_output():
    output = "Connected to ESP32-S3\nMAC:                AA:BB:CC:DD:EE:FF\n"
    assert SETUP.extract_mac(output) == "aa:bb:cc:dd:ee:ff"
    assert SETUP.extract_mac("no identity here") is None


# ---- board-family autodetect ----------------------------------------------


def test_family_from_usb_bridge_is_solide_native_is_ambiguous():
    assert SETUP.family_from_usb(SETUP.VID_CP210X) == SETUP.FAMILY_SOLIDE
    assert SETUP.family_from_usb(SETUP.VID_CH34X) == SETUP.FAMILY_SOLIDE
    # The native ESP32-S3 USB is shared by the Freenove and a native-flashed
    # Solide, so USB alone cannot decide - NVS refines it.
    assert SETUP.family_from_usb(SETUP.VID_ESP32S3_NATIVE) is None
    assert SETUP.family_from_usb(None) is None


def test_family_from_nvs_reads_the_live_ota_type_setting():
    # A real nvs_partition_gen seed (the published freenove-35 image) parses to its type.
    assert SETUP.saved_ota_type(_partition(_REAL_SEED_PAGE)) == "freenove-35"
    assert SETUP.family_from_nvs(_partition(_REAL_SEED_PAGE)) == SETUP.FAMILY_FREENOVE
    # Every type the installer seeds maps back to its own family (the class).
    for slug in [SETUP.OTA_TYPE_SOLIDE, *SETUP.FREENOVE_SLUGS]:
        want = SETUP.FAMILY_SOLIDE if slug == SETUP.OTA_TYPE_SOLIDE else SETUP.FAMILY_FREENOVE
        assert SETUP.family_from_nvs(_partition(_nvs_page(("otaType", slug, "written")))) == want, slug
    assert SETUP.family_from_nvs(_partition()) is None  # blank NVS


def test_resolve_family_precedence_and_ambiguity():
    # Explicit --board always wins.
    assert SETUP.resolve_family("freenove_s3", SETUP.VID_CP210X, SETUP.FAMILY_SOLIDE) == "freenove_s3"
    # Then a configured board's own NVS marker beats the USB hint.
    assert SETUP.resolve_family(None, SETUP.VID_ESP32S3_NATIVE, SETUP.FAMILY_SOLIDE) == SETUP.FAMILY_SOLIDE
    # Then the USB bridge implies Solide.
    assert SETUP.resolve_family(None, SETUP.VID_CP210X, None) == SETUP.FAMILY_SOLIDE
    # A native board with no marker is AMBIGUOUS (could be a native-flashed Solide,
    # like Nimbus-4) -> None, so we never silently flash the wrong pinout.
    assert SETUP.resolve_family(None, SETUP.VID_ESP32S3_NATIVE, None) is None


def test_prompt_family_refuses_under_yes_and_asks_interactively():
    try:
        SETUP.prompt_family("/dev/cu.usbmodem1", assume_yes=True)
    except RuntimeError as exc:
        assert "--board" in str(exc)
    else:
        raise AssertionError("ambiguous family under --yes must demand --board")
    SETUP.input = _Input("1")
    try:
        assert SETUP.prompt_family("/dev/cu.usbmodem1", assume_yes=False) == SETUP.FAMILY_SOLIDE
    finally:
        del SETUP.input


def test_friendly_name_prefers_specific_product():
    assert SETUP.friendly_name(SETUP.FAMILY_SOLIDE, "") == "Nimbus board"
    assert SETUP.friendly_name(SETUP.FAMILY_FREENOVE, "USB Single Serial") == "Freenove CYD"
    assert SETUP.friendly_name(SETUP.FAMILY_SOLIDE, "Nimbus-4") == "Nimbus-4"


# ---- native USB accepted for every board ----------------------------------


def test_native_usb_ports_are_accepted_for_any_board():
    # The old hard rejection is gone: an explicit native-USB --port only fails on
    # existence now, never on being "the native port".
    args = _args(port="/dev/cu.usbmodem-does-not-exist")
    try:
        SETUP.resolve_port(args)
    except RuntimeError as exc:
        assert "does not exist" in str(exc)
        assert "native" not in str(exc).lower()
    else:
        raise AssertionError("a nonexistent port must still fail on existence")


# ---- seeding: e-ink gone, otaType added ------------------------------------


def test_bootstrap_commands_seed_display_mode_and_ota_type():
    # Solide TFT board: scrModel + the 180-degree flip + mode + typed-OTA slug.
    assert SETUP.bootstrap_commands("tft", "orchestrator", "solide_s3", "nimbus-tft") == [
        ("SET scrModel=tft", "SET scrModel ok=1"),
        ("SETI tftFlip=1", "SETI tftFlip=1 ok=1"),
        ("SETI nimbus_mode=1", "SETI nimbus_mode=1 ok=1"),
        ("SET otaType=nimbus-tft", "SET otaType ok=1"),
    ]
    # Freenove: no tftFlip (it owns its orientation), and a freenove-* slug.
    assert SETUP.bootstrap_commands("tft", "notifier", "freenove_s3", "freenove-35") == [
        ("SET scrModel=tft", "SET scrModel ok=1"),
        ("SETI nimbus_mode=0", "SETI nimbus_mode=0 ok=1"),
        ("SET otaType=freenove-35", "SET otaType ok=1"),
    ]


def test_freenove_ota_type_from_size_flag_and_default():
    assert SETUP.freenove_ota_type(_args(size="28")) == "freenove-28"
    assert SETUP.freenove_ota_type(_args(size="40")) == "freenove-40"
    # --yes with no size and nothing saved falls back to the base panel, with a note.
    assert SETUP.freenove_ota_type(_args(yes=True)) == "freenove-28"
    # Interactive prompt.
    SETUP.input = _Input("2")
    try:
        assert SETUP.freenove_ota_type(_args()) == "freenove-35"
    finally:
        del SETUP.input


def test_freenove_ota_type_keeps_a_configured_boards_saved_panel():
    # A configured board already carries its slug: --yes with no --size keeps it instead
    # of re-typing a 3.5"/4.0" board to the 2.8" default (and so flashing the 2.8" image).
    assert SETUP.freenove_ota_type(_args(yes=True), saved="freenove-40") == "freenove-40"
    # Interactive runs keep it too, without asking again.
    SETUP.input = _Input()  # any prompt would pop from an empty list and fail
    try:
        assert SETUP.freenove_ota_type(_args(), saved="freenove-35") == "freenove-35"
    finally:
        del SETUP.input
    # An explicit --size still wins over the saved slug.
    assert SETUP.freenove_ota_type(_args(size="28"), saved="freenove-40") == "freenove-28"


def test_freenove_ota_type_prompt_rejects_out_of_range_answers():
    for answer in ("0", "4", "x", "", "\u00b2", "01"):  # "\u00b2" passes str.isdigit() but not int()
        SETUP.input = _Input(answer)
        try:
            SETUP.freenove_ota_type(_args())
        except RuntimeError as exc:
            assert "display size" in str(exc)
        else:
            raise AssertionError(f"answer {answer!r} must not pick a display size")
        finally:
            del SETUP.input


def test_freenove_ota_type_refuses_to_guess_for_a_configured_board_under_yes():
    try:
        SETUP.freenove_ota_type(_args(yes=True), saved=None, configured=True)
    except RuntimeError as exc:
        assert "--size" in str(exc)
    else:
        raise AssertionError("a configured Freenove with no readable size must not be re-typed to 2.8")


def test_freenove_slug_comes_only_from_the_live_ota_type_entry():
    for slug in SETUP.FREENOVE_SLUGS:
        assert SETUP.freenove_slug_from_nvs(_partition(_nvs_page(("otaType", slug, "written")))) == slug
    # The erased older value stays in flash next to the live one: only the live one counts.
    resized = _nvs_page(("otaType", "freenove-28", "erased"), ("otaType", "freenove-35", "written"))
    assert SETUP.freenove_slug_from_nvs(_partition(resized)) == "freenove-35"
    # Free text elsewhere in NVS (a device name, a network name) is never the setting.
    named = _nvs_page(("devName", "freenove-40", "written"), ("staSsid", "freenove-35", "written"))
    assert SETUP.freenove_slug_from_nvs(_partition(named)) is None
    assert SETUP.family_from_nvs(_partition(named)) is None
    solide = _nvs_page(("otaType", "nimbus-tft", "written"), ("devName", "freenove-40", "written"))
    assert SETUP.freenove_slug_from_nvs(_partition(solide)) is None
    assert SETUP.family_from_nvs(_partition(solide)) == SETUP.FAMILY_SOLIDE
    # Two live values that disagree: ask, never guess.
    split = _partition(
        _nvs_page(("otaType", "freenove-28", "written")), _nvs_page(("otaType", "freenove-40", "written"))
    )
    assert SETUP.freenove_slug_from_nvs(split) is None
    assert SETUP.freenove_slug_from_nvs(_partition()) is None


def test_prompt_bootstrap_solide_and_freenove():
    # Solide fresh board: display seeded to tft.
    assert SETUP.prompt_bootstrap(_args(mode="orchestrator"), "blank", SETUP.FAMILY_SOLIDE) == ("tft", "orchestrator")
    # A configured Solide keeps its saved display (None).
    assert SETUP.prompt_bootstrap(_args(mode="notifier"), "nimbus", SETUP.FAMILY_SOLIDE) == (None, "notifier")
    # Freenove derives display tft (its panel slug comes from board_ota_type).
    assert SETUP.prompt_bootstrap(_args(mode="orchestrator", size="40"), "blank", SETUP.FAMILY_FREENOVE) == (
        "tft",
        "orchestrator",
    )


def test_board_ota_type_solide_is_fixed_freenove_follows_the_panel():
    assert SETUP.board_ota_type(_args(size="40"), SETUP.FAMILY_SOLIDE, "blank", None) == "nimbus-tft"
    assert SETUP.board_ota_type(_args(size="35"), SETUP.FAMILY_FREENOVE, "blank", None) == "freenove-35"
    assert SETUP.board_ota_type(_args(yes=True), SETUP.FAMILY_FREENOVE, "nimbus", "freenove-40") == "freenove-40"
    assert SETUP.board_ota_type(_args(yes=True), SETUP.FAMILY_FREENOVE, "blank", None) == "freenove-28"


def test_prompt_mode_yes_requires_mode_on_blank_board():
    try:
        SETUP.prompt_mode(_args(yes=True), known_nimbus=False)
    except RuntimeError as exc:
        assert "--mode" in str(exc)
    else:
        raise AssertionError("a blank board under --yes must demand a mode")
    # A configured board may keep its saved mode.
    assert SETUP.prompt_mode(_args(yes=True), known_nimbus=True) is None


# ---- identify-and-confirm --------------------------------------------------


def test_pick_port_single_and_ambiguous():
    one = [{"port": "/dev/cu.usbmodem1", "vid": SETUP.VID_ESP32S3_NATIVE, "pid": 1, "product": ""}]
    assert SETUP.pick_port(one, assume_yes=False)["port"] == "/dev/cu.usbmodem1"
    # Empty -> clear error.
    try:
        SETUP.pick_port([], assume_yes=False)
    except RuntimeError as exc:
        assert "No board" in str(exc)
    else:
        raise AssertionError("no boards must raise")
    # Several under --yes is ambiguous.
    two = one + [{"port": "/dev/cu.usbserial-9", "vid": SETUP.VID_CP210X, "pid": 2, "product": ""}]
    try:
        SETUP.pick_port(two, assume_yes=True)
    except RuntimeError as exc:
        assert "--port" in str(exc)
    else:
        raise AssertionError("--yes with several boards must demand --port")


def test_pick_port_identify_then_choose():
    two = [
        {"port": "/dev/cu.usbmodem1", "vid": SETUP.VID_ESP32S3_NATIVE, "pid": 1, "product": ""},
        {"port": "/dev/cu.usbserial-9", "vid": SETUP.VID_CP210X, "pid": 2, "product": ""},
    ]
    identified = []
    SETUP.input = _Input("i2", "1")  # identify board 2, then choose board 1
    try:
        # Restore, never delete, the real identify: later tests call it.
        with _patched(identify=lambda port: identified.append(port) or True):
            chosen = SETUP.pick_port(two, assume_yes=False)
    finally:
        del SETUP.input
    assert identified == ["/dev/cu.usbserial-9"]
    assert chosen["port"] == "/dev/cu.usbmodem1"


def test_confirm_install_yes_and_decline():
    SETUP.confirm_install("Nimbus-4", SETUP.FAMILY_SOLIDE, True, "/dev/cu.usbserial-1", assume_yes=True)
    SETUP.input = _Input("")  # bare Enter == yes
    try:
        SETUP.confirm_install("Nimbus-4", SETUP.FAMILY_SOLIDE, True, "/dev/cu.x", assume_yes=False)
    finally:
        del SETUP.input
    SETUP.input = _Input("n")
    try:
        SETUP.confirm_install("blank", SETUP.FAMILY_FREENOVE, False, "/dev/cu.x", assume_yes=False)
    except RuntimeError as exc:
        assert "Cancelled" in str(exc)
    else:
        raise AssertionError("answering no must cancel")
    finally:
        del SETUP.input


# ---- post-flash panel-responds check (CUM-388) ----------------------------
#
# A wrong-variant flash (a Solide image on a Freenove) boots network-healthy with a
# dead screen: scrModel verifies while the panel controller never answers. The
# restored firmware emits a one-shot boot signal (PANEL scrok=0/1 + a loud human
# line); these pin the parser that fails the install instead of printing "installed"
# over black glass. A fake serial stands in for the board so no hardware is needed.


class _FakeSerial:
    """A pyserial-shaped stub: readline() pops scripted byte lines, b'' when done."""

    def __init__(self, *lines):
        self.lines = [ln if isinstance(ln, bytes) else ln.encode() for ln in lines]

    def readline(self):
        return self.lines.pop(0) if self.lines else b""


def test_read_panel_signal_dead_on_scrok_zero_token():
    # scrok=0 is decisive the moment it appears - a later lying "scrok=1" cannot undo it.
    conn = _FakeSerial("[boot] wifi up", "PANEL scrok=0", "PANEL scrok=1")
    assert SETUP.read_panel_signal(conn, timeout=5.0) == "dead"


def test_read_panel_signal_dead_on_loud_human_line():
    conn = _FakeSerial(
        "!! DISPLAY NOT RESPONDING - likely the WRONG board variant was flashed; reflash via tools/setup_device.py",
        "PANEL scrok=0",
    )
    assert SETUP.read_panel_signal(conn, timeout=5.0) == "dead"


def test_read_panel_signal_dead_on_init_failed_line():
    conn = _FakeSerial(
        "!! DISPLAY DID NOT COME UP - panel init failed; check the display or reflash via tools/setup_device.py"
    )
    assert SETUP.read_panel_signal(conn, timeout=5.0) == "dead"


def test_read_panel_signal_ok_on_scrok_one():
    conn = _FakeSerial("[disp] colour touch panel up", "PANEL scrok=1")
    assert SETUP.read_panel_signal(conn, timeout=5.0) == "ok"


def test_read_panel_signal_ok_from_full_status_line():
    # The console-build STATUS line carries scrok=1 too; the same parser reads it,
    # and the neighbouring scr=tft must NOT be misread as the panel token.
    status = "STATUS fw=x mode=1 wifi=1 ip=192.168.50.61 rssi=-50 heap=1000 scr=tft scrok=1 want=tft board=freenove_s3"
    assert SETUP.read_panel_signal(_FakeSerial(status), timeout=5.0) == "ok"


def test_read_panel_signal_unverified_on_scrok_unknown_token():
    # CUM-423: a shared-MISO board honestly reports liveness as unknown. That is a
    # soft caution ("unverified"), never a pass ("ok") and never a failure ("dead").
    conn = _FakeSerial("[boot] wifi up", "PANEL scrok=unknown")
    assert SETUP.read_panel_signal(conn, timeout=5.0) == "unverified"


def test_read_panel_signal_unverified_on_loud_unknown_line():
    conn = _FakeSerial(
        "?? DISPLAY LIVENESS UNKNOWN on this board; touch shares the panel bus, so it cannot self-check. Look at the screen.",
        "PANEL scrok=unknown",
    )
    assert SETUP.read_panel_signal(conn, timeout=5.0) == "unverified"


def test_read_panel_signal_dead_wins_over_unknown():
    # A real not-responding fault is decisive even if an "unknown" line follows: the
    # softer caution must never override a genuine dead-panel signal.
    conn = _FakeSerial("PANEL scrok=0", "PANEL scrok=unknown")
    assert SETUP.read_panel_signal(conn, timeout=5.0) == "dead"


def test_read_panel_signal_unverified_from_full_status_line():
    # The shared-MISO console STATUS line carries scrok=unknown; the same parser reads
    # it, and neighbouring scr=tft is not misread as the panel token.
    status = (
        "STATUS fw=x mode=1 wifi=1 ip=192.168.50.61 rssi=-50 heap=1000 scr=tft scrok=unknown want=tft board=solide_s3"
    )
    assert SETUP.read_panel_signal(_FakeSerial(status), timeout=5.0) == "unverified"


def test_read_panel_signal_undetermined_is_none():
    # Non-decisive boot chatter then silence -> None (the installer treats it as a
    # soft 'unknown', never a false failure). Exercises the read-then-timeout path.
    conn = _FakeSerial("[boot] wifi up", "READY mode=1 ip=192.168.50.61")
    assert SETUP.read_panel_signal(conn, timeout=0.3) is None
    # An empty stream within the window is likewise undetermined, not dead.
    assert SETUP.read_panel_signal(_FakeSerial(), timeout=0.0) is None


def test_panel_dead_message_points_at_the_guarded_installer():
    msg = SETUP.PANEL_DEAD_MESSAGE
    assert "tools/setup_device.py" in msg
    # It steers the operator away from a raw pio upload (the way the incident happened).
    assert "raw" in msg and "pio" in msg


def test_skip_panel_check_flag_parses():
    assert SETUP.parse_args(["--port", "/dev/cu.x", "--skip-panel-check"]).skip_panel_check is True
    assert SETUP.parse_args(["--port", "/dev/cu.x"]).skip_panel_check is False


def test_show_token_and_ota_type_args_plumb_through():
    args = SETUP.parse_args(["--port", "/dev/cu.usbserial-test", "--show-token"])
    assert args.show_token is True and args._show_token is False
    assert args.board is None  # auto-detect by default
    sub = SETUP.parse_args(["--_bootstrap-port", "/dev/x", "--board", "freenove_s3", "--_ota-type", "freenove-28"])
    assert sub._ota_type == "freenove-28" and sub.board == "freenove_s3"


# ---- CUM-388 (lane I): prov-env by family+transport (D2) + failure-path check (D1) ----
#
# D2: the provisioning sketch must be chosen by board family AND transport, because
# its Serial has to answer over the same wire the tool reads. A Solide on native USB
# given provision-uart (CDC off, Serial=UART0) can never reply, which is what made a
# blank Solide unsetupable over native USB and made the wrong-variant Freenove case
# fail at bootstrap instead of at the screen check. D1: a failed bootstrap must still
# read the boot screen signal and show the wrong-variant message when the screen is
# dead, on every failure path, without stranding the board or waiting 20 s on a Ctrl-C.


class _RecordingRunner:
    """Stand in for run_checked: record the env of every `pio run -e <env> ... upload`
    and optionally raise, so a full main() run needs no PlatformIO and no board.

    fail_env: raise CalledProcessError from the upload of this env (a failed flash).
    fail_ack: an exception to raise from the esptool bootstrap-acknowledge call (a
    silent board / a Ctrl-C during bootstrap)."""

    def __init__(self, fail_env=None, fail_ack=None):
        self.calls: list[list[str]] = []
        self.envs: list[str] = []
        self.panel_calls: list[str] = []
        self.probe_calls: list[str] = []
        self.fail_env = fail_env
        self.fail_ack = fail_ack

    def __call__(self, command, capture=False):
        self.calls.append(list(command))
        if "--_bootstrap-port" in command and self.fail_ack is not None:
            raise self.fail_ack
        if "run" in command and "-e" in command and "upload" in command:
            env = command[command.index("-e") + 1]
            self.envs.append(env)
            if env == self.fail_env:
                raise subprocess.CalledProcessError(1, command)
        return subprocess.CompletedProcess(command, 0, "", "")


def _board(port="/dev/cu.usbmodem101", vid=None, product="", nvs_state="blank", nvs_family=None, nvs_slug=None):
    """A discovered-board description for _run_main (what resolve_port/inspect_board
    would report)."""
    return dict(port=port, vid=vid, product=product, nvs_state=nvs_state, nvs_family=nvs_family, nvs_slug=nvs_slug)


def _run_main(argv, board=None, panel="unknown", runner=None, probe="clear"):
    """Drive SETUP.main(argv) fully host-side: stub discovery, flashing, the serial
    screen read, and the board-family probe so no hardware or PlatformIO is touched.
    Returns (rc, stdout, stderr, runner). The runner records which envs were uploaded,
    runner.panel_calls records whether the screen was read, and runner.probe_calls
    records whether the board-family probe ran. ``probe`` defaults to 'clear' (a genuine
    solide) so existing installs proceed; set 'freenove' to exercise the CUM-422 refusal."""
    board = board if board is not None else _board()
    runner = runner if runner is not None else _RecordingRunner()
    saved: dict[str, object] = {}

    def _patch(name, value):
        saved[name] = getattr(SETUP, name)
        setattr(SETUP, name, value)

    def _fake_verify(p, timeout=20.0):
        runner.panel_calls.append(p)
        return panel

    def _fake_probe(p, timeout=8.0):
        runner.probe_calls.append(p)
        if isinstance(probe, BaseException):
            raise probe
        return probe

    _patch("resolve_port", lambda args: (board["port"], board["vid"], board["product"]))
    _patch("platformio_executable", lambda: (Path("/tmp/core"), "pio"))
    _patch("esptool_command", lambda core: ["python", "esptool.py"])
    _patch(
        "inspect_board",
        lambda esptool, p: ("aa:bb:cc:dd:ee:ff", board["nvs_state"], board["nvs_family"], board["nvs_slug"]),
    )
    # pyserial presence is its own test (CUM-487, below); here the in-process main()
    # carries on in this interpreter whether or not the host has pyserial.
    _patch("ensure_pyserial", lambda argv: None)
    _patch("run_checked", runner)
    _patch("verify_panel_after_flash", _fake_verify)
    _patch("probe_for_freenove", _fake_probe)
    out, err = io.StringIO(), io.StringIO()
    try:
        with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
            rc = SETUP.main(argv)
    finally:
        for name, value in saved.items():
            setattr(SETUP, name, value)
    return rc, out.getvalue(), err.getvalue(), runner


def _outcome(**kw):
    o = SETUP.InstallOutcome()
    for key, value in kw.items():
        setattr(o, key, value)
    return o


# ---- D2: transport-aware provisioning-env selection ------------------------


def test_transport_of_uses_vid_then_port_name_then_fails_closed():
    assert SETUP.transport_of(SETUP.VID_CP210X) == "bridge"
    assert SETUP.transport_of(SETUP.VID_CH34X) == "bridge"
    assert SETUP.transport_of(SETUP.VID_ESP32S3_NATIVE) == "native"
    # No descriptor (an explicit --port): the device-node name still carries it.
    assert SETUP.transport_of(None, "/dev/cu.usbmodem2101") == "native"
    assert SETUP.transport_of(None, "/dev/ttyACM0") == "native"
    assert SETUP.transport_of(None, "/dev/cu.usbserial-1420") == "bridge"
    assert SETUP.transport_of(None, "/dev/ttyUSB0") == "bridge"
    # Neither a VID nor a transport-bearing name -> None, so the caller fails closed.
    assert SETUP.transport_of(None, "/dev/cu.mystery") is None
    assert SETUP.transport_of(None, "") is None


def test_provision_env_solide_native_usb_is_not_the_uart_sketch():
    # The D2 defect in one line: a Solide on native USB must NOT get provision-uart.
    assert SETUP.provision_env(SETUP.FAMILY_SOLIDE, SETUP.VID_ESP32S3_NATIVE) == "provision"
    assert SETUP.provision_env(SETUP.FAMILY_SOLIDE, SETUP.VID_CP210X) == "provision-uart"
    assert SETUP.provision_env(SETUP.FAMILY_FREENOVE, SETUP.VID_ESP32S3_NATIVE) == "provision-cyd"
    # Unknown VID falls back to the transport the device-node name implies.
    assert SETUP.provision_env(SETUP.FAMILY_SOLIDE, None, "/dev/cu.usbmodem9") == "provision"


def test_prov_and_prod_env_table_over_family_x_transport_x_nvs():
    # Class table (charter: test the class, not the instance). Every family in
    # FAMILY_NAME and every transport must have a row; a new family or transport
    # added with no row FAILS here rather than picking a sketch unnoticed.
    FAIL = object()
    transports = {
        "bridge": SETUP.VID_CP210X,
        "native": SETUP.VID_ESP32S3_NATIVE,
        "unknown": None,  # explicit --port, no descriptor, non-transport-bearing name
    }
    nvs_states = ("blank", "nimbus", "other")
    # The production image of a Freenove depends on its panel too; this table pins the
    # 2.8" row, test_every_freenove_panel_* covers every size.
    expected = {
        (SETUP.FAMILY_FREENOVE, "bridge"): ("provision-cyd", "esp32s3-cyd"),
        (SETUP.FAMILY_FREENOVE, "native"): ("provision-cyd", "esp32s3-cyd"),
        (SETUP.FAMILY_FREENOVE, "unknown"): ("provision-cyd", "esp32s3-cyd"),
        (SETUP.FAMILY_SOLIDE, "bridge"): ("provision-uart", "esp32s3"),
        (SETUP.FAMILY_SOLIDE, "native"): ("provision", "esp32s3"),
        (SETUP.FAMILY_SOLIDE, "unknown"): (FAIL, "esp32s3"),
    }
    # A new family with no row must fail this test (not silently pass uncovered).
    for family in SETUP.FAMILY_NAME:
        assert any(fam == family for (fam, _t) in expected), f"no prov-env rows for family {family!r}"
    # A new transport with no row must fail this test too.
    for transport in transports:
        assert any(tr == transport for (_f, tr) in expected), f"no prov-env rows for transport {transport!r}"

    for (family, transport), (want_prov, want_prod) in expected.items():
        vid = transports[transport]
        node = "/dev/cu.mystery" if transport == "unknown" else "/dev/cu.usbX"
        for nvs in nvs_states:  # transport is independent of NVS; assert it holds for all
            slug = "freenove-28" if family == SETUP.FAMILY_FREENOVE else "nimbus-tft"
            assert SETUP.production_env(family, slug) == want_prod, (family, transport, nvs)
            if want_prov is FAIL:
                try:
                    SETUP.provision_env(family, vid, node)
                except RuntimeError as exc:
                    assert "Could not tell how" in str(exc)
                else:
                    raise AssertionError(f"unknown transport must fail closed for {family!r}")
            else:
                assert SETUP.provision_env(family, vid, node) == want_prov, (family, transport, nvs)


def test_production_and_provision_env_reject_unknown_family():
    for fn in (lambda f: SETUP.production_env(f, None), lambda f: SETUP.provision_env(f, SETUP.VID_CP210X)):
        try:
            fn("mystery_board")
        except RuntimeError as exc:
            assert "mystery_board" in str(exc)
        else:
            raise AssertionError("an unknown family must raise, never default to a sketch")


def test_main_blank_solide_native_usb_uploads_native_provision_sketch():
    # End-to-end wiring (D2): a blank Solide on native USB, given --board solide_s3,
    # must upload the native-USB provision sketch so the bootstrap can be
    # acknowledged. On base 0bdb3cb this uploads provision-uart and the assertion
    # below fails (non-tautological).
    rc, out, err, runner = _run_main(
        ["--yes", "--port", "/dev/cu.usbmodem101", "--board", "solide_s3", "--mode", "orchestrator"],
        board=_board(vid=SETUP.VID_ESP32S3_NATIVE),
        panel="ok",
    )
    assert rc == 0, (rc, err)
    assert runner.envs[0] == "provision"  # the setup sketch uploaded first
    assert "provision-uart" not in runner.envs
    assert "esp32s3" in runner.envs  # production installed


# ---- D1: the failure path still reads the screen ---------------------------


def test_finish_install_decision_matrix():
    dead = "WRONG board variant"  # the loud line inside PANEL_DEAD_MESSAGE
    success = "installed. NVS was not erased"

    def run(outcome, panel):
        out, err = io.StringIO(), io.StringIO()
        with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
            rc = SETUP.finish_install(outcome, panel, "orchestrator", False)
        return rc, (out.getvalue() + err.getvalue())

    # (outcome kwargs, panel signal, expected rc, must-appear, must-NOT-appear).
    # The success line must appear only on a clean install; the wrong-variant
    # message must appear exactly when the screen reads dead.
    cases = [
        (dict(production_on_board=True), "ok", 0, [success], [dead]),
        (dict(production_on_board=True), "unknown", 0, [success, "look at the screen"], []),
        # CUM-423: a shared-MISO board that cannot self-check its panel is a SOFT
        # caution, never a hard failure - install proceeds (rc 0) with the success line
        # and a nudge to look at the glass, and never the wrong-variant message.
        (dict(production_on_board=True), "unverified", 0, [success, "look at the screen"], [dead]),
        (dict(production_on_board=True), "dead", 1, [dead], [success]),
        (dict(production_on_board=True, bootstrap_error=RuntimeError("x")), "dead", 1, [dead], [success]),
        (
            dict(production_on_board=True, bootstrap_error=RuntimeError("x")),
            "ok",
            1,
            ["variant is correct"],
            [success, dead],
        ),
        (dict(restore_error=RuntimeError("x")), "dead", 1, ["could not be restored"], [success, dead]),
        (dict(interrupted=True, production_on_board=True), "unknown", 1, ["interrupted"], [success]),
        # CUM-422: a probe-refused Freenove is decisive first - non-zero, the wrong-variant
        # message, and never the success line, whatever the (unused) screen signal says.
        (dict(wrong_variant=True), "unknown", 1, ["Freenove capacitive touch"], [success, dead]),
        (dict(wrong_variant=True), "ok", 1, ["Freenove capacitive touch"], [success]),
    ]
    for kwargs, panel, want_rc, must, forbid in cases:
        rc, text = run(_outcome(**kwargs), panel)
        low = text.lower()
        assert rc == want_rc, (kwargs, panel, rc)
        for needle in must:
            assert needle.lower() in low, (kwargs, needle)
        for needle in forbid:
            assert needle.lower() not in low, (kwargs, needle)


def test_main_bootstrap_no_reply_with_dead_screen_shows_wrong_variant():
    # CUM-388 D1 core: the setup step fails (a silent board) AND the restored image
    # reports a dead screen -> the operator gets the wrong-variant message and a
    # non-zero exit. On base 0bdb3cb main() returns 1 before the panel check runs,
    # so PANEL_DEAD_MESSAGE never appears and this fails.
    runner = _RecordingRunner(fail_ack=subprocess.CalledProcessError(1, "ack"))
    rc, out, err, runner = _run_main(
        ["--yes", "--port", "/dev/cu.usbmodem101", "--board", "solide_s3", "--mode", "orchestrator"],
        board=_board(vid=SETUP.VID_ESP32S3_NATIVE),
        panel="dead",
        runner=runner,
    )
    assert rc == 1, (rc, out, err)
    assert "WRONG board variant" in err
    assert "installed. NVS was not erased" not in out  # success line must NOT print
    assert runner.envs[-1] == "esp32s3"  # production restored, board not stranded
    assert runner.panel_calls == ["/dev/cu.usbmodem101"]  # the screen was actually read


def test_main_bootstrap_upload_failure_still_checks_screen():
    # The prov upload itself failing is a bootstrap failure too: restore runs, the
    # screen is read, a dead screen yields the wrong-variant message and exit 1.
    runner = _RecordingRunner(fail_env="provision")
    rc, out, err, runner = _run_main(
        ["--yes", "--port", "/dev/cu.usbmodem101", "--board", "solide_s3", "--mode", "orchestrator"],
        board=_board(vid=SETUP.VID_ESP32S3_NATIVE),
        panel="dead",
        runner=runner,
    )
    assert rc == 1 and "WRONG board variant" in err
    assert runner.envs[-1] == "esp32s3"
    assert runner.panel_calls == ["/dev/cu.usbmodem101"]


def test_main_ctrl_c_during_bootstrap_does_not_wait_on_serial():
    # A Ctrl-C must not tack a 20 s screen read onto the interrupt: the panel check
    # is skipped, production is restored, exit is non-zero.
    runner = _RecordingRunner(fail_ack=KeyboardInterrupt())
    rc, out, err, runner = _run_main(
        ["--yes", "--port", "/dev/cu.usbmodem101", "--board", "solide_s3", "--mode", "orchestrator"],
        board=_board(vid=SETUP.VID_ESP32S3_NATIVE),
        panel="dead",
        runner=runner,
    )
    assert rc == 1
    assert runner.panel_calls == []  # the screen was NOT read on a Ctrl-C
    assert runner.envs[-1] == "esp32s3"  # production restored anyway
    assert "installed. NVS was not erased" not in out


def test_main_restore_failure_reports_and_skips_screen_read():
    # If production firmware cannot be put back, say so and do not read the screen
    # (there is no trustworthy production image to signal); exit non-zero.
    runner = _RecordingRunner(fail_env="esp32s3")
    rc, out, err, runner = _run_main(
        ["--yes", "--port", "/dev/cu.usbmodem101", "--board", "solide_s3", "--mode", "orchestrator"],
        board=_board(vid=SETUP.VID_ESP32S3_NATIVE),
        panel="ok",
        runner=runner,
    )
    assert rc == 1
    assert runner.panel_calls == []
    assert "could not be restored" in err


def test_main_wrong_variant_freenove_given_solide_board_caught_by_screen():
    # The exact incident: a blank Freenove flashed with --board solide_s3. With the
    # transport fix the setup sketch answers over native USB, so the run reaches the
    # production flash; the dead screen from the wrong pinout is then caught by the
    # post-flash screen check with the wrong-variant message and a non-zero exit. The
    # env assertion below is non-tautological (base uploads provision-uart).
    runner = _RecordingRunner()
    rc, out, err, runner = _run_main(
        ["--yes", "--port", "/dev/cu.usbmodem101", "--board", "solide_s3", "--mode", "orchestrator"],
        board=_board(vid=SETUP.VID_ESP32S3_NATIVE),
        panel="dead",
        runner=runner,
    )
    assert rc == 1 and "WRONG board variant" in err
    assert "installed. NVS was not erased" not in out
    assert runner.envs == ["provision", "esp32s3"]


def test_main_happy_path_solide_native_installs_and_reports_success():
    # A correct install on a Solide over native USB: setup sketch answers, production
    # flashes, the screen check passes, exit 0 with the success line.
    rc, out, err, runner = _run_main(
        ["--yes", "--port", "/dev/cu.usbmodem101", "--board", "solide_s3", "--mode", "orchestrator"],
        board=_board(vid=SETUP.VID_ESP32S3_NATIVE),
        panel="ok",
    )
    assert rc == 0
    assert runner.envs == ["provision", "esp32s3"]
    assert "installed. NVS was not erased" in out
    assert "Screen check passed" in out


def test_serial_bootstrap_no_reply_message_is_operator_language():
    # Point 4: a truly silent board says so in operator terms, not by quoting the
    # wire token. A fake pyserial that never returns a matching line drives the
    # no-reply path; the NoReplyError message names the port and the likely cause.
    import types

    class _SilentSerial:
        def __init__(self):
            self.port = None
            self.baudrate = None
            self.dtr = None
            self.rts = None
            self.timeout = None
            self.write_timeout = None
            self.is_open = False

        def open(self):
            self.is_open = True

        def close(self):
            self.is_open = False

        def reset_input_buffer(self):
            pass

        def write(self, _data):
            pass

        def flush(self):
            pass

        def readline(self):
            return b""  # never answers

    fake_serial = types.ModuleType("serial")
    fake_serial.Serial = lambda *a, **k: _SilentSerial()
    import sys as _sys

    class _FastClock:
        # Skip the real 3 s boot wait and jump the command deadline so a silent
        # board resolves instantly instead of spinning out the real 4 s timeout.
        def __init__(self):
            self._t = 0.0

        def monotonic(self):
            self._t += 0.5
            return self._t

        def sleep(self, _s):
            pass

    saved = _sys.modules.get("serial")
    saved_time = SETUP.time
    _sys.modules["serial"] = fake_serial
    SETUP.time = _FastClock()
    err = io.StringIO()
    try:
        with contextlib.redirect_stderr(err), contextlib.redirect_stdout(io.StringIO()):
            rc = SETUP.serial_bootstrap(
                "/dev/cu.usbmodem7", "tft", "orchestrator", board="solide_s3", ota_type="nimbus-tft"
            )
    finally:
        SETUP.time = saved_time
        if saved is None:
            del _sys.modules["serial"]
        else:
            _sys.modules["serial"] = saved
    assert rc == 1
    text = err.getvalue()
    assert "/dev/cu.usbmodem7" in text
    assert "wrong board choice" in text or "other USB port" in text
    assert "SET scrModel" not in text  # never quotes the wire token to the operator


# ---- CUM-422: board-family probe (FT6336U) refuses a wrong-variant flash ----
#
# CUM-388 caught a wrong-variant flash only on a BLANK NVS; once the installer seeded
# scrModel=tft the Solide image read the panel as healthy on the Freenove and passed.
# The probe asks the physical board: the provision sketch's PROBE scans the Freenove
# FT6336U at I2C 0x38, and a Freenove is refused a Solide flash regardless of --board or
# NVS. A fake serial stands in for the board.


def test_read_probe_signal_freenove_when_ft6336_present():
    conn = _FakeSerial("[boot] provision ready", "PROBE ft6336=1 addr=0x38")
    assert SETUP.read_probe_signal(conn, timeout=5.0) == "freenove"


def test_read_probe_signal_clear_when_ft6336_absent():
    # A genuine Solide: nothing answers at 0x38 on those pins -> proceed.
    conn = _FakeSerial("[boot] provision ready", "PROBE ft6336=0 addr=0x38")
    assert SETUP.read_probe_signal(conn, timeout=5.0) == "clear"


def test_read_probe_signal_none_on_old_sketch_without_probe():
    # An old provision sketch never answers PROBE -> None (installer treats as unsupported).
    conn = _FakeSerial("[boot] provision ready", "PROVISION READY")
    assert SETUP.read_probe_signal(conn, timeout=0.3) is None
    assert SETUP.read_probe_signal(_FakeSerial(), timeout=0.0) is None


def test_probe_wrong_variant_message_names_the_board_and_next_step():
    msg = SETUP.PROBE_WRONG_VARIANT_MESSAGE
    assert "Freenove" in msg
    assert "--board freenove_s3" in msg  # the corrected command
    assert "Refusing" in msg


def test_main_freenove_under_solide_board_refused_by_probe():
    # The exact CUM-422 incident: --board solide_s3 on a Freenove. The probe answers
    # ft6336=1 -> the solide flash is REFUSED before seeding or any production write:
    # non-zero exit, wrong-variant message, and ONLY the provision sketch was uploaded
    # (no esp32s3), so the NVS that fakes a healthy panel is never seeded.
    rc, out, err, runner = _run_main(
        ["--yes", "--port", "/dev/cu.usbmodem101", "--board", "solide_s3", "--mode", "orchestrator"],
        board=_board(vid=SETUP.VID_ESP32S3_NATIVE),
        probe="freenove",
    )
    assert rc == 1, (rc, out, err)
    assert "Freenove capacitive touch" in err
    assert "installed. NVS was not erased" not in out  # nothing installed
    assert runner.envs == ["provision"]  # only the setup sketch; no esp32s3
    assert runner.probe_calls == ["/dev/cu.usbmodem101"]  # the probe actually ran
    assert runner.panel_calls == []  # no production image to screen-check


def test_main_solide_probe_clear_proceeds_and_installs():
    # A genuine Solide on native USB: the probe answers ft6336=0 (clear) -> the install
    # proceeds through seeding and the production flash exactly as before.
    rc, out, err, runner = _run_main(
        ["--yes", "--port", "/dev/cu.usbmodem101", "--board", "solide_s3", "--mode", "orchestrator"],
        board=_board(vid=SETUP.VID_ESP32S3_NATIVE),
        panel="ok",
        probe="clear",
    )
    assert rc == 0, (rc, err)
    assert runner.envs == ["provision", "esp32s3"]
    assert runner.probe_calls == ["/dev/cu.usbmodem101"]
    # The clear verdict is said out loud, so an install log carries the evidence.
    assert "no Freenove touch controller found" in out
    assert "could not confirm this is a Nimbus board" not in out
    assert "installed. NVS was not erased" in out


def test_main_old_sketch_unsupported_probe_proceeds_with_caution():
    # An old provision sketch with no PROBE support -> the probe cannot rule out a
    # Freenove, so the install proceeds (with a caution), not a refusal.
    rc, out, err, runner = _run_main(
        ["--yes", "--port", "/dev/cu.usbmodem101", "--board", "solide_s3", "--mode", "orchestrator"],
        board=_board(vid=SETUP.VID_ESP32S3_NATIVE),
        panel="ok",
        probe="unsupported",
    )
    assert rc == 0, (rc, err)
    assert runner.envs == ["provision", "esp32s3"]
    assert "could not confirm this is a Nimbus board" in out
    assert "installed. NVS was not erased" in out


def test_main_probe_unknown_serial_error_proceeds_with_caution():
    # A serial hiccup during the probe -> 'unknown'. The install still proceeds (never
    # fail a legitimate solide install on a read glitch), but carries the same caution,
    # because the screen check is not a reliable backstop for a seeded Freenove.
    rc, out, err, runner = _run_main(
        ["--yes", "--port", "/dev/cu.usbmodem101", "--board", "solide_s3", "--mode", "orchestrator"],
        board=_board(vid=SETUP.VID_ESP32S3_NATIVE),
        panel="ok",
        probe="unknown",
    )
    assert rc == 0, (rc, err)
    assert runner.envs == ["provision", "esp32s3"]
    assert "could not confirm this is a Nimbus board" in out
    assert "installed. NVS was not erased" in out


def test_freenove_install_leaves_probe_skipped_no_caution():
    # A freenove install never probes, so outcome.probe stays 'skipped' and NO board-family
    # caution prints (the caution is only for an inconclusive probe on a solide flash).
    rc, out, err, runner = _run_main(
        ["--yes", "--port", "/dev/cu.usbmodem101", "--board", "freenove_s3", "--mode", "notifier", "--size", "28"],
        board=_board(vid=SETUP.VID_ESP32S3_NATIVE),
        panel="ok",
    )
    assert rc == 0, (rc, err)
    assert "could not confirm this is a Nimbus board" not in out


def test_freenove_install_does_not_probe():
    # A --board freenove_s3 install wants a Freenove, so detecting the FT6336U is expected,
    # not a refusal: the probe is scoped to solide flashes only and must not run here.
    rc, out, err, runner = _run_main(
        ["--yes", "--port", "/dev/cu.usbmodem101", "--board", "freenove_s3", "--mode", "notifier", "--size", "28"],
        board=_board(vid=SETUP.VID_ESP32S3_NATIVE),
        panel="ok",
        probe="freenove",  # even if the fake would answer freenove, it must not be consulted
    )
    assert rc == 0, (rc, err)
    assert runner.probe_calls == []  # never probed on a freenove install
    assert runner.envs == ["provision-cyd", "esp32s3-cyd"]
    assert "installed. NVS was not erased" in out


# ---- CUM-490: every Freenove panel size installs its own image ----------------------
#
# production_env() used to return esp32s3-cyd (the 2.8" image) for every Freenove, so
# `--size 35` seeded otaType=freenove-35 but flashed a picture sized for the wrong glass.
# Class tests: every row of FREENOVE_PANELS, every flow that writes production firmware
# (install and token recovery), and the release config table as the canonical mapping.


def _load_config_table():
    path = Path(__file__).with_name("release_gate") / "check_config_mapping.py"
    spec = importlib.util.spec_from_file_location("check_config_mapping_for_installer", path)
    module = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = module  # the frozen @dataclass resolves its module namespace
    try:
        spec.loader.exec_module(module)
    finally:
        sys.modules.pop(spec.name, None)
    return module.CONFIG_TABLE


def _acked_ota_type(runner):
    """The --_ota-type the bootstrap step was asked to seed (None if not seeded)."""
    for command in runner.calls:
        if "--_bootstrap-port" in command and "--_ota-type" in command:
            return command[command.index("--_ota-type") + 1]
    return None


def test_installer_image_mapping_matches_the_release_config_table():
    # The release gate's CONFIG_TABLE is the canonical type -> image table (CUM-388). The
    # installer must map every type to the same env, and offer no type the table lacks:
    # a fifth config added to the table without an installer row (or vice versa) fails.
    table = {c.type_slug: (c.env, c.is_freenove) for c in _load_config_table()}
    installer = {SETUP.OTA_TYPE_SOLIDE: (SETUP.production_env(SETUP.FAMILY_SOLIDE, SETUP.OTA_TYPE_SOLIDE), False)}
    for slug in SETUP.FREENOVE_SLUGS:
        installer[slug] = (SETUP.production_env(SETUP.FAMILY_FREENOVE, slug), True)
    assert installer == table


def test_size_flag_accepts_exactly_the_panel_rows():
    for size in SETUP.FREENOVE_PANELS:
        assert SETUP.parse_args(["--size", size]).size == size
    with contextlib.redirect_stderr(io.StringIO()):
        try:
            SETUP.parse_args(["--size", "50"])
        except SystemExit:
            pass
        else:
            raise AssertionError("a size with no panel row must be rejected")


def test_production_env_freenove_never_guesses_a_panel():
    for slug in (None, "", "freenove-50", "nimbus-tft"):
        try:
            SETUP.production_env(SETUP.FAMILY_FREENOVE, slug)
        except RuntimeError as exc:
            assert "--size" in str(exc)
        else:
            raise AssertionError(f"Freenove slug {slug!r} must fail closed, not fall back to the 2.8 image")


def test_every_freenove_panel_installs_its_own_image_and_seeds_its_own_type():
    for size, panel in SETUP.FREENOVE_PANELS.items():
        rc, out, err, runner = _run_main(
            ["--yes", "--port", "/dev/cu.usbmodem101", "--board", "freenove_s3", "--mode", "notifier", "--size", size],
            board=_board(vid=SETUP.VID_ESP32S3_NATIVE),
            panel="ok",
        )
        assert rc == 0, (size, rc, err)
        assert runner.envs == ["provision-cyd", panel.env], (size, runner.envs)
        assert _acked_ota_type(runner) == panel.slug, (size, runner.calls)


def test_every_freenove_panel_from_the_prompt_installs_its_own_image():
    for index, panel in enumerate(SETUP.FREENOVE_PANELS.values(), 1):
        SETUP.input = _Input(str(index), "")  # display size, then Enter at the confirm prompt
        try:
            rc, out, err, runner = _run_main(
                ["--port", "/dev/cu.usbmodem101", "--board", "freenove_s3", "--mode", "orchestrator"],
                board=_board(vid=SETUP.VID_ESP32S3_NATIVE),
                panel="ok",
            )
        finally:
            del SETUP.input
        assert rc == 0, (index, rc, err)
        assert runner.envs == ["provision-cyd", panel.env], (index, runner.envs)
        assert _acked_ota_type(runner) == panel.slug


def _configured_freenove(slug):
    return _board(vid=SETUP.VID_ESP32S3_NATIVE, nvs_state="nimbus", nvs_family=SETUP.FAMILY_FREENOVE, nvs_slug=slug)


def test_configured_freenove_keeps_its_saved_panel_image_under_yes():
    for panel in SETUP.FREENOVE_PANELS.values():
        rc, out, err, runner = _run_main(
            ["--yes", "--port", "/dev/cu.usbmodem101", "--mode", "notifier"],
            board=_configured_freenove(panel.slug),
            panel="ok",
        )
        assert rc == 0, (panel.slug, rc, err)
        assert runner.envs == ["provision-cyd", panel.env], (panel.slug, runner.envs)
        assert _acked_ota_type(runner) == panel.slug  # re-seeded with its own type, never re-typed to 2.8"


def test_configured_freenove_without_a_readable_size_is_refused_under_yes():
    # No guess for a board already in use: nothing is uploaded, the operator is asked for --size.
    rc, out, err, runner = _run_main(
        ["--yes", "--port", "/dev/cu.usbmodem101", "--mode", "notifier"],
        board=_configured_freenove(None),
        panel="ok",
    )
    assert rc == 1
    assert "could not be read" in err and "--size" in err
    assert runner.envs == []


def test_show_token_restores_the_saved_panel_image():
    # Token recovery also writes production firmware back; it must be the board's own size.
    for panel in SETUP.FREENOVE_PANELS.values():
        rc, out, err, runner = _run_main(
            ["--yes", "--port", "/dev/cu.usbmodem101", "--show-token"],
            board=_configured_freenove(panel.slug),
        )
        assert rc == 0, (panel.slug, rc, err)
        assert runner.envs == ["provision-cyd", panel.env], (panel.slug, runner.envs)


# ---- CUM-487: the board checks never silently turn off without pyserial --------------
#
# The wrong-board probe (CUM-422), the post-flash screen check (CUM-388) and USB-descriptor
# discovery used to print "Note: pyserial is unavailable ..." and let the flash proceed
# when the python3 running the installer had no pyserial (pipx/Homebrew/PEP 668 installs).

_MISSING = object()


@contextlib.contextmanager
def _no_pyserial():
    """Make every pyserial import fail in this process (a None entry in sys.modules
    raises ImportError on import), restoring the real modules afterwards."""
    names = ("serial", "serial.tools", "serial.tools.list_ports")
    saved = {name: sys.modules.get(name, _MISSING) for name in names}
    for name in names:
        sys.modules[name] = None
    try:
        yield
    finally:
        for name, module in saved.items():
            if module is _MISSING:
                sys.modules.pop(name, None)
            else:
                sys.modules[name] = module


def test_board_checks_raise_instead_of_skipping_without_pyserial():
    checks = {
        "probe_for_freenove": lambda: SETUP.probe_for_freenove("/dev/null", timeout=0.1),
        "verify_panel_after_flash": lambda: SETUP.verify_panel_after_flash("/dev/null", timeout=0.1),
        "enumerate_esp_ports": SETUP.enumerate_esp_ports,
        "identify": lambda: SETUP.identify("/dev/null"),
    }
    with _no_pyserial():
        assert SETUP.pyserial_available() is False
        for name, call in checks.items():
            err = io.StringIO()
            try:
                with contextlib.redirect_stderr(err):
                    result = call()
            except SETUP.PyserialMissing as exc:
                assert "pyserial" in str(exc), name
            else:
                raise AssertionError(f"{name} returned {result!r} without pyserial instead of failing loudly")
            assert "skipped" not in err.getvalue() and "could not be verified" not in err.getvalue(), name


def test_main_probe_without_pyserial_fails_and_restores_production():
    # If the probe ever runs without pyserial, the setup step fails (exit 1) instead of
    # proceeding unprobed; production firmware is still put back, nothing reports success.
    rc, out, err, runner = _run_main(
        ["--yes", "--port", "/dev/cu.usbmodem101", "--board", "solide_s3", "--mode", "orchestrator"],
        board=_board(vid=SETUP.VID_ESP32S3_NATIVE),
        panel="ok",
        probe=SETUP.PyserialMissing("pyserial is not installed"),
    )
    assert rc == 1
    assert "could not be applied" in err
    assert runner.envs == ["provision", "esp32s3"]  # setup sketch, then production restored
    assert "installed. NVS was not erased" not in out


def _core_with_penv_python(tmp: str) -> tuple[Path, Path]:
    core = Path(tmp)
    python = SETUP.penv_python(core)
    python.parent.mkdir(parents=True)
    python.write_text("")
    return core, python


def _must_not_rerun(command):
    raise AssertionError(f"must not re-run the installer: {command}")


def _without_reexec_flag():
    return {key: value for key, value in os.environ.items() if key != SETUP.REEXEC_ENV}


def test_ensure_pyserial_is_a_no_op_when_pyserial_is_present():
    def _must_not_run():
        raise AssertionError("PlatformIO must not be consulted when pyserial is present")

    with _patched(pyserial_available=lambda: True, platformio_executable=_must_not_run):
        assert SETUP.ensure_pyserial(["--yes"]) is None


def test_ensure_pyserial_reruns_under_platformio_python_with_the_same_args():
    children = []
    argv = ["--yes", "--board", "freenove_s3", "--size", "35"]
    with tempfile.TemporaryDirectory() as tmp:
        core, python = _core_with_penv_python(tmp)
        with (
            _patched(
                pyserial_available=lambda: False,
                platformio_executable=lambda: (core, "pio"),
                _wait_for_child=lambda command: children.append(command) or 7,
            ),
            mock.patch.dict(os.environ, _without_reexec_flag(), clear=True),
            contextlib.redirect_stdout(io.StringIO()) as out,
        ):
            assert SETUP.ensure_pyserial(argv) == 7  # the re-run's own exit code
    assert children == [[str(python), str(Path(SETUP.__file__).resolve()), *argv]]
    assert "re-running with PlatformIO's Python" in out.getvalue()


def test_ensure_pyserial_uses_the_python_a_pipx_pio_runs_on():
    # No PlatformIO penv (pipx / Homebrew install): the interpreter on the `pio` script's
    # #! line has PlatformIO, so it has pyserial. Re-run there; no build first.
    children, runner = [], _RecordingRunner()
    with tempfile.TemporaryDirectory() as tmp:
        venv_python = Path(tmp) / "venv" / "bin" / "python3.12"
        venv_python.parent.mkdir(parents=True)
        venv_python.write_text("")
        pio = Path(tmp) / "venv" / "bin" / "pio"
        pio.write_text(f"#!{venv_python}\nfrom platformio.__main__ import main\n")
        with (
            _patched(
                pyserial_available=lambda: False,
                platformio_executable=lambda: (Path(tmp) / "core", str(pio)),
                _wait_for_child=lambda command: children.append(command) or 0,
                run_checked=runner,
            ),
            mock.patch.dict(os.environ, _without_reexec_flag(), clear=True),
            contextlib.redirect_stdout(io.StringIO()),
        ):
            assert SETUP.ensure_pyserial(["--yes"]) == 0
    assert children[0][0] == str(venv_python)
    assert runner.calls == []  # found without building anything


def test_script_interpreter_accepts_only_a_python():
    with tempfile.TemporaryDirectory() as tmp:
        script = Path(tmp) / "pio"
        for line, want_python in (("#!/bin/sh", False), ("no shebang", False), ("#!/usr/bin/python3", True)):
            script.write_text(line + "\n")
            found = SETUP._script_interpreter(str(script))
            assert (found is not None) == want_python, (line, found)
    assert SETUP._script_interpreter("/no/such/pio") is None


def test_ensure_pyserial_stops_in_the_rerun_when_platformio_python_lacks_it_too():
    err = io.StringIO()
    with (
        _patched(pyserial_available=lambda: False, _wait_for_child=_must_not_rerun),
        mock.patch.dict(os.environ, {SETUP.REEXEC_ENV: "1"}),
        contextlib.redirect_stderr(err),
    ):
        assert SETUP.ensure_pyserial(["--yes"]) == 1
    assert "-m pip install pyserial" in err.getvalue()
    assert sys.executable in err.getvalue()


def test_ensure_pyserial_installs_build_tools_first_then_stops_if_still_missing():
    runner = _RecordingRunner()
    err = io.StringIO()
    with tempfile.TemporaryDirectory() as tmp:
        with (
            # No penv yet and a `pio` that is no script: a first run with nothing to re-run on.
            _patched(
                pyserial_available=lambda: False,
                platformio_executable=lambda: (Path(tmp), "pio"),
                run_checked=runner,
                _wait_for_child=_must_not_rerun,
            ),
            mock.patch.dict(os.environ, _without_reexec_flag(), clear=True),
            contextlib.redirect_stderr(err),
            contextlib.redirect_stdout(io.StringIO()),
        ):
            assert SETUP.ensure_pyserial(["--yes"]) == 1
    assert runner.calls == [["pio", "run", "-e", "esp32s3"]]  # the build tools, nothing else
    assert "PlatformIO's own Python was not found" in err.getvalue()


def test_wait_for_child_turns_a_broken_interpreter_into_a_stop():
    try:
        SETUP._wait_for_child(["/no/such/python", "setup_device.py"])
    except RuntimeError as exc:
        assert "PlatformIO's Python" in str(exc)
    else:
        raise AssertionError("a missing interpreter must become a RuntimeError, not a traceback")


# The same contract end to end, with real interpreters: the installer is launched by a
# python3 whose pyserial import fails (a blocker package first on PYTHONPATH), and
# PLATFORMIO_CORE_DIR points at a fake core whose penv holds fake tools.

_SCRIPT = str(Path(__file__).with_name("setup_device.py"))


def _posix_only():
    if os.name == "nt":
        raise unittest.SkipTest("the fake interpreters are POSIX shell scripts")


def _fake_core(tmp: Path, python_body: str) -> tuple[dict, Path]:
    blocker = tmp / "blocker" / "serial"
    blocker.mkdir(parents=True)
    (blocker / "__init__.py").write_text('raise ImportError("pyserial blocked for this test")\n')
    pio_log = tmp / "pio.log"
    bindir = tmp / "core" / "penv" / "bin"
    bindir.mkdir(parents=True)
    tools = {"pio": f'#!/bin/sh\necho "$*" >> "{pio_log}"\nexit 0\n', "python": python_body}
    for name, body in tools.items():
        (bindir / name).write_text(body)
        (bindir / name).chmod(0o755)
    env = {k: v for k, v in os.environ.items() if k not in (SETUP.REEXEC_ENV, "PYTHONHOME")}
    env.update(PYTHONPATH=str(tmp / "blocker"), PLATFORMIO_CORE_DIR=str(tmp / "core"))
    return env, pio_log


def _launch(env, *args):
    command = [sys.executable, _SCRIPT, *args]
    return subprocess.run(command, env=env, capture_output=True, text=True, timeout=60)


def test_launch_without_pyserial_reruns_under_platformio_python():
    _posix_only()
    with tempfile.TemporaryDirectory() as tmp:
        fake_python = (
            '#!/bin/sh\necho "FAKE-PENV $*"\necho "REEXEC=$NIMBUS_SETUP_REEXEC"\n'
            'echo "PYTHONPATH=[$PYTHONPATH]"\nexit 7\n'
        )
        env, pio_log = _fake_core(Path(tmp), fake_python)
        args = ("--yes", "--port", "/dev/null", "--board", "freenove_s3", "--size", "40", "--mode", "notifier")
        result = _launch(env, *args)
        assert result.returncode == 7, (result.stdout, result.stderr)  # the re-run's exit code
        assert f"FAKE-PENV {Path(_SCRIPT).resolve()} {' '.join(args)}" in result.stdout
        assert "REEXEC=1" in result.stdout
        assert "PYTHONPATH=[]" in result.stdout  # the launching Python's path does not leak in
        assert not pio_log.exists()  # nothing was built or flashed by the parent


def test_launch_when_platformio_python_also_lacks_pyserial_stops_before_any_board_work():
    # The fake penv python is a real interpreter that ALSO lacks the module (it puts the
    # blocker on its own path): the re-run must stop with the fix, not proceed with checks off.
    _posix_only()
    with tempfile.TemporaryDirectory() as tmp:
        fake_python = f'#!/bin/sh\nPYTHONPATH="{Path(tmp) / "blocker"}" exec "{sys.executable}" "$@"\n'
        env, pio_log = _fake_core(Path(tmp), fake_python)
        result = _launch(env, "--yes", "--port", "/dev/null", "--board", "solide_s3", "--mode", "orchestrator")
        assert result.returncode == 1, (result.stdout, result.stderr)
        assert "re-running with PlatformIO's Python" in result.stdout
        assert "PlatformIO's Python has no pyserial" in result.stderr
        assert "-m pip install pyserial" in result.stderr
        assert "Note:" not in result.stderr and "installed" not in result.stdout
        assert not pio_log.exists()  # no build tools run, no upload, no board touched


def test_launch_first_run_installs_build_tools_then_stops_if_python_never_appears():
    _posix_only()
    with tempfile.TemporaryDirectory() as tmp:
        env, pio_log = _fake_core(Path(tmp), "")
        (Path(tmp) / "core" / "penv" / "bin" / "python").unlink()  # PlatformIO's Python not set up yet
        result = _launch(env, "--yes", "--port", "/dev/null", "--board", "solide_s3", "--mode", "orchestrator")
        assert result.returncode == 1, (result.stdout, result.stderr)
        assert pio_log.read_text().splitlines()[0] == "run -e esp32s3"  # the build tools, then stop
        assert "PlatformIO's own Python was not found" in result.stderr


if __name__ == "__main__":
    for name, fn in sorted(globals().items()):
        if name.startswith("test_") and callable(fn):
            try:
                fn()
            except unittest.SkipTest as skip:
                print(f"skipped {name}: {skip}")
    print("setup_device: family autodetect, otaType seed, identify-and-confirm, native-USB all passed")
    print("setup_device: CUM-388 D1 failure-path screen check + D2 transport env selection passed")
    print("setup_device: CUM-490 per-panel image + CUM-487 pyserial guard (re-run or stop) passed")
