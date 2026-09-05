# Notifier quick start

Turn Nimbus into an ambient status light for your AI coding sessions: your
computer reports session state over an encrypted Bluetooth link, and the ring
shows what every session is doing - running, waiting on you, done, or errored.

Prerequisites: a flashed device in **Notifier mode** ([flash guide](flash.md);
or switch an Orchestrator under Settings → Mode & identity - the switch
restarts it), and a computer with Python 3 and Bluetooth.

## 1. Install the broker

```bash
pip install nimbus-notify
```

The broker is the host-side half of the
[nimbus-notify](https://github.com/ristllin/nimbus-notify) project
([PyPI](https://pypi.org/project/nimbus-notify/)). It receives session events
from your coding tools and streams them to the device over Bluetooth.

## 2. Install the session hooks

```bash
nimbus-notify install-hooks
```

This wires supported AI coding tools (Claude Code, Codex, and others) to
report their session state - starting, thinking, waiting for input, waiting
for approval, done, error - to the broker. Run `nimbus-notify doctor` to check
the installation.

## 3. First pairing - run the broker in a foreground terminal

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
complete the *first* pairing - it hangs about 30 seconds and fails. Run the
broker in a normal terminal window once; after that first bond succeeds, it
works fine as a background service, and the bond survives restarts on both
sides. Also expected: Nimbus never appears in System Settings → Bluetooth -
it is a custom peripheral, paired on access rather than picked from a list.
:::

## 4. Watch it work

Start a coding session. The ring assigns the session a colored segment:

- **comet motion** - running
- **slow breathe** - waiting on you (hue says which kind: input vs approval)
- **fade to a dim ember** - done
- **breathing red** - error
- tap a session's segment on the screen; the display shows its detail

## 5. Two devices on one desk? Target one by name

Every Nimbus advertises its own name over Bluetooth. Fresh boards auto-number
themselves the first time they power up next to each other (`Nimbus`, then
`Nimbus-2`, and so on), so most fleets are already distinct. Point the broker at
one board by that name:

```bash
nimbus-notify-broker --transport ble --ble-name Nimbus-2
```

The broker then connects only to a board advertising that exact name (it still
checks for the notify service, so it never grabs an unrelated Bluetooth device).

Read the name off the device's home screen (top-left corner) or the web page
under **Settings → Mode & identity**. The **Settings → Connectivity** screen only
reports the Bluetooth state (advertising, linked, or off), not a name or address.

If two boards both show `Nimbus` (each one first booted on its own, so neither
saw the other to auto-number), rename one on the web page under
**Settings → Mode & identity**. The new name becomes its Bluetooth name after the
board restarts, and `--ble-name` can then tell them apart.

If you must target by the raw Bluetooth address instead, `--ble-address <address>`
still works. It is a distant second choice: on macOS the address is an opaque
per-host CoreBluetooth identifier (not a MAC), it changes from one computer to the
next, and the device does not display it. Read it from the serial console with the
`BLEMAC?` command if you need it.

## 6. Keep the broker running in the background

Once the first bond has succeeded (step 3), install the broker as a background
service so it starts on login and survives a restart:

```bash
nimbus-notify-broker --install-service     # enable auto-start
nimbus-notify-broker --uninstall-service   # remove it
```

This writes a launchd agent on macOS or a systemd user unit on Linux. On macOS
with Bluetooth, complete the first bond in a foreground terminal (step 3) before
you install the service: a fully detached broker cannot finish the *first*
pairing. After that first bond, the background service connects fine.

The broker keeps its runtime state (its control socket and a `status.json`
snapshot) under `~/.local/share/nsnotify/`, and prints that path on startup. When
it runs as an installed service, its log goes to `/tmp/nimbus-notify-broker.log`.

---

*How it works → [Notifier status language](../notifier-status-language.md)*
