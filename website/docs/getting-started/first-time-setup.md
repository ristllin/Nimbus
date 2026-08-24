---
title: "First-time setup"
sidebar_label: "First-time setup"
description: "Out-of-box setup for a Nimbus device: select its display and mode safely, then configure Bluetooth or Wi-Fi."
---

# First-time setup

This is the out-of-box path: from a new board to a working device you can talk
to. After the one-time firmware install, it takes about ten minutes and needs no
serial console.

The stages, in order:

1. **Install the firmware.** The installer detects the board and asks only for
   the operating mode (and, for a Freenove, its panel size); the display is
   seeded for you.
2. **Power on.** A Notifier pairs over Bluetooth and is nearly done; an
   Orchestrator broadcasts its own setup Wi-Fi network.
3. **Orchestrator:** join the setup network and move the device onto *your*
   Wi-Fi.
4. Optionally add an SD card, an AI provider key, and Telegram.

---

## 1. Install the firmware

If this is an existing, correctly configured Nimbus, skip to the next section.

For a factory board - or one left in the wrong display/mode by an older setup
flow - connect the USB-C cable to the DevKit port labeled **UART**, not the
port labeled USB. From the firmware repository run:

```bash
python3 tools/setup_device.py
```

The installer lists every UART device and asks which one to use. On a new board
it detects the board family from its USB descriptor and saved settings, then
confirms the target and asks what it cannot know:

| Choice | Options | Effect |
|---|---|---|
| **Operating mode** | Notifier / Orchestrator | Notifier uses Bluetooth with Wi-Fi off; Orchestrator starts Wi-Fi and the web UI |
| **Panel size** *(Freenove only)* | 2.8 / 3.5 / 4.0 inch | Sets the board's update type |

It shows what it found - the board, whether persistent Nimbus settings already
exist, and the port - and confirms before writing. When more than one board is
connected it lists them and offers an Identify action that blinks a board so you
can pick the right one; the chosen port is passed to PlatformIO explicitly, and
stored settings are not erased.

For a new board, a temporary diagnostic seeds and verifies the display, its
mounting orientation, the operating mode, and the board's update type; production
Nimbus firmware is always installed last. A board flashed from the browser
flasher is seeded the same way by the image itself.

<details>
<summary>I flashed <code>[env:provision]</code> - is that setup?</summary>

No. `[env:provision]` installs a standalone serial network diagnostic. It has
no Nimbus display UI, setup Wi-Fi, or web settings. Run the installer command
above to install the real production firmware.

</details>

For details about the board's two USB-C ports, see the
[hardware reference](../guides/hardware.md#first-flash-of-a-fresh-board-use-the-uart-port).

---

## 2. Power on

Power the board over USB-C. The selected display now has the correct driver,
and what you see depends on the mode you chose:

- **Notifier** shows Bluetooth connection guidance. It intentionally keeps
  Wi-Fi and the web UI off to leave enough memory for Bluetooth.
- **Orchestrator** shows the device's **Wi-Fi network name** and a sign-in QR.

If you want web settings or Wi-Fi onboarding, choose Orchestrator in the
installer. You can change mode later, but a mode change restarts the device.

### An Orchestrator device names itself

A brand-new device picks its name automatically by looking for sibling Nimbus
devices already broadcasting nearby, then numbering itself:

| Device | Setup Wi-Fi network (SSID) | LAN address |
|---|---|---|
| first / only one | **`Nimbus-setup`** | `nimbus.local` |
| a second nearby | **`Nimbus-2-setup`** | `nimbus-2.local` |
| a third | **`Nimbus-3-setup`** | `nimbus-3.local` |

The setup network name is always `<device-name>-setup`. You can **rename the
device later** (Settings → Mode & identity); one name then drives the setup
Wi-Fi SSID, the LAN/mDNS address, the Bluetooth name, and what the assistant
calls itself. A rename **applies on the next restart**.

---

## 3. Orchestrator: join the setup Wi-Fi

Skip this section in Notifier mode; there is no Nimbus Wi-Fi network in that
mode.

On your phone or laptop, join the Wi-Fi network shown on the panel:

- **Network:** `Nimbus-setup` (or the name your panel shows)
- **Password:** shown on the device screen (unique to your device) - or scan
  the on-screen QR to join automatically

Once you join, a **captive page opens automatically** (if it doesn't, browse to
the setup AP address **`http://192.168.4.1`**). Do not use `nimbus.local` for
this first hop: it is a LAN name, and with multiple devices it may resolve to a
different Nimbus.

The captive page opens setup **already signed in**. There is no access token or
sign-in code to copy during first-time setup.

On that page, in the setup wizard's **Connect to Wi-Fi** step (or, on an
already-configured device, under **Settings → Connectivity → Wi-Fi**):

