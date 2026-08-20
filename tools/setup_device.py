#!/usr/bin/env python3
"""Safely configure and install Nimbus firmware on a board connected by UART.

The installer never lets PlatformIO guess the upload port.  Before writing it
reads the board's NVS partition, reports the immutable factory MAC, and requires
that MAC to be typed back.  On a new board it also asks for the fitted display
and operating mode, writes only those two bootstrap settings through a temporary
UART diagnostic, then installs production firmware.  It never erases NVS or
factory-resets a configured Nimbus.
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
NATIVE_USB_MARKERS = ("usbmodem", "ttyACM")
NIMBUS_NVS_MARKERS = (
    b"nimbus_mode",
    b"nimbus_name",
    b"nimbus_cfg",
    b"scrModel",
    b"webTok",
)
NVS_OFFSET = "0x9000"
NVS_SIZE = "0x5000"


def uart_candidates() -> list[str]:
    """Return likely CP210x/CH34x UART ports, never native USB ports."""
    return sorted({port for pattern in UART_GLOBS for port in glob.glob(pattern)})


def is_native_usb_port(port: str) -> bool:
    return any(marker.lower() in port.lower() for marker in NATIVE_USB_MARKERS)


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


def bootstrap_commands(display: str | None, mode: str | None, board: str = "solide_s3") -> list[tuple[str, str]]:
    """Return (command, expected reply) pairs for the serial NVS diagnostic."""
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
    return commands


def prompt_bootstrap(args: argparse.Namespace, nvs_state: str) -> tuple[str | None, str | None]:
    """Choose hardware/mode, offering preserve only for a known Nimbus."""
    display, mode = args.display, args.mode
    known_nimbus = nvs_state == "nimbus"
    if args.yes:
        if not known_nimbus and (not display or not mode):
            raise RuntimeError(
                "A board without Nimbus settings needs explicit --display and --mode values when --yes is used."
            )
        return display, mode
    if not display:
        print("\nFitted display (hardware, not a theme):")
        if known_nimbus:
            print("  0. Keep the saved display setting")
        print("  1. E-ink + knob")
        print("  2. TFT color touchscreen")
        prompt = "Select the fitted display [0-2]: " if known_nimbus else "Select [1-2]: "
        answer = input(prompt).strip()
        if known_nimbus and answer == "0":
            display = None
        else:
            display = {"1": "eink", "2": "tft"}.get(answer)
        if display is None and not (known_nimbus and answer == "0"):
            raise RuntimeError("No valid display was selected.")
    if not mode:
        print("\nOperating mode:")
        if known_nimbus:
            print("  0. Keep the saved operating mode")
        print("  1. Notifier (Bluetooth only; Wi-Fi and web settings are off)")
        print("  2. Orchestrator (Wi-Fi setup and web settings are on)")
        prompt = "Select the operating mode [0-2]: " if known_nimbus else "Select [1-2]: "
        answer = input(prompt).strip()
        if known_nimbus and answer == "0":
            mode = None
        else:
            mode = {"1": "notifier", "2": "orchestrator"}.get(answer)
        if mode is None and not (known_nimbus and answer == "0"):
            raise RuntimeError("No valid operating mode was selected.")
    return display, mode


def select_port(explicit: str | None) -> str:
    if explicit:
        port = explicit
    else:
        ports = uart_candidates()
        if not ports:
            raise RuntimeError(
                "No UART bridge found. Connect the cable to the DevKit port labeled UART, then try again."
            )
        print("UART devices:")
        for index, candidate in enumerate(ports, 1):
            print(f"  {index}. {candidate}")
        answer = input("Select the board to install [number]: ").strip()
        try:
            port = ports[int(answer) - 1]
        except (ValueError, IndexError):
            raise RuntimeError("No valid UART device was selected.") from None

    if is_native_usb_port(port):
        raise RuntimeError(
            f"{port} is the ESP32-S3 native USB port, not the UART bridge. "
            "Move the cable to the DevKit port labeled UART."
        )
    if os.name != "nt" and not Path(port).exists():
        raise RuntimeError(f"Serial port does not exist: {port}")
    return port


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
        return mac, classify_nvs(nvs_path.read_bytes())


def confirm_target(
    port: str,
    mac: str,
    nvs_state: str,
    display: str | None,
    mode: str | None,
    assume_yes: bool,
) -> None:
    print("\nTarget board")
    print(f"  UART port:   {port}")
    print(f"  Factory MAC: {mac}")
    if nvs_state == "blank":
        print("  Saved state: blank (no persistent settings detected)")
    elif nvs_state == "nimbus":
        print("  Saved state: existing Nimbus settings detected")
        print("  This may already be a configured device. The settings will be preserved.")
    else:
        print("  Saved state: existing non-Nimbus data detected")
        print("  The application will change, but persistent data will be preserved.")
    if display:
        print(f"  Set display: {display}")
    if mode:
        wifi = "on" if mode == "orchestrator" else "off"
        print(f"  Set mode:    {mode} (Wi-Fi {wifi})")

    if assume_yes:
        return
    answer = input(f"\nType the factory MAC {mac} to install production firmware: ").strip().lower()
    if answer != mac:
        raise RuntimeError("Factory MAC did not match. Nothing was written.")


def serial_bootstrap(
    port: str,
    display: str | None,
    mode: str | None,
    show_token: bool = False,
    board: str = "solide_s3",
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

        for line, expected in bootstrap_commands(display, mode, board):
            print(f"  {command(line, expected)}")
        status = command("STATUS", "STATUS ")
        if display and f"screen='{display}'" not in status:
            raise RuntimeError(f"Display setting did not verify: {status}")
        expected_mode = 1 if mode == "orchestrator" else 0
        if mode and f"mode={expected_mode}" not in status:
            raise RuntimeError(f"Operating mode did not verify: {status}")
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
    parser = argparse.ArgumentParser(description="Safely install production Nimbus firmware over the UART port.")
    parser.add_argument("--port", help="exact UART serial device (otherwise choose from a list)")
    parser.add_argument(
        "--display",
        choices=("eink", "tft"),
        help="fitted display/input hardware (prompted for a blank board)",
    )
    parser.add_argument(
        "--mode",
        choices=("notifier", "orchestrator"),
        help="initial operating mode (prompted for a blank board)",
    )
    parser.add_argument(
        "--board",
        choices=("solide_s3", "freenove_s3"),
        default="solide_s3",
        help=(
            "board pinout (compile-time). solide_s3 = the hand-built boards; "
            "freenove_s3 = the Freenove CYD all-in-one (its display is fixed to tft "
            "and it flashes the cyd firmware + provisioning envs)"
        ),
    )
    parser.add_argument(
        "--yes",
        action="store_true",
        help="skip the typed-MAC confirmation; requires an explicit --port",
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
    return parser.parse_args(argv)


def main(argv: list[str] | None = None) -> int:
    args = parse_args(argv)
    if args._bootstrap_port:
        return serial_bootstrap(args._bootstrap_port, args.display, args.mode, args._show_token, board=args.board)
    if args.yes and not args.port:
        print("error: --yes is allowed only with an explicit --port", file=sys.stderr)
        return 2
    # Board pinout is compile-time: pick the matching firmware + provisioning envs.
    # The Freenove all-in-one has a fixed display, so force it to tft.
    is_cyd = args.board == "freenove_s3"
    prod_env = "esp32s3-cyd" if is_cyd else "esp32s3"
    # The Freenove is native-USB only (no CP2102 UART bridge), so its provisioning
    # console rides native USB CDC (provision-cyd), not UART0 (provision-uart).
    prov_env = "provision-cyd" if is_cyd else "provision-uart"
    if is_cyd:
        args.display = "tft"
    try:
        port = select_port(args.port)
        core, pio = platformio_executable()
        esptool = esptool_command(core)
        if esptool is None:
            print("PlatformIO needs to install the ESP32 build tools first.")
            run_checked([pio, "run", "-e", prod_env])
            esptool = esptool_command(core)
        if esptool is None:
            raise RuntimeError("PlatformIO did not install its esptool package.")
        mac, nvs_state = inspect_board(esptool, port)
        if args.show_token:
            if nvs_state != "nimbus":
                raise RuntimeError("Access-token recovery is only allowed when existing Nimbus settings are detected.")
            confirm_target(port, mac, nvs_state, None, None, args.yes)
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
        display, mode = prompt_bootstrap(args, nvs_state)
        confirm_target(port, mac, nvs_state, display, mode, args.yes)
        if display or mode:
            print("\nApplying the selected hardware and mode without erasing NVS...")
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
                        args.board,
                        *(["--display", display] if display else []),
                        *(["--mode", mode] if mode else []),
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
            "Wi-Fi network shown there (normally Nimbus-setup; password: nimbus1234)\n"
            "and open http://192.168.4.1. The Wi-Fi radio is 2.4 GHz only."
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
