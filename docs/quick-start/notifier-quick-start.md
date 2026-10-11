# Notifier quick start

Turn Nimbus into an ambient status light for your AI coding sessions: your
computer reports session state over an encrypted Bluetooth link, and the ring
shows what every session is doing - running, waiting on you, done, or errored.

You need a flashed device in **Notifier mode** and a computer with Python 3 and
Bluetooth. The [installer](flash.md) asks for the starting mode on a new board;
a device already running as an Orchestrator switches on its web page under
**Settings → Mode & identity** (the switch restarts it).

## 1. Install the broker

Install it with `pipx`, which keeps command-line tools in their own space:

```bash
pipx install nimbus-notify
```

If pipx is not installed, run `sudo apt install pipx` (Ubuntu, Debian) or
`brew install pipx` (macOS with Homebrew), then `pipx ensurepath`, and reopen the
terminal. On Windows, or with Python from python.org, `pip install nimbus-notify`
also works.

The broker is the host-side half of the
[nimbus-notify](https://github.com/ristllin/nimbus-notify) project
([PyPI](https://pypi.org/project/nimbus-notify/)). It receives session events
from your coding tools and streams them to the device over Bluetooth.

## 2. Install the session hooks

```bash
nimbus-notify install-hooks
```

This wires the supported coding tools (Claude Code, Codex, and Mistral Vibe) to
report their session state (starting, thinking, waiting for input, waiting for
approval, done, error) to the broker. Add `--harness claude|codex|vibe` to wire
only one. Run `nimbus-notify doctor` to check the installation.

## 3. Pair: run the broker in a foreground terminal

```bash
nimbus-notify-broker --transport ble
```

The broker talks over a USB serial cable by default, so pass `--transport ble`
for the wireless link (or `--transport auto`, which picks Bluetooth when no
board is plugged in over USB).

The device's Bluetooth link is **bonded and encrypted** (Just Works pairing):
an unpaired computer can connect but cannot paint the ring. The bond is
created automatically on the broker's first frame - no dialog, no code.

:::caution The first bond must happen in a foreground terminal
On macOS, a fully detached broker (launchd service, `nohup … &`) cannot
complete the *first* pairing: it waits about 30 seconds and fails. Run the
broker in a normal terminal window once. After that first bond succeeds, the
broker runs as a background service and the bond survives restarts on both
sides. Nimbus never appears in System Settings → Bluetooth: it is a custom
peripheral, bonded on first access rather than picked from a list.
:::

## 4. Watch it work

Start a coding session. The ring gives the session a colored segment:

- **Comet motion**: running
- **Slow breathe**: waiting on you (cool hue for an answer, amber for an approval)
- **Fade to a dim ember**: done
- **Breathing red**: error

Tap a session's card on the screen to open its detail.

## 5. Target one device by name

Every Nimbus advertises its device name over Bluetooth. A board names itself the
first time it starts in Orchestrator mode, checking for neighbors so two boards
on one desk come up as `Nimbus` and `Nimbus-2`. A board that has only ever run
in Notifier mode advertises `Nimbus`. Point the broker at one board by name:

```bash
nimbus-notify-broker --transport ble --ble-name Nimbus-2
```

The broker then connects only to a board advertising that exact name (it still
checks for the notify service, so it never grabs an unrelated Bluetooth device).

Read the name from the header of the device's home screen. The device's
**Settings → Connectivity** screen reports only the Bluetooth state (advertising,
linked, or off), not the name or address. The web page is not available in
Notifier mode.

If two boards both advertise `Nimbus`, rename one: switch it to Orchestrator
mode, open its web page, and set **Device name** on the Home page. The new name
becomes its Bluetooth name after the board restarts, and `--ble-name` can then
tell them apart.

If you must target by the raw Bluetooth address instead, `--ble-address <address>`
still works, with caveats: on macOS the address is an opaque per-host
CoreBluetooth identifier (not a MAC), it changes from one computer to the next,
and the device does not display it. Read it from the serial console with the
`BLEMAC?` command if you need it.

## 6. Keep the broker running in the background

Once the first bond has succeeded (step 3), install the broker as a background
service so it starts on login and survives a restart:

```bash
nimbus-notify-broker --install-service     # enable auto-start
nimbus-notify-broker --uninstall-service   # remove it
```

This writes a launchd agent on macOS or a systemd user unit on Linux.

The broker keeps its runtime state (its control socket and a `status.json`
snapshot) under `~/.local/share/nsnotify/`, and prints that path on startup. When
it runs as an installed service, its log goes to `/tmp/nimbus-notify-broker.log`.

---

*How it works → [Notifier status language](../notifier-status-language.md)*