1. Click **Scan** and pick your home network from the list.
2. Type your Wi-Fi password and click **Connect**.

The page reports progress and, once the device joins, records the **exact LAN
IP address and signed-in continuation URL before changing networks**.

On a **TFT** board, keep that page open. The wizard displays "Switching to your
Wi-Fi," waits while `Nimbus-setup` closes and your phone or computer rejoins its
normal Wi-Fi, then automatically continues at the provider step using the exact
IP. A manual continuation link remains visible if the operating system does not
switch promptly. This avoids both the lost-token problem (LAN storage is a
different browser origin) and an ambiguous `nimbus.local` when several Nimbus
devices are present.

:::note Wi-Fi requirements
- The radio is **2.4 GHz only** - a 5 GHz-only SSID won't appear in the scan.
- SSIDs are **case-sensitive**.
- The setup AP turns off after a short, bounded handoff (to protect the panel
  from the beacon train). The wizard carries authentication to the exact
  reported LAN IP first; the setup AP returns if the LAN link is lost. In
  Notifier mode Wi-Fi stays off entirely.
:::

:::info Home Wi-Fi and the setup hotspot are different
"Home Wi-Fi connected" means Nimbus joined your router. "Setup hotspot" means
the temporary `Nimbus-setup` recovery network broadcast by Nimbus itself. A TFT
normally shows **home Wi-Fi connected + setup hotspot off** after handoff; that
is healthy, not a Wi-Fi failure.
:::

---

## 4. Signing in from another browser

The settings page is protected by a **per-device access token**, but during
first-time setup you never handle it - the wizard transfers it to the exact LAN
address automatically. The token rides inside the **sign-in QR** on the device
screen: scanning it opens the settings page at `http://<device-ip>/?t=<token>`,
and the browser keeps the token and attaches it to every request from then on.

For a later browser, or a device that was configured by older firmware:

1. **Scan the sign-in QR** on the panel with your phone. It opens the settings
   page on the device's LAN address, already signed in.
2. If you instead browse in without the signed-in link, a full-screen **Sign in
   to Nimbus** gate appears. Scan the QR (on the device: **Settings >
   Connectivity > Sign-in QR**). Typing the recovery access token is only the
   fallback when a QR cannot be scanned; on the device, select **Settings >
   Connectivity > Sign-in code** to open a full-screen view of the complete
   code (the compact menu row deliberately does not try to fit the secret).
   After a few rejected attempts the device also shows the sign-in QR on its
   own screen unprompted, so it's always at hand.

:::tip
You identify once per browser - the token is remembered, so you won't be asked
again on that phone or computer.
:::

<details>
<summary>The display is unavailable and I can't scan or read the token</summary>

Connect the board's **UART** port and run:

```bash
python3 tools/setup_device.py --show-token
```

The guarded tool identifies the board by factory MAC, temporarily installs the
UART diagnostic, prints only the existing web token, and restores production
firmware in a `finally` path. It does not erase stored settings. Open
`http://<reported-lan-ip>/?t=<token>` (or `http://nimbus.local/?t=<token>` only
when that name is known to be unique).

</details>

---

## 5. Change mode later

The installer chose the initial mode. To change it later, open **Settings →
Mode & identity** and choose a **Mode**:

