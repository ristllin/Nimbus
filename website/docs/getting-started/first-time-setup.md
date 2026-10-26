---
title: "First-time setup"
sidebar_label: "First-time setup"
description: "From a new board to a Nimbus you can talk to: flash, power on, join the setup Wi-Fi, add one provider key. About ten minutes, no serial console."
---

# First-time setup

New board to a device you can talk to, in about ten minutes. No serial console.
After the one-time firmware install it asks for just two things: your Wi-Fi and
one AI provider key.

The whole path, detailed below:

1. **Flash the firmware** once. It seeds the display for you.
2. **Power on.** The device shows its setup Wi-Fi network name, its password, and
   a QR code that joins it.
3. **Join that Wi-Fi.** A setup page opens by itself, already signed in.
4. **Walk the wizard:** pick your Wi-Fi, add one provider key. Done.

:::danger Get your provider key before step 1
Setup puts your phone or laptop on the device's own Wi-Fi, which has **no
internet**, so you cannot fetch a key partway through. Create it first and keep it
pasteable on the same device you will set up from. Step-by-step for each provider:
[Get your provider key](../quick-start/provider-keys.md).
:::

:::tip The two things you cannot skip
Your **home Wi-Fi** and **one verified provider key**. Everything else (SD card,
Telegram, voice, device name) is optional and can wait. If you only want the
Notifier status light, you do not even need a provider key. See
[Notifier quick start](../quick-start/notifier-quick-start.md).
:::

---

## 1. Flash the firmware

