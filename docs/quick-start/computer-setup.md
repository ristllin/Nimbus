<!-- audience: user -->
# Set up your computer

For anyone who has never opened a terminal. By the end you will have the tools
the command-line flasher needs: a terminal, Python 3, PlatformIO, and (on some
computers) a USB-serial driver.

:::tip Most people can skip this page
The **[browser flasher](flash.md#path-1---browser-flasher-recommended)** needs
none of the below. All it takes is a Chromium-based browser (Chrome or Edge) and
a data-capable USB cable. Come here only if you want the command-line path, or the
browser flasher never shows your board's serial port.
:::

## 1. Open a terminal

The terminal is a window where you type commands. Opening it is all step 1 asks.

- **macOS:** press Cmd+Space, type `Terminal`, press Enter.
- **Windows:** press the Start key, type `PowerShell`, press Enter.
- **Linux:** press Ctrl+Alt+T, or search your apps for `Terminal`.

Type a command, press Enter, read the result. That is the whole skill.

## 2. Install Python 3

Nimbus's installer is a Python program, so you need Python 3.8 or newer.

- **macOS / Windows:** download the installer from
  [python.org/downloads](https://www.python.org/downloads/) and run it. On
  Windows, tick **Add Python to PATH** on the first screen.
- **Linux:** it is almost always already there; if not, install `python3` and
  `python3-pip` from your package manager.

Check it worked:

```bash
python3 --version
```

You should see `Python 3.something`. If Windows says `python3` is not found, try
`python --version` instead.

## 3. Install PlatformIO

PlatformIO builds and uploads the firmware. Install it with Python's package
tool:

```bash
pip3 install platformio
```

Check it worked:

```bash
pio --version
```

If the command is not found, close and reopen the terminal so it picks up the new
tool, then try again.

## 4. Install a USB-serial driver (only if needed)

The Nimbus board's **UART** port talks to your computer through a CP2102N chip.
macOS and modern Windows and Linux usually recognize it with no driver. Install
one only if the flasher never lists a serial port for a connected board:

- Download the **CP210x** driver from
  [Silicon Labs](https://www.silabs.com/developer-tools/usb-to-uart-bridge-vcp-drivers),
  install it, then unplug and replug the board.

The **all-in-one (Freenove CYD)** board rides the ESP32-S3's own USB and needs no
driver on any current operating system.

## 5. Get the firmware and flash

You are ready for the command-line path. Clone the firmware repository, then run
the installer:

```bash
git clone https://github.com/ristllin/Nimbus.git
cd Nimbus
python3 tools/setup_device.py
```

The installer finds your board, confirms before it writes, and never erases an
already-configured device. Full flashing steps, flags, and recovery:
**[Flash the firmware](flash.md)**.

---

*Next step: [Flash the firmware](flash.md)*
