#!/usr/bin/env python3
"""Safely configure and install Nimbus firmware on a connected board.

The installer never lets PlatformIO guess the upload port. It discovers connected
boards by their USB descriptor, auto-detects the board family (Nimbus board or
Freenove CYD) from that plus the saved NVS, and confirms with an identify-and-
confirm prompt (with several boards, a numbered pick + an Identify action that
blinks a board). On a new board it seeds the display, orientation, operating mode,
and the typed-OTA device slug through a temporary serial diagnostic, then installs
production firmware. It never erases NVS or factory-resets a configured Nimbus.

Transport: the Nimbus board connects over its CP210x/CH34x UART bridge (or its
native USB); the Freenove CYD all-in-one rides the ESP32-S3's native USB-CDC. A
native port whose family cannot be told from USB + NVS is disambiguated by a
prompt (or an explicit --board), never guessed, so the wrong pinout is never
flashed. --yes --port keeps the CI path.
"""

from __future__ import annotations

import argparse
import fnmatch
import importlib
import os
from dataclasses import dataclass
from pathlib import Path
from typing import NamedTuple
import re
import shlex
import shutil
import subprocess
import sys
import tempfile
import time


UART_GLOBS = (
    "/dev/cu.usbserial-*",
    "/dev/cu.SLAB_USBtoUART*",
    "/dev/cu.wchusbserial*",
    "/dev/ttyUSB*",
)
# The Freenove CYD has no CP210x/CH34x UART bridge at all - a single USB-C port
# rides the ESP32-S3's native USB-CDC straight through. transport_of() reads these
# device-node names when an explicit --port never enumerated a USB descriptor.
NATIVE_USB_GLOBS = (
    "/dev/cu.usbmodem*",
    "/dev/tty.usbmodem*",
    "/dev/ttyACM*",
)
NIMBUS_NVS_MARKERS = (
    b"nimbus_mode",
    b"nimbus_name",
    b"nimbus_cfg",
    b"scrModel",
    b"webTok",
)
# USB vendor IDs. The hand-built Solide boards reach the host through a CP210x or
# CH34x UART bridge; the Freenove CYD all-in-one has no bridge and rides the
# ESP32-S3's own native USB (Espressif VID). A bridge therefore means "Solide";
# a native port is refined by NVS (a Solide can also be flashed over native USB).
VID_CP210X = 0x10C4
VID_CH34X = 0x1A86
VID_ESP32S3_NATIVE = 0x303A
BRIDGE_VIDS = (VID_CP210X, VID_CH34X)

# Board families and their friendly names / firmware envs. E-ink is gone: both
# shipping configurations drive a TFT (the Solide board adds the ring; the
# Freenove is the touchscreen all-in-one).
FAMILY_SOLIDE = "solide_s3"
FAMILY_FREENOVE = "freenove_s3"
FAMILY_NAME = {FAMILY_SOLIDE: "Nimbus board", FAMILY_FREENOVE: "Freenove CYD"}


class FreenovePanel(NamedTuple):
    label: str  # the size as the operator says it
    slug: str  # typed-OTA device slug seeded into NVS (frozen; the manifest key)
    env: str  # PlatformIO env that builds this panel's image


# The Freenove panel sizes. Each size is its own firmware image, compiled at that
# panel's resolution (platformio.ini esp32s3-cyd / -35 / -40), AND its own typed-OTA
# device slug, so the image the installer flashes and the slug it seeds come from ONE
# row (CUM-490: the env used to ignore the size and flash the 2.8" image on every
# panel). Keyed by the --size value.
FREENOVE_PANELS = {
    "28": FreenovePanel("2.8\"", "freenove-28", "esp32s3-cyd"),
    "35": FreenovePanel("3.5\"", "freenove-35", "esp32s3-cyd-35"),
    "40": FreenovePanel("4.0\"", "freenove-40", "esp32s3-cyd-40"),
}
FREENOVE_SLUGS = {panel.slug: panel for panel in FREENOVE_PANELS.values()}
# The Solide board has one panel: always nimbus-tft, always the esp32s3 image.
OTA_TYPE_SOLIDE = "nimbus-tft"

NVS_OFFSET = "0x9000"
NVS_SIZE = "0x5000"


class PyserialMissing(RuntimeError):
    """pyserial is not importable in the running Python (CUM-487)."""


def _pyserial(module: str = "serial"):
    """Import pyserial (or a submodule), or raise PyserialMissing. The backstop to
    ensure_pyserial() (CUM-487): a board check fails loudly, it never skips."""
    try:
        return importlib.import_module(module)
    except ImportError as exc:
        raise PyserialMissing(
            f"pyserial is not installed for {sys.executable}, so the board cannot be read. "
            "Run the installer as `python3 tools/setup_device.py` so it can switch to PlatformIO's Python."
        ) from exc


def enumerate_esp_ports() -> list[dict]:
    """Discover connected ESP boards with their USB descriptor.

    Uses pyserial's port enumeration (VID/PID/product), so a board is identified
    by what it *is*, not just its device-node name. Only ports whose vendor is a
    known UART bridge or the ESP32-S3 native USB are returned. There is no
    name-glob fallback: without pyserial this raises PyserialMissing instead of
    discovering boards blind (CUM-487)."""
    list_ports = _pyserial("serial.tools.list_ports")
    boards = []
    for info in list_ports.comports():
        vid = info.vid
        if vid not in (*BRIDGE_VIDS, VID_ESP32S3_NATIVE):
            continue
        boards.append({"port": info.device, "vid": vid, "pid": info.pid, "product": info.product or ""})
    return sorted(boards, key=lambda b: b["port"])


def usb_is_bridge(vid: int | None) -> bool:
    return vid in BRIDGE_VIDS


def family_from_usb(vid: int | None) -> str | None:
    """A UART bridge is only ever on a Solide board; a native port is ambiguous
    (a Freenove, or a Solide flashed over its native USB) so returns None."""
    return FAMILY_SOLIDE if usb_is_bridge(vid) else None


# ESP-IDF NVS page layout: a 32-byte header, a 32-byte entry-state bitmap (2 bits
# per entry), then 126 entries of 32 bytes. A string item is one header entry (type
# 0x21, key at bytes 8..24, value size at 24..26) followed by its data entries.
NVS_PAGE_SIZE = 4096
NVS_ENTRIES_PER_PAGE = 126
NVS_TYPE_STRING = 0x21
NVS_ENTRY_WRITTEN = 0b10