If this is an already-configured Nimbus, skip to [step 2](#2-power-on).

Two ways to install. Both seed the display type, so the board comes up on the
right screen without asking:

- **From your browser** (easiest): open [Flash Nimbus](/flash) in Chrome or Edge,
  pick your board, and click Install. Nothing to download. The board starts in
  Orchestrator mode. Reinstalling this way replaces saved settings (see
  [Reinstalling from the browser](../quick-start/flash.md#reinstalling-from-the-browser)).
- **From the firmware repository** (needs Python 3, git, and PlatformIO; see
  [Set up your computer](../quick-start/computer-setup.md)):

  ```bash
  python3 tools/setup_device.py
  ```

  On Windows, if `python3` is not found, run `python tools/setup_device.py`
  instead. The installer lists connected boards, detects the family, and asks only what it
  cannot know: the **operating mode**, and on a Freenove the **display size**. It
  confirms before writing and never erases saved settings.

  | It asks | Options | What it sets |
  |---|---|---|
  | Operating mode | 1. Notifier · 2. Orchestrator | Notifier uses Bluetooth with Wi-Fi off; Orchestrator turns on Wi-Fi and the web UI |
  | Display size *(Freenove only)* | 2.8 / 3.5 / 4.0 inch | Matches the firmware image to the fitted display |

:::caution Classic Nimbus board: use the port labeled UART
The hand-built Nimbus board (the DevKitC-1) has **two** USB-C ports. On a fresh
board, only the one silkscreened **UART** can flash it: a board that "won't flash"
is almost always on the wrong port, not broken. The Freenove has a single port, so
there is no wrong one to pick.
:::

<details>
<summary>If you flashed <code>[env:provision]</code> or a bring-up sketch by mistake</summary>

Those are standalone diagnostics with no Nimbus UI, setup Wi-Fi, or web
settings. Run `python3 tools/setup_device.py` to install the real firmware; it
keeps saved settings. The browser flasher also works, but it replaces saved
settings.

</details>

---

## 2. Power on

Power the board over USB-C. What you see depends on the operating mode: a
browser install always starts in Orchestrator, and the command-line installer
starts in the mode you picked.

- **Orchestrator** shows its **setup Wi-Fi network name**, its **password**, and a
  **QR code that joins that network**. Continue below.
- **Notifier** shows Bluetooth pairing guidance instead, and keeps Wi-Fi off to
  leave room for Bluetooth. Follow the
  [Notifier quick start](../quick-start/notifier-quick-start.md) and you are done.

### The device names itself

A brand-new Orchestrator names itself: it looks for other Nimbus devices nearby,
then numbers itself.

| Device | Setup Wi-Fi network | Address on your network |
|---|---|---|
| the first one | `Nimbus-setup` | `nimbus.local` |
| a second nearby | `Nimbus-2-setup` | `nimbus-2.local` |
| a third | `Nimbus-3-setup` | `nimbus-3.local` |

It counts only the setup networks that are on when it first starts. A device that
has already joined your Wi-Fi turns its setup network off, so a device added later
can also call itself `Nimbus`, and one of the two then needs a new name.

Rename it later on the web page under **Home > Device name**. One name drives the setup
Wi-Fi, the network address, the Bluetooth name, and what the assistant calls
itself; a rename takes effect on the next restart.

---

## 3. Join the setup Wi-Fi

Two steps, in the order the device screen shows them:

1. **Join the setup network** on your phone or laptop:
   - **Network:** `Nimbus-setup` (or the name your screen shows)
   - **Password:** shown on the device screen, unique to your device. Or scan
     the on-screen QR to join automatically.
2. **Open the setup page.** It usually opens by itself, already signed in (a
   brand-new device signs you in automatically on its setup network). If nothing opens, browse to `http://192.168.4.1`. Do
   not use `nimbus.local` for this first hop; it can point at a different Nimbus.

On that page, pick your home network, type its password, and click **Connect**.
The wizard records the device's exact LAN address, waits while your phone or
computer rejoins its normal Wi-Fi, then continues on its own at the provider
step (a manual continuation link stays visible).

:::note Wi-Fi requirements
- The radio is **2.4 GHz only**: a 5 GHz-only network does not appear in the scan.
- Network names are case-sensitive.
- After the handoff, "home Wi-Fi connected" with "setup hotspot off" is the
  healthy state. The `Nimbus-setup` network closes once the device joins your
  router and returns if the LAN link is lost.
:::

---

## 4. Walk the wizard

The wizard asks for two required things and lets you skip the rest.

| Step | Required? | What to do |
|---|---|---|
| Choose your display | Optional | Flip the display 180 degrees if it is mounted upside down. |
| **Connect to Wi-Fi** | Yes | Tap **Scan**, pick your home network, type its password, **Connect**. |
| **Add an AI provider** | Yes, one verified key | Pick the provider, paste the API key, and click **Save & Verify**. A key whose prefix names another provider is saved under that provider; a failed check says why. |
| Your timezone | Optional | One tap sets the device to this computer's timezone. |
| Choose a mode | Optional | Keep Orchestrator, or choose Notifier (a Bluetooth status light that turns Wi-Fi and this page off; the way back is **Settings > Mode** on the device). A change restarts the device. |
| Telegram | Optional | Paste a bot token and click **Save & Verify**, or add it later. |
| Voice | Optional | Dictation and spoken replies default to Mistral. |
| Directive | Optional | Keep the default standing guidance or edit it. |
| Name this device | Optional | Rename it now or later. |

**Which provider key?** Any one verified key runs the assistant. The recommended
key is **Cumulo Nimbus** (the wizard's preselected choice): one prepaid balance
across providers, voice and memory included. **Mistral** is free to try and also
covers voice and memory. OpenAI, Anthropic, and Z.ai work too.
[Get your provider key](../quick-start/provider-keys.md) covers each provider
step by step. Keys are write-only: a saved key shows as "set"
and is never displayed again.

:::info On a touchscreen board, keep the wizard open
When the device joins your Wi-Fi, its setup network shuts down and the wizard
**follows the device to its new address automatically**. Keep the tab open until
it lands on the dashboard.
:::

:::note Long-term memory needs a Cumulo Nimbus, OpenAI or Mistral key
The device embeds memories with whichever of these keys you add, chosen before the
first memory is stored. With only an Anthropic or Z.ai key, chat works and the
wizard and the **Memory** page say that memory is off until you add one.
:::

That is the whole setup. The wizard lands on the **dashboard** at the device's
address, and **you are ready to talk to it.** Jump to
[your first turn](#your-first-turn), or add the optional pieces below.

---

## Optional: add an SD card

The microSD card holds long-term memory, media, and logs. Without one everything
still works, just with lower limits.

1. **Format it as FAT32** ("MS-DOS (FAT)" in macOS Disk Utility). Cards over
   32 GB ship as exFAT and **will not mount** until reformatted.
2. **Insert it fully**, then **tap the SD card row** in the Settings menu. It
   re-checks the card in place, no restart needed. A mounted card shows its free
   space; an unseen one shows "none - click to retry", and a card that dropped out
   shows "dropped - click to retry".

## Optional: connect Telegram

To reach the assistant from Telegram:

1. Create a bot with [@BotFather](https://t.me/BotFather) and paste its **bot
   token** into **Assistant > Connectors > Telegram**.
2. **Message your bot once** from your own Telegram account.
3. Your message appears as a **pending approval card**. Click **Approve**.

Token and allowlist changes apply right away; no restart is needed.

:::caution Open access is off for a reason
The **Open access** checkbox lets anyone who finds your bot use it, and your API
credits. Leave it off unless you mean to run a public bot.
:::

## Optional: routing and a local model

One verified key is enough. To make a specific provider lead, set the
**Primary provider** and **Fallback order** under **Assistant > Models > Routing**.

<details>
<summary>Point the assistant at a local model (Ollama)</summary>

Under **Custom endpoint**:

- **Base URL:** `http://<your-computer-ip>:11434/v1` (plain `http://` uses LAN
  HTTP, no TLS)
- **Wire:** `openai`
- **Model:** e.g. `qwen2.5`
- **API key:** leave blank

Then set the **Primary provider** to **Custom** if you want it to lead.

</details>

:::note Verify keys from Orchestrator mode
Verifying a key needs a large block of memory for the secure handshake, which
Notifier mode (with Bluetooth up) does not have. The UI hints this if you try it
there.
:::

---

## Your first turn

- **Telegram:** send the bot a message. It runs a turn and replies, sometimes
  with voice if it chooses to speak.
- **On the device:** press and hold the **on-screen mic bar** to talk, then
  release to send. The ring shows your theme color while listening, then a
  spinner from the moment you let go until the reply lands.

The device is live. From here, the
[web UI reference](webui-reference.md) walks every settings tab, and
[Orchestrator World](../guides/orchestrator-world.md) explains memory, sessions,
and the tool loop.

---

## Signing in from another browser

First-time setup never makes you handle the device sign-in code; the wizard
carried it for you. For a **later** browser, or a device configured by older
firmware:

1. **Scan the Sign-in QR** on the device screen with your phone. It opens the
   settings page already signed in.
2. If you browse in without it, a **Sign in to Nimbus** page appears. Scan the
   Sign-in QR (on the device: **Settings > Connectivity > Sign-in QR**). You
   identify once per browser; the device remembers it after that.

<details>
<summary>The screen is unavailable and the code cannot be scanned or read</summary>

Connect the board's **UART** port and run:

```bash
python3 tools/setup_device.py --show-token
```

It identifies the board, prints only the device's saved sign-in code (the long,
permanent one, not the short code the sign-in page asks for; treat it like a
password), and restores the firmware afterward. It does not erase saved
settings. Then open `http://<device-address>/?t=<code>` in a private (incognito)
window, so the code is not kept in your browser history; once the screen works
again, sign in your usual browser with the Sign-in QR.

</details>
