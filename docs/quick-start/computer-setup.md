<!-- audience: user -->
# Set up your computer

This page installs everything the command-line flasher needs: a terminal,
Python 3, git, PlatformIO, and (on some computers) a USB-serial driver or
permission to use serial ports. It assumes you have never opened a terminal.

:::tip Most people can skip this page
The **[browser flasher](flash.md#path-1---browser-flasher-recommended)** needs
none of the below. All it takes is a Chromium-based browser (Chrome or Edge) and
a data-capable USB cable. Come here only if you want the command-line path, or the
browser flasher never shows your board's serial port.
:::

:::note Plan for the download
The first build downloads about 0.6 GB of ESP32 build tools and takes several
minutes: about three on a fast computer and connection, longer on a slow one.
Afterwards PlatformIO's tools take about 5 GB of disk and each board you build
adds about 0.35 GB, so have at least 6 GB free. Later builds take about a
minute.
:::

## 1. Open a terminal

The terminal is a window where you type a command, press Enter, and read the result.

- **macOS:** press Cmd+Space, type `Terminal`, press Enter.
- **Windows:** press the Start key, type `PowerShell`, press Enter.
- **Linux:** press Ctrl+Alt+T, or search your apps for `Terminal`.

## 2. Install Python 3

The Nimbus installer (`tools/setup_device.py`) is a Python script, so you need Python 3.8 or newer.

- **macOS / Windows:** download the installer from
  [python.org/downloads](https://www.python.org/downloads/) and run it. On
  Windows, tick **Add Python to PATH** on the first screen.
- **Linux:** Python is almost always already there. Also install `pipx`, which
  step 4 uses to install PlatformIO. On Ubuntu or Debian:

  ```bash
  sudo apt install python3 pipx
  ```

Check it worked:

```bash
python3 --version
```

You should see `Python 3.something`. If Windows says `python3` is not found, try
`python --version` instead.

## 3. Install git

Step 6 uses git to download the firmware.

- **macOS:** run `git --version`. If git is missing, macOS offers to install its
  command line developer tools: accept, and wait for the install to finish (it
  can take several minutes).
- **Windows:** download the installer from
  [git-scm.com](https://git-scm.com/downloads), run it with the default options,
  then close and reopen PowerShell.
- **Linux:** on Ubuntu or Debian, `sudo apt install git`; elsewhere, install the
  `git` package from your package manager.

Check it worked:

```bash
git --version
```

You should see `git version 2.something`.

## 4. Install PlatformIO

PlatformIO builds and uploads the firmware. How you install it depends on where
your Python came from.

**Linux, or a Mac that uses Homebrew's Python:** install it with `pipx`, which
keeps command-line tools in their own space. On a Mac, run `brew install pipx`
first; on Linux, step 2 installed it.

```bash
pipx install platformio
pipx ensurepath
```

These systems refuse a plain `pip3 install` with an
`externally-managed-environment` error; pipx is the fix that error suggests.

**Windows, or a Mac with Python from python.org:** use Python's package tool:

```bash
pip3 install platformio
```

On Windows, if `pip3` is not found, run `pip install platformio` instead. If
`pip3` stops with `externally-managed-environment`, use the pipx commands above.

Close and reopen the terminal so it picks up the new `pio` command, then check it
worked:

```bash
pio --version
```

You should see `PlatformIO Core, version 6.something`.

## 5. Install a USB-serial driver (only if needed)

The Nimbus board's **UART** port talks to your computer through a CP2102N chip.
macOS and modern Windows and Linux usually recognize it with no driver. Install
one only if the flasher never lists a serial port for a connected board:

- Download the **CP210x** driver from
  [Silicon Labs](https://www.silabs.com/developer-tools/usb-to-uart-bridge-vcp-drivers),
  install it, then unplug and replug the board.

The **all-in-one (Freenove CYD)** board uses the ESP32-S3's built-in USB and needs no
driver on any current operating system.

### Linux: allow your account to use serial ports

On Linux a connected board shows up as `/dev/ttyUSB0` (the Nimbus board's UART
port) or `/dev/ttyACM0` (native USB, including the Freenove CYD), and those ports
belong to the `dialout` group. A new account is usually not in it, so the installer stops
with a `Permission denied` error naming the port. Add your account once, then log
out and back in:

```bash
sudo usermod -aG dialout $USER
```

On Arch Linux the group is `uucp` instead. PlatformIO may also print
`Please install 99-platformio-udev.rules` during an upload; its
[udev rules](https://docs.platformio.org/en/latest/core/installation/udev-rules.html)
work too, but they open the port to every account on the computer, so on a
shared computer prefer the group.

## 6. Get the firmware and flash

Clone the firmware repository, then run the installer:

```bash
git clone https://github.com/ristllin/Nimbus.git
cd Nimbus
python3 tools/setup_device.py
```

On Windows, if `python3` is not found, run `python tools/setup_device.py` instead
(as in step 2).

The installer finds your board, confirms before it writes, and never erases an
already-configured device. If the ESP32 build tools are not installed yet, it
prints `PlatformIO needs to install the ESP32 build tools first.` and starts the
download described at the top of this page; let it finish. Installer flags,
building without a board, and recovery are covered in
[Flash the firmware](flash.md).

---

*Next step: [Flash the firmware](flash.md)*
