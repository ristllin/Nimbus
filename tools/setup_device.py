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
import glob
import os
from pathlib import Path
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
# rides the ESP32-S3's native USB-CDC straight through. Used only by the
# enumerate_esp_ports() name-glob fallback when pyserial is unavailable.
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
# NVS markers that pin a configured board to a family (seeded by this installer's
# otaType, matched by presence only - no stored value is displayed).
FAMILY_NVS_MARKER = ((b"freenove", FAMILY_FREENOVE), (b"nimbus-tft", FAMILY_SOLIDE))
# The Freenove panel sizes -> the typed-OTA device slug seeded into NVS. The
# Solide board is always nimbus-tft.
FREENOVE_SIZES = {"1": ("2.8\"", "freenove-28"), "2": ("3.5\"", "freenove-35"), "3": ("4.0\"", "freenove-40")}
OTA_TYPE_SOLIDE = "nimbus-tft"

NVS_OFFSET = "0x9000"
NVS_SIZE = "0x5000"


def uart_candidates(*, allow_native: bool = False) -> list[str]:
    """Return likely CP210x/CH34x UART ports - or (allow_native, for the
    Freenove CYD) the ESP32-S3 native USB-CDC ports it uses instead, since it
    has no UART bridge for UART_GLOBS to ever find."""
    globs = NATIVE_USB_GLOBS if allow_native else UART_GLOBS
    return sorted({port for pattern in globs for port in glob.glob(pattern)})


def enumerate_esp_ports() -> list[dict]:
    """Discover connected ESP boards with their USB descriptor.

    Uses pyserial's port enumeration (VID/PID/product), so a board is identified
    by what it *is*, not just its device-node name. Only ports whose vendor is a
    known UART bridge or the ESP32-S3 native USB are returned. Falls back to the
    name globs when pyserial is unavailable, with the descriptor fields left
    blank (the name still carries the transport)."""
    try:
        from serial.tools import list_ports  # type: ignore
    except ImportError:
        out = []
        for port in uart_candidates() + uart_candidates(allow_native=True):
            out.append({"port": port, "vid": None, "pid": None, "product": ""})
        return out
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


def family_from_nvs(data: bytes) -> str | None:
    """Pin a configured board to a family by marker presence (no value shown)."""
    for marker, family in FAMILY_NVS_MARKER:
        if marker in data:
            return family
    return None


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
    try:
        import serial  # type: ignore
    except ImportError:
        print("  (cannot identify: PlatformIO's Python has no pyserial)")
        return False
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


def freenove_ota_type(args: argparse.Namespace) -> str:
    """The typed-OTA slug for a Freenove, from its panel size. --size wins; under
    --yes with no size we default to the 2.8\" panel (all sizes share one image, so
    this only labels the OTA type) with a note; otherwise we prompt."""
    if args.size:
        return {"28": "freenove-28", "35": "freenove-35", "40": "freenove-40"}[args.size]
    if args.yes:
        print("Note: no --size given; seeding the Freenove OTA type as freenove-28 (2.8\").")
        return "freenove-28"
    print("\nFreenove panel size:")
    for key, (label, slug) in FREENOVE_SIZES.items():
        print(f"  {key}. {label}  ({slug})")
    answer = input("Select the fitted panel [1-3]: ").strip()
    if answer not in FREENOVE_SIZES:
        raise RuntimeError("No valid panel size was selected.")
    return FREENOVE_SIZES[answer][1]


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


def prompt_bootstrap(
    args: argparse.Namespace, nvs_state: str, family: str
) -> tuple[str | None, str | None, str | None]:
    """Choose the settings to seed: (display, mode, ota_type).

    E-ink is gone - both configurations are TFT. The display is derived from the
    board family (never asked), a Freenove additionally picks its panel size, and
    the typed-OTA slug is seeded from the family so no board boots without a type.
    A configured Nimbus keeps its saved display/mode (display None) but is still
    (re)seeded with its OTA type, which is idempotent."""
    known_nimbus = nvs_state == "nimbus"
    mode = prompt_mode(args, known_nimbus)
    if family == FAMILY_FREENOVE:
        return "tft", mode, freenove_ota_type(args)
    # Solide board: TFT + ring. Seed the display only on a fresh board.
    display = "tft" if not known_nimbus else args.display
    return display, mode, OTA_TYPE_SOLIDE


def resolve_port(args: argparse.Namespace) -> tuple[str, int | None, str]:
    """Resolve the target to (port, usb_vid, usb_product).

    Native-USB ports are accepted for every board now (a Solide can be flashed
    over its native USB too), so there is no transport rejection. An explicit
    --port is validated for existence and its descriptor looked up; otherwise
    boards are discovered by USB descriptor and chosen via the identify-and-confirm
    picker."""
    candidates = enumerate_esp_ports()
    if args.port:
        if os.name != "nt" and not Path(args.port).exists():
            raise RuntimeError(f"Serial port does not exist: {args.port}")
        for cand in candidates:
            if cand["port"] == args.port:
                return args.port, cand["vid"], cand["product"]
        return args.port, None, ""
    chosen = pick_port(candidates, args.yes)
    return chosen["port"], chosen["vid"], chosen["product"]


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
    penv = core / "penv"

    python = penv / ("Scripts/python.exe" if os.name == "nt" else "bin/python")
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


def inspect_board(esptool: list[str], port: str) -> tuple[str, str]:
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
        return mac, classify_nvs(data), family_from_nvs(data)