def nvs_string_values(data: bytes, key: str) -> set[str]:
    """Every live value of the string setting ``key`` in a raw NVS partition dump.

    Only entries the bitmap marks written count, and only under that exact key: an
    erased older value stays in flash until its page is reclaimed, and free text
    elsewhere in NVS (a device name, a Wi-Fi network name, the assistant's notes)
    can contain any string, so neither may ever be read as the setting. Values are
    returned to the caller to compare, never displayed."""
    want = key.encode()
    values = set()
    for base in range(0, len(data) - NVS_PAGE_SIZE + 1, NVS_PAGE_SIZE):
        page = data[base : base + NVS_PAGE_SIZE]
        index = 0
        while index < NVS_ENTRIES_PER_PAGE:
            entry = page[64 + index * 32 : 96 + index * 32]
            written = (page[32 + index // 4] >> ((index % 4) * 2)) & 0b11 == NVS_ENTRY_WRITTEN
            span = entry[2] if written and 1 <= entry[2] <= NVS_ENTRIES_PER_PAGE - index else 1
            if written and entry[1] == NVS_TYPE_STRING and entry[8:24].split(b"\0")[0] == want:
                size = min(int.from_bytes(entry[24:26], "little"), (span - 1) * 32)
                start = 64 + (index + 1) * 32
                values.add(page[start : start + size].split(b"\0")[0].decode("utf-8", "replace"))
            index += span
    return values


def saved_ota_type(data: bytes) -> str | None:
    """The board's live typed-OTA slug (the otaType setting this installer and the
    web flasher seed), or None when there is none or it is not unique."""
    values = nvs_string_values(data, "otaType")
    return next(iter(values)) if len(values) == 1 else None


def family_from_nvs(data: bytes) -> str | None:
    """Pin a configured board to a family by its saved otaType setting."""
    ota_type = saved_ota_type(data)
    if ota_type == OTA_TYPE_SOLIDE:
        return FAMILY_SOLIDE
    if ota_type in FREENOVE_SLUGS:
        return FAMILY_FREENOVE
    return None


def freenove_slug_from_nvs(data: bytes) -> str | None:
    """The Freenove panel slug a configured board already carries, or None (then the
    caller asks, or under --yes refuses on a configured board, never guesses)."""
    ota_type = saved_ota_type(data)
    return ota_type if ota_type in FREENOVE_SLUGS else None


def resolve_family(explicit: str | None, usb_vid: int | None, nvs_family: str | None) -> str | None:
    """Decide the board family, or None when it is genuinely ambiguous.

    Order: an explicit --board wins; then the NVS marker (a configured board that
    already carries its typed-OTA slug knows itself); then a UART bridge (only ever
    a Solide). A board on a *native* USB port with no family marker is ambiguous -
    it could be a Freenove OR a Solide flashed over native USB - and returning None
    forces a prompt or an explicit --board rather than risking the WRONG pinout
    firmware, which could drive a bad pin map onto the board."""
    if explicit:
        return explicit
    if nvs_family:
        return nvs_family
    return family_from_usb(usb_vid)


def transport_of(usb_vid: int | None, port: str = "") -> str | None:
    """Report how the board reaches the host: 'bridge' (a CP210x/CH34x UART bridge)
    or 'native' (the ESP32-S3's own USB-CDC).

    The USB vendor id decides it. When that is unknown - an explicit --port whose
    descriptor never enumerated - the device-node name is the fallback, since it
    still carries the transport (a usbmodem/ttyACM node is native, a usbserial/
    ttyUSB node is a bridge). Returns None when neither is decisive, so the caller
    can fail closed instead of guessing."""
    if usb_vid is not None:
        return "bridge" if usb_is_bridge(usb_vid) else "native"
    if any(fnmatch.fnmatch(port, pattern) for pattern in NATIVE_USB_GLOBS):
        return "native"
    if any(fnmatch.fnmatch(port, pattern) for pattern in UART_GLOBS):
        return "bridge"
    return None


def production_env(family: str, ota_type: str | None) -> str:
    """The PlatformIO env holding the production firmware for a board family and,
    for a Freenove, its panel (named by the typed-OTA slug the device keeps).

    A Freenove's panel size is compiled into its image, so the env comes from the
    same FREENOVE_PANELS row as the slug that is seeded (CUM-490). A Freenove whose
    slug matches no row fails closed rather than fall back to the 2.8" image."""
    if family == FAMILY_SOLIDE:
        return "esp32s3"
    if family == FAMILY_FREENOVE:
        if ota_type in FREENOVE_SLUGS:
            return FREENOVE_SLUGS[ota_type].env
        raise RuntimeError(f"No Freenove firmware matches the display type {ota_type!r}. Pass --size 28, 35 or 40.")
    raise RuntimeError(f"No production firmware env is defined for board family {family!r}.")


def provision_env(family: str, usb_vid: int | None, port: str = "") -> str:
    """Pick the temporary provisioning sketch by board family AND transport.

    The sketch's Serial has to reach the host over the very wire the tool reads, so
    transport matters as much as the pinout:
      - Freenove CYD -> provision-cyd (freenove pins, native USB-CDC; it has no UART
        bridge, so its transport never varies).
      - Solide over a UART bridge -> provision-uart (Serial on UART0).
      - Solide over native USB -> provision (Serial on USB-CDC, solide pins).
    A Solide whose transport cannot be determined (an explicit --port with no USB
    descriptor and a device node that names neither transport) fails closed rather
    than pick a sketch whose Serial could never answer on that port."""
    if family == FAMILY_FREENOVE:
        return "provision-cyd"
    if family != FAMILY_SOLIDE:
        raise RuntimeError(f"No provisioning sketch is defined for board family {family!r}.")
    transport = transport_of(usb_vid, port)
    if transport == "bridge":
        return "provision-uart"
    if transport == "native":
        return "provision"
    raise RuntimeError(
        f"Could not tell how {port} is connected (a USB bridge or native USB), so the setup "
        "step cannot be prepared. Reconnect the board so it lists its USB details, or run it "
        "again on the port it enumerates as."
    )


def prompt_family(port: str, assume_yes: bool) -> str:
    """Resolve an ambiguous board family: ask interactively, or (under --yes)
    refuse and demand an explicit --board so CI can never flash the wrong pinout."""
    if assume_yes:
        raise RuntimeError(
            f"Could not tell which board is on {port} from USB + NVS. Pass --board solide_s3 or --board freenove_s3."
        )
    print(f"\nCould not auto-detect the board on {port}. Which is it?")
    print(f"  1. {FAMILY_NAME[FAMILY_SOLIDE]} (TFT + ring)")
    print(f"  2. {FAMILY_NAME[FAMILY_FREENOVE]} (all-in-one touchscreen)")
    answer = input("Select the board [1-2]: ").strip()
    family = {"1": FAMILY_SOLIDE, "2": FAMILY_FREENOVE}.get(answer)
    if not family:
        raise RuntimeError("No valid board was selected.")
    return family


def friendly_name(family: str, product: str) -> str:
    """A short label for the confirm prompt: the USB product string when it says
    something specific, else the family's friendly name."""
    product = (product or "").strip()
    generic = product.lower() in (
        "",
        "usb single serial",
        "cp2102 usb to uart bridge controller",
        "usb jtag/serial debug unit",
    )
    return FAMILY_NAME.get(family, family) if generic else product


def identify(port: str) -> bool:
    """Blink the ring/screen on the board at ``port`` for ~3 s so the operator can
    tell which physical device it is. Best-effort: it asks the running firmware
    (serial ``IDENTIFY``); a board not running Nimbus simply will not answer, and
    we say so rather than fail. Returns True if the board acknowledged."""
    serial = _pyserial()
    try:
        with serial.Serial(port, 115200, timeout=0.25) as conn:
            time.sleep(0.3)
            conn.reset_input_buffer()
            conn.write(b"IDENTIFY\n")
            conn.flush()
            deadline = time.monotonic() + 3.5
            while time.monotonic() < deadline:
                raw = conn.readline()
                if raw and b"IDENTIFY" in raw:
                    print(f"  {port}: identifying (ring/screen blinking ~3 s)")
                    return True
        print(f"  {port}: no response - it may not be running Nimbus yet")
        return False
    except (OSError, ValueError) as exc:
        print(f"  {port}: could not open to identify ({exc})")
        return False


def pick_port(candidates: list[dict], assume_yes: bool) -> dict:
    """Choose among discovered boards. One board is returned straight away; several
    get a numbered menu with an Identify action (``i2`` blinks board 2) before the
    choice. --yes with a single board proceeds; --yes with several is ambiguous and
    must be disambiguated by --port."""
    if not candidates:
        raise RuntimeError(
            "No board found. Connect it over USB with a data-capable cable "
            "(the Solide board via its UART bridge, the Freenove via USB-C), then try again."
        )
    if len(candidates) == 1:
        return candidates[0]
    if assume_yes:
        raise RuntimeError("More than one board is connected; --yes needs an explicit --port to pick one.")
    print("Boards found:")
    for index, cand in enumerate(candidates, 1):
        fam = family_from_usb(cand["vid"]) or "USB-native (Freenove or native-flashed board)"
        label = FAMILY_NAME.get(fam, fam)
        print(f"  {index}. {cand['port']}  ({label})")
    print("  Enter a number to pick, or i<number> to identify that board first (blinks it ~3 s).")
    while True:
        answer = input("Select the board to install [number / i<number>]: ").strip().lower()
        if answer.startswith("i"):
            try:
                identify(candidates[int(answer[1:]) - 1]["port"])
            except (ValueError, IndexError):
                print("  Not a board number.")
            continue
        try:
            return candidates[int(answer) - 1]
        except (ValueError, IndexError):
            print("  Not a board number.")


def confirm_install(name: str, family: str, configured: bool, port: str, assume_yes: bool) -> None:
    """The identify-and-confirm gate that replaces the typed-MAC prompt."""
    family_label = FAMILY_NAME.get(family, family)
    state = "configured" if configured else "blank"
    line = f"Install to '{name}' ({family_label}, {state}) on {port}?"
    if assume_yes:
        print(line + " yes (--yes)")
        return
    answer = input(f"{line} [Y/n] ").strip().lower()
    if answer not in ("", "y", "yes"):
        raise RuntimeError("Cancelled. Nothing was written.")


def classify_nvs(data: bytes) -> str:
    """Classify a raw NVS partition without displaying any stored values."""
    if data and all(byte == 0xFF for byte in data):
        return "blank"
    if any(marker in data for marker in NIMBUS_NVS_MARKERS):
        return "nimbus"
    return "other"


def extract_mac(output: str) -> str | None:
    matches = re.findall(r"\bMAC:\s*((?:[0-9a-fA-F]{2}:){5}[0-9a-fA-F]{2})", output)
    return matches[-1].lower() if matches else None


def bootstrap_commands(
    display: str | None,
    mode: str | None,
    board: str = "solide_s3",
    ota_type: str | None = None,
) -> list[tuple[str, str]]:
    """Return (command, expected reply) pairs for the serial NVS diagnostic.

    Seeds display + orientation + mode + the typed-OTA device slug so a freshly
    flashed board never boots blank into the wrong driver and already knows its
    OTA type (no board ever waits for the transition release to learn it)."""
    commands: list[tuple[str, str]] = []
    if display:
        commands.append((f"SET scrModel={display}", "SET scrModel ok=1"))
        if display == "tft" and board != "freenove_s3":
            # Nimbus mounts the landscape module with the connector at the end
            # that needs MADCTL's 180-degree variant. The bring-up sketch had
            # this hard-coded, but fresh-board provisioning only stored
            # scrModel, leaving the production UI upside down until TFTFLIP was
            # discovered through the test console. Keep existing Nimbus boards
            # untouched (display=None); seed the known assembly only when TFT
            # hardware is explicitly selected.
            commands.append(("SETI tftFlip=1", "SETI tftFlip=1 ok=1"))
    if mode:
        value = 1 if mode == "orchestrator" else 0
        commands.append((f"SETI nimbus_mode={value}", f"SETI nimbus_mode={value} ok=1"))
    if ota_type:
        commands.append((f"SET otaType={ota_type}", "SET otaType ok=1"))
    return commands


def freenove_ota_type(args: argparse.Namespace, saved: str | None = None, configured: bool = False) -> str:
    """The typed-OTA slug for a Freenove, from its display size. --size wins; then the
    slug a configured board already carries (``saved``, its live otaType setting);
    otherwise a prompt, or under --yes the 2.8\" default for a blank board. A
    configured board with no readable saved size is refused under --yes rather than
    re-typed to 2.8\".

    The slug is load-bearing twice: it picks the image flashed (each size is its own
    image, CUM-490) and it is the OTA type the device keeps. The firmware will NOT
    guess a size later (an unseeded Freenove is offered no update, CUM-417), so a
    wrong answer here means a mis-sized image on a 3.5\"/4.0\" board."""
    if args.size:
        return FREENOVE_PANELS[args.size].slug
    if saved:
        print(f"Display size: keeping the saved size ({saved}). Pass --size to change it.")
        return saved
    if args.yes and configured:
        raise RuntimeError(
            "This Freenove is already set up, but its display size could not be read. Pass --size 28, 35 or 40."
        )
    if args.yes:
        print(
            "Note: no --size given; using the 2.8\" display (freenove-28). "
            "Pass --size 35/40 for a larger one - the firmware does not guess it later."
        )
        return FREENOVE_PANELS["28"].slug
    choices = {str(index): panel for index, panel in enumerate(FREENOVE_PANELS.values(), 1)}
    print("\nFreenove display size:")
    for key, panel in choices.items():
        print(f"  {key}. {panel.label}  ({panel.slug})")
    answer = input(f"Select the fitted display [1-{len(choices)}]: ").strip()
    if answer not in choices:
        raise RuntimeError("No valid display size was selected.")
    return choices[answer].slug


def board_ota_type(args: argparse.Namespace, family: str, nvs_state: str, saved: str | None) -> str:
    """The typed-OTA slug for this board, which also names its production image:
    nimbus-tft for the Nimbus board, the display-size slug for a Freenove."""
    if family == FAMILY_FREENOVE:
        return freenove_ota_type(args, saved, configured=nvs_state == "nimbus")
    return OTA_TYPE_SOLIDE


def prompt_mode(args: argparse.Namespace, known_nimbus: bool) -> str | None:
    """Operating mode, offering 'keep' only for a configured board."""
    if args.mode:
        return args.mode
    if args.yes:
        if not known_nimbus:
            raise RuntimeError("A blank board needs an explicit --mode when --yes is used.")
        return None
    print("\nOperating mode:")
    if known_nimbus:
        print("  0. Keep the saved operating mode")
    print("  1. Notifier (Bluetooth only; Wi-Fi and web settings are off)")
    print("  2. Orchestrator (Wi-Fi setup and web settings are on)")
    prompt = "Select the operating mode [0-2]: " if known_nimbus else "Select [1-2]: "
    answer = input(prompt).strip()
    if known_nimbus and answer == "0":
        return None
    mode = {"1": "notifier", "2": "orchestrator"}.get(answer)
    if mode is None:
        raise RuntimeError("No valid operating mode was selected.")
    return mode


def prompt_bootstrap(args: argparse.Namespace, nvs_state: str, family: str) -> tuple[str | None, str | None]:
    """Choose the display and mode to seed: (display, mode).

    E-ink is gone - both configurations are TFT. The display is derived from the
    board family (never asked). The typed-OTA slug is chosen earlier, by
    board_ota_type(), because it also picks the production image; it is seeded on
    every install so no board boots without a type. A configured Nimbus keeps its
    saved display/mode (display None)."""
    known_nimbus = nvs_state == "nimbus"
    mode = prompt_mode(args, known_nimbus)
    if family == FAMILY_FREENOVE:
        return "tft", mode
    # Solide board: TFT + ring. Seed the display only on a fresh board.
    display = "tft" if not known_nimbus else args.display
    return display, mode


def resolve_port(args: argparse.Namespace) -> tuple[str, int | None, str]:
    """Resolve the target to (port, usb_vid, usb_product).

    Native-USB ports are accepted for every board now (a Solide can be flashed
    over its native USB too), so there is no transport rejection. An explicit
    --port is validated for existence and its descriptor looked up; otherwise
    boards are discovered by USB descriptor and chosen via the identify-and-confirm
    picker."""
    if args.port:
        if os.name != "nt" and not Path(args.port).exists():
            raise RuntimeError(f"Serial port does not exist: {args.port}")
        for cand in enumerate_esp_ports():
            if cand["port"] == args.port:
                return args.port, cand["vid"], cand["product"]
        return args.port, None, ""
    chosen = pick_port(enumerate_esp_ports(), args.yes)
    return chosen["port"], chosen["vid"], chosen["product"]


def penv_python(core: Path) -> Path:
    """PlatformIO's own Python. It always has pyserial (esptool needs it)."""
    return core / "penv" / ("Scripts/python.exe" if os.name == "nt" else "bin/python")


def platformio_executable() -> tuple[Path, str]:
    core = Path(os.environ.get("PLATFORMIO_CORE_DIR", Path.home() / ".platformio"))
    penv = core / "penv"
    pio_candidates = (
        penv / ("Scripts/platformio.exe" if os.name == "nt" else "bin/pio"),
        penv / ("Scripts/pio.exe" if os.name == "nt" else "bin/platformio"),
    )
    pio = next((str(path) for path in pio_candidates if path.is_file()), None)
    if pio is None:
        pio = shutil.which("pio") or shutil.which("platformio")
    if pio is None:
        raise RuntimeError("PlatformIO is not installed or is not on PATH.")

    return core, pio


def esptool_command(core: Path) -> list[str] | None:
    python = penv_python(core)
    esptool = core / "packages/tool-esptoolpy/esptool.py"
    if not python.is_file() or not esptool.is_file():
        return None
    return [str(python), str(esptool)]


def run_checked(command: list[str], *, capture: bool = False) -> subprocess.CompletedProcess[str]:
    print(f"\n$ {shlex.join(command)}", flush=True)
    try:
        return subprocess.run(
            command,
            check=True,
            text=True,
            stdout=subprocess.PIPE if capture else None,
            stderr=subprocess.STDOUT if capture else None,
        )
    except subprocess.CalledProcessError as exc:
        if capture and exc.stdout:
            print(exc.stdout, file=sys.stderr)
        raise


def inspect_board(esptool: list[str], port: str) -> tuple[str, str, str | None, str | None]:
    """Read the NVS partition: (MAC, NVS state, family marker, saved Freenove slug)."""
    with tempfile.TemporaryDirectory(prefix="nimbus-setup-") as temp_dir:
        nvs_path = Path(temp_dir) / "nvs.bin"
        command = esptool + [
            "--chip",
            "esp32s3",
            "--port",
            port,
            "read-flash",
            NVS_OFFSET,
            NVS_SIZE,
            str(nvs_path),
        ]
        result = run_checked(command, capture=True)
        mac = extract_mac(result.stdout)
        if mac is None:
            raise RuntimeError("Connected to the board, but could not read its factory MAC.")
        data = nvs_path.read_bytes()
        return mac, classify_nvs(data), family_from_nvs(data), freenove_slug_from_nvs(data)


class NoReplyError(RuntimeError):
    """The setup sketch never answered at all on the port (no lines read).

    Distinct from a wrong or partial reply so the operator hears the likely cause
    (wrong board choice, or the board is on its other USB port) instead of a raw
    wire token like 'SET scrModel ok=1'."""

    def __init__(self, port: str):
        super().__init__(f"no reply on {port}")
        self.port = port


def _verify_bootstrap_status(status: str, display: str | None, mode: str | None, ota_type: str | None) -> None:
    """Confirm the diagnostic's STATUS line reflects each setting we just wrote."""
    if display and f"screen='{display}'" not in status:
        raise RuntimeError(f"Display setting did not verify: {status}")
    expected_mode = 1 if mode == "orchestrator" else 0
    if mode and f"mode={expected_mode}" not in status:
        raise RuntimeError(f"Operating mode did not verify: {status}")
    if ota_type and f"type='{ota_type}'" not in status:
        raise RuntimeError(f"OTA type did not verify: {status}")


def serial_bootstrap(
    port: str,
    display: str | None,
    mode: str | None,
    show_token: bool = False,
    board: str = "solide_s3",
    ota_type: str | None = None,
) -> int:
    """Apply bootstrap settings using the provisioning firmware for this board."""
    try:
        import serial  # type: ignore
    except ImportError:
        print("Stopped: PlatformIO's Python environment has no pyserial.", file=sys.stderr)
        return 1

    connection = serial.Serial()
    connection.port = port
    connection.baudrate = 115200
    connection.dtr = False
    connection.rts = False
    connection.timeout = 0.25
    connection.write_timeout = 2
    try:
        connection.open()
        # Opening the UART bridge may reset the board even with DTR/RTS cleared.
        # Hold this one session for every setting and let the diagnostic boot once.
        time.sleep(3.0)
        connection.reset_input_buffer()

        def command(line: str, expected: str, timeout: float = 4.0) -> str:
            connection.write((line + "\n").encode())
            connection.flush()
            deadline = time.monotonic() + timeout
            seen: list[str] = []
            while time.monotonic() < deadline:
                raw = connection.readline()
                if not raw:
                    continue
                text = raw.decode("utf-8", "replace").strip()
                if text:
                    seen.append(text)
                if expected in text:
                    return text
            # No lines at all means the setup sketch never answered on this port -
            # almost always the wrong board choice, or the board is on its other USB
            # port. Say that in operator language rather than quote the wire token.
            if not seen:
                raise NoReplyError(port)
            raise RuntimeError(f"No reply containing {expected!r} to {line!r}; last lines: {seen[-3:]}")

        for line, expected in bootstrap_commands(display, mode, board, ota_type):
            print(f"  {command(line, expected)}")
        status = command("STATUS", "STATUS ")
        _verify_bootstrap_status(status, display, mode, ota_type)
        print(f"  {status}")
        if show_token:
            token_line = command("TOKEN", "TOKEN ")
            if token_line == "TOKEN missing":
                raise RuntimeError("This Nimbus has not generated an access token yet.")
            print(f"\nAccess {token_line.lower().replace('token ', 'token: ', 1)}")
        return 0
    except NoReplyError as exc:
        print(
            f"Stopped: the setup sketch did not answer on {exc.port}. This is usually the wrong "
            "board choice, or the board is on its other USB port. Nothing was changed.",
            file=sys.stderr,
        )
        return 1
    except (OSError, RuntimeError) as exc:
        print(f"Stopped: could not apply bootstrap settings: {exc}", file=sys.stderr)
        return 1
    finally:
        if connection.is_open:
            connection.close()


# --- post-flash panel-responds verification (CUM-388) ---------------------------
#
# A wrong-variant flash (a Solide image on a Freenove, or vice versa) boots the
# board network-healthy with a DEAD screen: the display config verifies (scrModel
# matches) while the panel controller never answers, so the old flow printed
# "production firmware is installed" over black glass and nobody noticed. The
# restored production firmware now emits a one-shot boot signal over serial: a
# machine token "PANEL scrok=1" once the colour panel answers its controller health
# read, or a loud "DISPLAY NOT RESPONDING" line plus "PANEL scrok=0" when it does
# not. Reading that signal lets a dead-panel flash fail loudly and non-zero here
# instead of reporting success over a screen that never came up.

PANEL_DEAD_MESSAGE = (
    "\nThe screen did not come up. This is very likely the WRONG board variant\n"
    "(for example a Solide image flashed onto a Freenove): the board boots and\n"
    "joins the network, but the display controller never answers, so the glass\n"
    "stays black. Verify the board and reflash with tools/setup_device.py\n"
    "(never a raw 'pio -e <env> -t upload' - it cannot tell the pinouts apart)."
)


def read_panel_signal(connection, timeout: float = 20.0) -> str | None:
    """Watch a freshly-booted board's serial for the one-shot panel-health signal.

    Returns 'ok' (the panel answered), 'dead' (it did not - a wrong-variant or
    dead-panel flash), 'unverified' (a shared-MISO board that cannot self-check its
    panel; CUM-423), or None (nothing decisive within ``timeout``). ``connection``
    is anything with a pyserial-style ``readline()`` returning bytes (b'' when
    idle), which keeps the decision unit-testable with a fake serial."""
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        raw = connection.readline()
        if not raw:
            continue
        text = raw.decode("utf-8", "replace").strip()
        if not text:
            continue
        # A dead panel is decisive the moment either the machine token or the loud
        # human line appears; do not wait out the rest of the window. Checked first so
        # a real fault always wins over the softer "unknown".
        if "scrok=0" in text or "DISPLAY NOT RESPONDING" in text or "DISPLAY DID NOT COME UP" in text:
            return "dead"
        # A shared-MISO board honestly reports its liveness as unknown (it cannot read
        # the panel without pinning touch). Not a pass and not a failure: a soft
        # caution to look at the screen, so a legitimate solide install still proceeds.
        if "scrok=unknown" in text or "DISPLAY LIVENESS UNKNOWN" in text:
            return "unverified"
        if "scrok=1" in text:
            return "ok"
    return None


def verify_panel_after_flash(port: str, timeout: float = 20.0) -> str:
    """Open ``port`` after the production flash and read the boot panel signal.

    Returns 'ok', 'dead', 'unverified', or 'unknown'. A serial hiccup (the port busy,
    USB re-enumeration) yields 'unknown' so a healthy board is never failed on a read
    problem - only a decisive not-responding signal fails. 'unverified' (a shared-MISO
    board that cannot self-check) is a soft caution, never a hard failure, so a
    legitimate solide install still proceeds. A missing pyserial is not a read
    hiccup: it raises PyserialMissing rather than skip the check (CUM-487); main()
    guarantees pyserial before it flashes anything."""
    serial = _pyserial()
    connection = serial.Serial()
    connection.port = port
    connection.baudrate = 115200
    connection.dtr = False
    connection.rts = False
    connection.timeout = 0.25
    try:
        connection.open()
        # The board reboots into production after the upload; give it a moment to
        # start, then let the ~12 s liveness debounce settle before the token lands.
        time.sleep(3.0)
        connection.reset_input_buffer()
        return read_panel_signal(connection, timeout=timeout) or "unknown"
    except (OSError, RuntimeError) as exc:
        print(f"Note: could not read the screen-health signal ({exc}); verify the display by eye.", file=sys.stderr)
        return "unknown"
    finally:
        if connection.is_open:
            connection.close()


# --- board-family probe (CUM-422) ----------------------------------------------
#
# CUM-388 fixed the wrong-variant flash for a BLANK NVS, but once the installer seeds
# scrModel=tft the Solide image's panel-liveness read answers "healthy" on the Freenove
# wiring, so a `--board solide_s3` flash onto a Freenove passed the post-flash screen
# check (scrok=1, exit 0) and installed over the wrong board. The fix probes the physical
# board directly: the provision sketch's PROBE command scans the Freenove's FT6336U
# capacitive touch at I2C 0x38 (SDA16/SCL15). Its presence proves a Freenove regardless
# of the seeded NVS or the --board flag, so a Solide flash is refused before it can seed
# the NVS that fakes the healthy read.

PROBE_WRONG_VARIANT_MESSAGE = (
    "\nRefusing to install the Solide image: a Freenove capacitive touch controller\n"
    "(FT6336U at I2C 0x38) answered on this board, so it is a Freenove CYD, not a Solide\n"
    "board. The Solide image drives the wrong display pinout here and would boot to a\n"
    "dead screen that a seeded NVS can make read as healthy. Nothing was seeded and no\n"
    "production image was flashed. Re-run with --board freenove_s3 to install correctly."
)


def read_probe_signal(connection, timeout: float = 8.0) -> str | None:
    """Read the provision sketch's PROBE reply for the board-family probe (CUM-422).

    Returns 'freenove' (the FT6336U capacitive touch answered at I2C 0x38, so the board
    is a Freenove), 'clear' (the probe ran and nothing answered, so it is not a Freenove),
    or None (no PROBE reply within the window - an old provision sketch with no PROBE
    support). ``connection`` is anything with a pyserial-style ``readline()`` returning
    bytes (b'' when idle), which keeps the decision unit-testable with a fake serial."""
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        raw = connection.readline()
        if not raw:
            continue
        text = raw.decode("utf-8", "replace").strip()
        if "PROBE ft6336=1" in text:
            return "freenove"
        if "PROBE ft6336=0" in text:
            return "clear"
    return None


def probe_for_freenove(port: str, timeout: float = 8.0) -> str:
    """Ask the running provision sketch whether the board is a Freenove (CUM-422).

    Returns 'freenove', 'clear', 'unsupported' (no PROBE reply - an old setup firmware),
    or 'unknown' (a serial problem, e.g. the port busy). A read hiccup must not fail an
    install, only a decisive 'freenove' refuses one; an 'unsupported'/'unknown' result
    proceeds with a caution. A missing pyserial is not a read hiccup: it raises
    PyserialMissing, so the setup step fails (exit 1, production firmware still put
    back) instead of skipping the probe (CUM-487)."""
    serial = _pyserial()
    connection = serial.Serial()
    connection.port = port
    connection.baudrate = 115200
    connection.dtr = False
    connection.rts = False
    connection.timeout = 0.25
    connection.write_timeout = 2
    try:
        connection.open()
        time.sleep(3.0)  # let the provision sketch boot once before it can answer
        connection.reset_input_buffer()
        connection.write(b"PROBE\n")
        connection.flush()
        result = read_probe_signal(connection, timeout=timeout)
        return result if result is not None else "unsupported"
    except (OSError, RuntimeError) as exc:
        print(f"Note: could not run the board-family probe ({exc}); the screen check still applies.", file=sys.stderr)
        return "unknown"
    finally:
        if connection.is_open:
            connection.close()


# --- pyserial guard (CUM-487) ----------------------------------------------------
#
# The wrong-board probe (CUM-422), the post-flash screen check (CUM-388) and USB-
# descriptor discovery read the board through pyserial. PlatformIO's own Python (its
# penv) always has it, but the python3 that launches this script often does not: pipx,
# Homebrew, PlatformIO's installer script and every PEP 668 system Python leave it out.
# Those checks used to degrade to a one-line "Note" and let the flash proceed
# unchecked. Now the installer re-runs itself under PlatformIO's Python before it
# touches a board, or stops with the one command that fixes it.

REEXEC_ENV = "NIMBUS_SETUP_REEXEC"


def pyserial_available() -> bool:
    """True when this Python can import pyserial, including its port enumeration
    (a different PyPI package also installs a bare `serial` module)."""
    try:
        importlib.import_module("serial.tools.list_ports")
    except ImportError:
        return False
    return True


def pyserial_stop_message(python: Path | None) -> str:
    """What happened plus the one next step, when no Python with pyserial can run the installer."""
    if python is not None:
        fix = shlex.join([str(python), "-m", "pip", "install", "pyserial"])
        return f"Stopped: PlatformIO's Python has no pyserial, so the board cannot be read. Install it, then run this again:\n  {fix}"
    fix = shlex.join([sys.executable, "-m", "pip", "install", "pyserial"])
    return (
        "Stopped: this Python has no pyserial and PlatformIO's own Python was not found, so the board "
        f"cannot be read. Install pyserial, then run this again:\n  {fix}"
    )


def _script_interpreter(executable: str) -> Path | None:
    """The Python named on the #! line of the `pio` script, if it is one. pipx and
    Homebrew install PlatformIO into its own environment and point `pio` at that
    Python, which has pyserial (PlatformIO depends on it)."""
    try:
        with open(Path(executable).resolve(), "rb") as script:
            first = script.readline(512)
    except OSError:
        return None
    words = first[2:].decode("utf-8", "replace").split() if first.startswith(b"#!") else []
    if len(words) > 1 and Path(words[0]).name == "env":  # "#!/usr/bin/env python3"
        found = shutil.which(words[1])
        words = [found] if found else []
    if not words or not Path(words[0]).name.startswith("python"):
        return None
    return Path(words[0])


def platformio_python(core: Path, pio: str) -> Path | None:
    """A Python that has pyserial because PlatformIO runs on it: PlatformIO's own
    environment (core/penv, which the esptool step uses), else the interpreter the
    `pio` command runs on."""
    for candidate in (penv_python(core), _script_interpreter(pio)):
        if candidate is not None and candidate.is_file():
            return candidate
    return None


def _wait_for_child(command: list[str]) -> int:
    """Run the installer again as a child and return its exit code. A Ctrl-C reaches
    the child too, which handles it (it restores production firmware before it
    exits), so the parent keeps waiting instead of abandoning it mid-flash. The
    launching Python's PYTHONPATH/PYTHONHOME are dropped: they belong to that
    interpreter and could hide pyserial from PlatformIO's."""
    env = {key: value for key, value in os.environ.items() if key not in ("PYTHONPATH", "PYTHONHOME")}
    env[REEXEC_ENV] = "1"
    try:
        child = subprocess.Popen(command, env=env)
    except OSError as exc:
        raise RuntimeError(f"could not start PlatformIO's Python ({exc})") from exc
    while True:
        try:
            return child.wait()
        except KeyboardInterrupt:
            continue


def ensure_pyserial(argv: list[str]) -> int | None:
    """Make sure the installer runs under a Python that has pyserial (CUM-487).

    Returns None to carry on in this interpreter, or the exit code to return now:
    the re-run's own exit code, or 1 after a stop. Never lets an install continue
    with the board checks switched off. On a first run PlatformIO may not have set
    up its own Python yet, so the ESP32 build tools are installed first (exactly
    what the install needs anyway)."""
    if pyserial_available():
        return None
    if os.environ.get(REEXEC_ENV):
        # Already re-run under PlatformIO's Python, and it has no pyserial either.
        print(pyserial_stop_message(Path(sys.executable)), file=sys.stderr)
        return 1
    core, pio = platformio_executable()
    python = platformio_python(core, pio)
    if python is None:
        install_build_tools(pio)
        python = platformio_python(core, pio)
    if python is None:
        print(pyserial_stop_message(None), file=sys.stderr)
        return 1
    # flush: the child writes to the same terminal, and this line must come first.
    print(
        f"This Python has no pyserial, so the installer is re-running with PlatformIO's Python:\n  {python}", flush=True
    )
    return _wait_for_child([str(python), str(Path(__file__).resolve()), *argv])


def install_build_tools(pio: str) -> None:
    """Have PlatformIO install the ESP32 build tools (esptool and its own Python)."""
    print("PlatformIO needs to install the ESP32 build tools first.")
    run_checked([pio, "run", "-e", "esp32s3"])  # neutral env just for the tools


def parse_args(argv: list[str] | None = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Safely install production Nimbus firmware.")
    parser.add_argument(
        "--port",
        help="exact serial device (UART bridge or native USB). Omit to discover and pick from connected boards",
    )
    parser.add_argument(
        "--display",
        choices=("tft",),
        help="fitted display (only tft ships now; derived from the board family, rarely needed)",
    )
    parser.add_argument(
        "--mode",
        choices=("notifier", "orchestrator"),
        help="initial operating mode (prompted for a blank board)",
    )
    parser.add_argument(
        "--board",
        choices=("solide_s3", "freenove_s3"),
        default=None,
        help=(
            "board pinout (compile-time). Omit to auto-detect from the USB descriptor + NVS. "
            "solide_s3 = the Nimbus board (TFT + ring); freenove_s3 = the Freenove CYD all-in-one"
        ),
    )
    parser.add_argument(
        "--size",
        choices=tuple(FREENOVE_PANELS),
        help=(
            "Freenove display size in inches*10 (2.8/3.5/4.0): picks that size's image and its "
            "typed-OTA slug; a configured board keeps its saved size, otherwise prompted"
        ),
    )
    parser.add_argument(
        "--yes",
        action="store_true",
        help="skip the confirm prompt (CI). Needs a single connected board or an explicit --port, plus --mode for a blank board",
    )
    parser.add_argument(
        "--show-token",
        action="store_true",
        help=(
            "recover an existing Nimbus web access token over physical UART; "
            "temporarily installs the diagnostic, then restores production firmware"
        ),
    )
    parser.add_argument(
        "--skip-panel-check",
        action="store_true",
        help=(
            "skip the post-flash screen-responds check (CUM-388). By default the "
            "installer reads the booted firmware's serial and fails if the display "
            "controller does not answer (a wrong-variant flash over black glass)"
        ),
    )
    parser.add_argument("--_bootstrap-port", help=argparse.SUPPRESS)
    parser.add_argument("--_show-token", action="store_true", help=argparse.SUPPRESS)
    parser.add_argument("--_ota-type", help=argparse.SUPPRESS)
    return parser.parse_args(argv)


@dataclass
class InstallPlan:
    """The resolved flashing context: which tools, which envs, which board and port.

    Bundled so the flash helpers take one plan instead of a long argument list."""

    pio: str
    esptool: list[str]
    prod_env: str
    prov_env: str
    port: str
    family: str
    name: str
    nvs_state: str


class InstallOutcome:
    """What the flash sequence achieved, so the caller can message and exit right.

    - bootstrap_error: the setup upload or its acknowledgement failed.
    - restore_error: production firmware could not be (re)placed on the board.
    - interrupted: the operator pressed Ctrl-C; the post-flash serial wait is
      skipped so an interrupt never pays a 20 s read.
    - production_on_board: a production image is believed flashed.
    """

    def __init__(self):
        self.bootstrap_error: BaseException | None = None
        self.restore_error: BaseException | None = None
        self.interrupted = False
        self.production_on_board = False
        # CUM-422: the board-family probe proved a Freenove under a solide flash, so the
        # solide image was refused - nothing seeded, no production image restored.
        self.wrong_variant = False
        self.probe = "skipped"  # 'skipped' (not a solide flash) | 'freenove' | 'clear' | 'unsupported' | 'unknown'


def _upload(pio: str, env: str, port: str) -> None:
    run_checked([pio, "run", "-e", env, "-t", "upload", "--upload-port", port])


def _bootstrap_ack(
    esptool: list[str],
    port: str,
    family: str,
    display: str | None,
    mode: str | None,
    ota_type: str | None,
) -> None:
    run_checked(
        [
            esptool[0],
            str(Path(__file__).resolve()),
            "--_bootstrap-port",
            port,
            "--board",
            family,
            *(["--display", display] if display else []),
            *(["--mode", mode] if mode else []),
            *(["--_ota-type", ota_type] if ota_type else []),
        ]
    )


def _restore_production(plan: InstallPlan, outcome: InstallOutcome) -> None:
    """Put production firmware back, recording any failure or interrupt on outcome."""
    print("\nRestoring production firmware...")
    try:
        _upload(plan.pio, plan.prod_env, plan.port)
        outcome.production_on_board = True
    except KeyboardInterrupt:
        outcome.interrupted = True
        outcome.restore_error = KeyboardInterrupt()
    except (RuntimeError, subprocess.CalledProcessError) as exc:
        outcome.restore_error = exc


def flash_production(plan: InstallPlan, display: str | None, mode: str | None, ota_type: str | None) -> InstallOutcome:
    """Seed the selected settings, then always leave production firmware on the board.

    The provisioning sketch is only ever a means to seed NVS; whatever happens to it
    (a failed upload, a silent board, a Ctrl-C) the finally clause puts production
    firmware back so no board is ever stranded on the temporary diagnostic."""
    outcome = InstallOutcome()
    if not (display or mode or ota_type):
        # Nothing to seed: a straight production flash.
        try:
            _upload(plan.pio, plan.prod_env, plan.port)
            outcome.production_on_board = True
        except KeyboardInterrupt:
            outcome.interrupted = True
        except (RuntimeError, subprocess.CalledProcessError) as exc:
            outcome.restore_error = exc
        return outcome

    print("\nApplying the selected settings without erasing NVS...")
    try:
        _upload(plan.pio, plan.prov_env, plan.port)
        # CUM-422: with the provision sketch running, probe the FT6336U touch bus before
        # trusting a Solide flash. A Freenove answers at I2C 0x38 -> refuse the Solide
        # image regardless of --board, and do NOT seed the NVS (which would fake a healthy
        # panel) or restore the wrong production image. Only a decisive 'freenove' refuses;
        # 'unsupported' (old sketch) and 'unknown' (a serial hiccup) proceed but carry a
        # caution in finish_install, because the post-flash screen check is NOT a reliable
        # backstop here - a seeded Freenove is exactly the case CUM-388's check passes.
        if plan.family == FAMILY_SOLIDE:
            outcome.probe = probe_for_freenove(plan.port)
            if outcome.probe == "freenove":
                outcome.wrong_variant = True
                return outcome
        _bootstrap_ack(plan.esptool, plan.port, plan.family, display, mode, ota_type)
    except KeyboardInterrupt:
        outcome.interrupted = True
    except (RuntimeError, subprocess.CalledProcessError) as exc:
        outcome.bootstrap_error = exc
    finally:
        # Never strand the board on the temporary diagnostic, even after a failure or an
        # interrupt - EXCEPT when the probe refused a wrong-variant flash, where restoring
        # the Solide image is exactly what must not happen.
        if not outcome.wrong_variant:
            _restore_production(plan, outcome)
    return outcome


def _print_mode_guidance(mode: str | None) -> None:
    if mode == "orchestrator":
        print(
            "Wait for the selected display to show first-time setup, then join the\n"
            "Wi-Fi network it names. Its name and password are both shown on that\n"
            "screen (each device has its own password), so read them from the device.\n"
            "Then open http://192.168.4.1. The Wi-Fi radio is 2.4 GHz only."
        )
    elif mode == "notifier":
        print(
            "Notifier intentionally keeps Wi-Fi and the web UI off. Pair the Nimbus\n"
            "Bluetooth service with the nimbus-notify broker to continue."
        )
    else:
        print("Existing display and operating-mode settings were preserved.")


def finish_install(outcome: InstallOutcome, panel: str, mode: str | None, skip_panel_check: bool) -> int:
    """Turn the flash outcome and screen signal into the operator result + exit code.

    Pure decision and messaging (no serial, no flashing) so every failure path is
    host-testable. The success line prints only on a clean install whose screen did
    not report dead; the wrong-variant message prints exactly when the screen is
    dead, on both the success and the setup-failed paths (CUM-388 D1). The board-family
    probe (CUM-422) is decisive first: a Freenove under a solide flash is refused before
    any seed or production write."""
    if outcome.wrong_variant:
        print(PROBE_WRONG_VARIANT_MESSAGE, file=sys.stderr)
        return 1
    if outcome.restore_error is not None:
        print(
            "\nStopped: production firmware could not be restored (the step above failed).\n"
            "The board may still hold the temporary setup firmware. Reconnect it and run\n"
            "tools/setup_device.py again to finish the install.",
            file=sys.stderr,
        )
        return 1
    if outcome.interrupted:
        print(
            "\nStopped: setup was interrupted. Production firmware was restored, so the\n"
            "board is safe to use, but the setup settings were not applied. Run\n"
            "tools/setup_device.py again to finish.",
            file=sys.stderr,
        )
        return 1
    if outcome.bootstrap_error is not None:
        print("\nStopped: the setup settings could not be applied (the step above failed).", file=sys.stderr)
        if panel == "dead":
            print(PANEL_DEAD_MESSAGE, file=sys.stderr)
        elif panel == "ok":
            print(
                "\nThe screen is responding, so the board variant is correct. The setup\n"
                "settings were not applied. Run tools/setup_device.py again to retry.",
                file=sys.stderr,
            )
        else:
            print(
                "\nProduction firmware was restored. Look at the screen, then run\n"
                "tools/setup_device.py again to retry the settings.",
                file=sys.stderr,
            )
        return 1
    # Clean success: production firmware is on the board with no error.
    if panel == "dead":
        print(PANEL_DEAD_MESSAGE, file=sys.stderr)
        return 1
    if panel == "ok":
        print("Screen check passed: the display is responding.")
    elif panel == "unverified":
        # A shared-MISO board cannot self-check its panel without pinning touch
        # (CUM-423), so the firmware reports liveness as unknown. That is expected on
        # this board, not a fault: install proceeds, with a nudge to glance at the glass.
        print(
            "Screen check: this board cannot self-check the display (touch shares the\n"
            "panel bus). The install is fine; look at the screen to confirm it shows\n"
            "the Nimbus UI."
        )
    elif not skip_panel_check:
        print("Screen check could not confirm the display; look at the screen to be sure.")
    if outcome.probe == "clear":
        # CUM-422: say so when the probe DID rule a Freenove out, so an install log carries
        # the evidence instead of only the silence of a refusal that did not happen.
        print("Board check: no Freenove touch controller found, so this is a Nimbus board.")
    if outcome.probe in ("unsupported", "unknown"):
        # CUM-422: the board-family probe could not confirm this is a Nimbus board - an old
        # provision sketch with no PROBE support ('unsupported'), or a serial hiccup
        # ('unknown'). A Freenove could not be ruled out, and the screen check is NOT a
        # reliable backstop for a seeded Freenove, so proceed with a loud caution.
        print(
            "Note: the board-family probe could not confirm this is a Nimbus board\n"
            "(no PROBE support, or the probe could not be read). A Freenove could not be\n"
            "ruled out, so look at the screen to be sure the correct variant was installed."
        )
    print("\nNimbus production firmware is installed. NVS was not erased.")
    _print_mode_guidance(mode)
    return 0


def run_show_token(plan: InstallPlan, assume_yes: bool) -> int:
    """Recover an existing Nimbus access token over serial, then restore production."""
    if plan.nvs_state != "nimbus":
        raise RuntimeError("Access-token recovery is only allowed when existing Nimbus settings are detected.")
    confirm_install(plan.name, plan.family, True, plan.port, assume_yes)
    print("\nReading the access token without erasing NVS...")
    try:
        _upload(plan.pio, plan.prov_env, plan.port)
        run_checked([plan.esptool[0], str(Path(__file__).resolve()), "--_bootstrap-port", plan.port, "--_show-token"])
    finally:
        print("\nRestoring production firmware...")
        _upload(plan.pio, plan.prod_env, plan.port)
    print("\nNimbus production firmware is restored. NVS was not erased.")
    return 0


def main(argv: list[str] | None = None) -> int:
    args = parse_args(argv)
    if args._bootstrap_port:
        return serial_bootstrap(
            args._bootstrap_port,
            args.display,
            args.mode,
            args._show_token,
            board=args.board or "solide_s3",
            ota_type=args._ota_type,
        )
    # Phase 1: discover the board and decide what to flash. Nothing is written yet,
    # so a failure here changed nothing on the board and no screen check applies.
    try:
        # CUM-487: the board checks need pyserial; switch to PlatformIO's Python (or
        # stop) before anything reads the board, never run them switched off.
        early_exit = ensure_pyserial(sys.argv[1:] if argv is None else argv)
        if early_exit is not None:
            return early_exit
        port, usb_vid, product = resolve_port(args)
        core, pio = platformio_executable()
        esptool = esptool_command(core)
        if esptool is None:
            install_build_tools(pio)
            esptool = esptool_command(core)
        if esptool is None:
            raise RuntimeError("PlatformIO did not install its esptool package.")
        mac, nvs_state, nvs_family, nvs_slug = inspect_board(esptool, port)
        # Board pinout is compile-time: auto-detect the family (explicit --board >
        # NVS marker > USB descriptor), then pick the matching firmware env. The
        # provisioning env is chosen by family AND transport (CUM-388 D2) so its
        # Serial can answer on the very port in use - a Solide on native USB gets
        # the native-USB provision sketch, not the UART one that can never reply.
        family = resolve_family(args.board, usb_vid, nvs_family) or prompt_family(port, args.yes)
        name = friendly_name(family, product)
        print(f"\nDetected: {name} ({FAMILY_NAME.get(family, family)}) on {port}  MAC {mac}")
        # The typed-OTA slug also names the production image (a Freenove panel size is
        # compiled in, CUM-490), so it is settled before the plan, for every flow that
        # writes production firmware back - the install and the token recovery alike.
        ota_type = board_ota_type(args, family, nvs_state, nvs_slug)
        plan = InstallPlan(
            pio=pio,
            esptool=esptool,
            prod_env=production_env(family, ota_type),
            prov_env=provision_env(family, usb_vid, port),
            port=port,
            family=family,
            name=name,
            nvs_state=nvs_state,
        )
        if args.show_token:
            return run_show_token(plan, args.yes)
        display, mode = prompt_bootstrap(args, nvs_state, family)
        confirm_install(name, family, nvs_state == "nimbus", port, args.yes)
    except (RuntimeError, subprocess.CalledProcessError, KeyboardInterrupt) as exc:
        print(f"\nStopped: {exc}", file=sys.stderr)
        return 1

    # Phase 2: flash. From here a production image may be, or may fail to be, on the
    # board, so a setup failure still reads the boot screen signal and fails loudly
    # with the wrong-variant message instead of stranding a dead-screen board with
    # only a bootstrap error (CUM-388 D1). The screen read is skipped when there is
    # nothing safe to read: a Ctrl-C (no 20 s wait tacked onto an interrupt) or a
    # failed restore (no trustworthy production image to signal).
    outcome = flash_production(plan, display, mode, ota_type)
    panel = "unknown"
    if (
        not args.skip_panel_check
        and outcome.production_on_board
        and outcome.restore_error is None
        and not outcome.interrupted
    ):
        print("\nChecking the screen came up...")
        panel = verify_panel_after_flash(port)
    return finish_install(outcome, panel, mode, args.skip_panel_check)


if __name__ == "__main__":
    raise SystemExit(main())
