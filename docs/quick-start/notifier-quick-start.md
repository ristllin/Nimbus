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
nimbus-notify-broker
```

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
- rotate the knob to point at a session; the display shows its detail

Two devices? Both advertise as "Nimbus" - target one by address:
`nimbus-notify-broker --ble-address <mac>` (the device prints its address in
Settings > Connectivity).

---

*How it works → [Notifier status language](../notifier-status-language.md)*