def serial_bootstrap(
    port: str,
    display: str | None,
    mode: str | None,
    show_token: bool = False,
    board: str = "solide_s3",
    ota_type: str | None = None,
) -> int:
    """Apply bootstrap settings using the UART-only provision firmware."""
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
            raise RuntimeError(f"No reply containing {expected!r} to {line!r}; last lines: {seen[-3:]}")

        for line, expected in bootstrap_commands(display, mode, board, ota_type):
            print(f"  {command(line, expected)}")
        status = command("STATUS", "STATUS ")
        if display and f"screen='{display}'" not in status:
            raise RuntimeError(f"Display setting did not verify: {status}")
        expected_mode = 1 if mode == "orchestrator" else 0
        if mode and f"mode={expected_mode}" not in status:
            raise RuntimeError(f"Operating mode did not verify: {status}")
        if ota_type and f"type='{ota_type}'" not in status:
            raise RuntimeError(f"OTA type did not verify: {status}")
        print(f"  {status}")
        if show_token:
            token_line = command("TOKEN", "TOKEN ")
            if token_line == "TOKEN missing":
                raise RuntimeError("This Nimbus has not generated an access token yet.")
            print(f"\nAccess {token_line.lower().replace('token ', 'token: ', 1)}")
        return 0
    except (OSError, RuntimeError) as exc:
        print(f"Stopped: could not apply bootstrap settings: {exc}", file=sys.stderr)
        return 1
    finally:
        if connection.is_open:
            connection.close()


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
        choices=("28", "35", "40"),
        help="Freenove panel size in inches*10 (2.8/3.5/4.0) -> its typed-OTA slug; prompted otherwise",
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
    parser.add_argument("--_bootstrap-port", help=argparse.SUPPRESS)
    parser.add_argument("--_show-token", action="store_true", help=argparse.SUPPRESS)
    parser.add_argument("--_ota-type", help=argparse.SUPPRESS)
    return parser.parse_args(argv)


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
    try:
        port, usb_vid, product = resolve_port(args)
        core, pio = platformio_executable()
        esptool = esptool_command(core)
        if esptool is None:
            print("PlatformIO needs to install the ESP32 build tools first.")
            run_checked([pio, "run", "-e", "esp32s3"])  # neutral env just for the tools
            esptool = esptool_command(core)
        if esptool is None:
            raise RuntimeError("PlatformIO did not install its esptool package.")
        mac, nvs_state, nvs_family = inspect_board(esptool, port)
        # Board pinout is compile-time: auto-detect the family (explicit --board >
        # NVS marker > USB descriptor), then pick the matching firmware + prov env.
        # A native-USB board with no family marker is ambiguous - ask rather than
        # risk flashing the wrong pinout.
        family = resolve_family(args.board, usb_vid, nvs_family) or prompt_family(port, args.yes)
        is_cyd = family == FAMILY_FREENOVE
        prod_env = "esp32s3-cyd" if is_cyd else "esp32s3"
        prov_env = "provision-cyd" if is_cyd else "provision-uart"
        name = friendly_name(family, product)
        print(f"\nDetected: {name} ({FAMILY_NAME.get(family, family)}) on {port}  MAC {mac}")
        if args.show_token:
            if nvs_state != "nimbus":
                raise RuntimeError("Access-token recovery is only allowed when existing Nimbus settings are detected.")
            confirm_install(name, family, nvs_state == "nimbus", port, args.yes)
            print("\nReading the access token without erasing NVS...")
            try:
                run_checked(
                    [
                        pio,
                        "run",
                        "-e",
                        prov_env,
                        "-t",
                        "upload",
                        "--upload-port",
                        port,
                    ]
                )
                run_checked(
                    [
                        esptool[0],
                        str(Path(__file__).resolve()),
                        "--_bootstrap-port",
                        port,
                        "--_show-token",
                    ]
                )
            finally:
                print("\nRestoring production firmware...")
                run_checked(
                    [
                        pio,
                        "run",
                        "-e",
                        prod_env,
                        "-t",
                        "upload",
                        "--upload-port",
                        port,
                    ]
                )
            print("\nNimbus production firmware is restored. NVS was not erased.")
            return 0
        display, mode, ota_type = prompt_bootstrap(args, nvs_state, family)
        confirm_install(name, family, nvs_state == "nimbus", port, args.yes)
        if display or mode or ota_type:
            print("\nApplying the selected settings without erasing NVS...")
            try:
                run_checked(
                    [
                        pio,
                        "run",
                        "-e",
                        prov_env,
                        "-t",
                        "upload",
                        "--upload-port",
                        port,
                    ]
                )
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
            finally:
                # Never strand a board in a partial/temporary diagnostic if its
                # upload or acknowledgement fails, or the operator interrupts.
                print("\nRestoring production firmware...")
                run_checked(
                    [
                        pio,
                        "run",
                        "-e",
                        prod_env,
                        "-t",
                        "upload",
                        "--upload-port",
                        port,
                    ]
                )
        else:
            run_checked(
                [
                    pio,
                    "run",
                    "-e",
                    prod_env,
                    "-t",
                    "upload",
                    "--upload-port",
                    port,
                ]
            )
    except (RuntimeError, subprocess.CalledProcessError, KeyboardInterrupt) as exc:
        print(f"\nStopped: {exc}", file=sys.stderr)
        return 1

    print("\nNimbus production firmware is installed. NVS was not erased.")
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
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