- **Notifier - status light** - a Bluetooth status light for your coding
  sessions. If this is all you want, you're nearly done: pair the
  [nimbus-notify broker](https://pypi.org/project/nimbus-notify/) and the ring
  starts reporting. (Notifier's ring level - Dark / Calm / Full - is set from
  the Settings menu on the device, not the web page.)
- **Orchestrator - AI assistant** - Telegram, voice, and memory. Continue
  below.

Switching modes **restarts** the device. Orchestrator rejoins a saved Wi-Fi
network (this can take a few seconds); Notifier deliberately turns Wi-Fi and
the web UI off and uses Bluetooth instead.

---

## 6. Prepare the SD card (format it as FAT32)

The microSD card holds the agent's long-term memory, media, and logs. Two
requirements:

1. **Format the card as FAT32** ("MS-DOS (FAT)" in macOS Disk Utility). Cards
   over 32 GB ship as **exFAT from the factory and will not mount** - the
   device sees the card (`cardType: 3`) but the filesystem fails. Reformat
   first.
2. **Insert the card fully.** The SD card row in the Settings menu shows the
   card's state: `…GB free` when mounted, `none - click to retry` when the
   card isn't seen. After inserting or reformatting, **click that row** - it
   re-probes the card in place, no restart needed.

Without a card everything still works, just capped (memory falls back to
on-chip storage with lower limits).

---

## 7. Add a provider key (Orchestrator)

In **Assistant → Models**, under *Providers & keys*, paste a key for at
least one provider (OpenAI, Anthropic, or Mistral) and click its **Verify**
button.

- Keys are **write-only**: a saved key shows as "set" and is never displayed
  again.
- **Model dropdowns unlock only after the key verifies** against the provider.
- Verify from **Orchestrator mode** - verification needs a large contiguous
  memory block for the TLS handshake, which Notifier mode (with Bluetooth up)
  doesn't have; the UI hints this if you try it there.

Routing - the **Primary provider**, the **Fallback order**, and per-provider
models - lives in the *Routing* group just below. The defaults are sensible;
you only need to touch routing if you want a specific provider to lead.

<details>
<summary>Optional: a local model via Ollama</summary>

Under **Custom endpoint** you can point the assistant at a model running on
your own machine - for example a local **Ollama**:

- **Base URL:** `http://<your-computer-ip>:11434/v1` (a plain `http://` base
  uses LAN HTTP, no TLS)
- **Wire:** `openai`
- **Model:** e.g. `qwen2.5`
- **API key:** leave **blank** (keyless LAN endpoint)

Then set the **Primary provider** (or the fallback order) to **Custom** if you
want it to lead.

</details>

---

## 8. Let yourself message the bot (Orchestrator)

To reach the device from Telegram:

1. Create a bot with [@BotFather](https://t.me/BotFather) and paste its **bot
   token** into **Assistant → Connectors → Telegram**.
2. **Message your bot once** from your own Telegram account.
3. Your message appears in the Telegram section as a **pending approval card**
   - click **Approve**. No chat-ID hunting.

Token and allowlist changes take effect **after a restart** (the poll task
reads them at boot).

:::caution Open access
The **Open access** checkbox lets *anyone* who finds your bot use it - and
your API credits. It sits behind a confirmation dialog; leave it off unless
you mean to run a public bot.
:::

The **Voice replies** toggle in the same group lets the assistant speak its reply
aloud on the device speaker (or as a Telegram voice note) when that fits. It is on
by default; turn it off to always reply in text. Spoken replies work whichever
voice provider you pick: the speaker plays OpenAI's WAV directly and decodes
Mistral's MP3 with the bundled decoder. Set the dictation and spoken-reply
providers and the voice itself under **Assistant → Models → Voice**.

---

## 9. Your first turn

- **Telegram:** send the bot a message. It runs a turn and replies (text, and
  sometimes voice if it chooses to speak).
- **On the device:** press and hold the **on-screen mic bar** to hold-to-talk.
  On the Nimbus board the ring goes red-breathe while listening, then a spinner
  while it transcribes; release to send the turn.

That's it - the device is live. From here, the
**[Web UI reference](webui-reference.md)** walks every tab in detail, and the
**[Orchestrator World](../guides/orchestrator-world.md)** guide explains
memory, sessions, and the tool loop.
