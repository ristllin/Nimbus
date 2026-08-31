---
title: "First-time setup"
sidebar_label: "First-time setup"
description: "From a new board to a Nimbus you can talk to. A short, foolproof path: flash, power on, join the setup Wi-Fi, add one provider key. About ten minutes, no serial console."
---

# First-time setup

New board to a device you can talk to, in about ten minutes. No serial console.
After the one-time firmware install it asks for just two things: your Wi-Fi and
one AI provider key.

The whole path, detailed below:

1. **Flash the firmware** once. It seeds the display for you.
2. **Power on.** The device shows its setup Wi-Fi network and a sign-in QR.
3. **Join that Wi-Fi.** A setup page opens by itself, already signed in.
4. **Walk the wizard:** pick your Wi-Fi, add one provider key. Done.

:::tip The two things you cannot skip
Your **home Wi-Fi** and **one verified provider key**. Everything else (SD card,
Telegram, voice, device name) is optional and can wait. If you only want the
Notifier status light, you do not even need a provider key. See
[Notifier quick start](../quick-start/notifier-quick-start.md).
:::

---

## 1. Flash the firmware

If this is an already-configured Nimbus, skip to [step 2](#2-power-on).

Two ways to install. Both seed the display, so the board comes up on the right
panel with no display question:

- **From your browser** (easiest): open [Flash Nimbus](/flash) in Chrome or Edge,
  pick your board, and click Install. Nothing to download.
- **From the firmware repository:**

  ```bash
  python3 tools/setup_device.py
  ```

  The installer lists connected boards, detects the family, and asks only what it
  cannot know: the **operating mode**, and on a Freenove the **panel size**. It
  confirms before writing and never erases saved settings.

  | It asks | Options | What it sets |
  |---|---|---|
  | Operating mode | 1. Notifier · 2. Orchestrator | Notifier uses Bluetooth with Wi-Fi off; Orchestrator turns on Wi-Fi and the web UI |
  | Panel size *(Freenove only)* | 2.8 / 3.5 / 4.0 inch | Matches the firmware image to your glass |

:::caution Classic Nimbus board: use the port labeled UART
The hand-built Nimbus board (the DevKitC-1) has **two** USB-C ports. On a fresh
board, only the one silkscreened **UART** can flash it: a board that "won't flash"
is almost always on the wrong port, not broken. The Freenove has a single port, so
there is no wrong one to pick.
:::

<details>
<summary>I flashed <code>[env:provision]</code> or a bring-up sketch by mistake</summary>

Those are standalone diagnostics with no Nimbus UI, setup Wi-Fi, or web
settings. Run `python3 tools/setup_device.py` (or the browser flasher) to install
the real firmware. Saved settings are preserved.

</details>

---

## 2. Power on

Power the board over USB-C. What you see depends on the mode you flashed:

- **Orchestrator** shows its **setup Wi-Fi network name** and a **sign-in QR**.
  Continue below.
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

Rename it later under **Settings > Mode & identity**. One name drives the setup
Wi-Fi, the network address, the Bluetooth name, and what the assistant calls
itself; a rename takes effect on the next restart.

---

## 3. Join the setup Wi-Fi

On your phone or laptop, join the network shown on the screen:

Two steps, in order - the same two the device screen shows:

1. **Join the setup network** shown on the panel:
   - **Network:** `Nimbus-setup` (or the name your screen shows)
   - **Password:** shown on the device screen, unique to your device. Or scan
     the on-screen QR to join automatically.
2. **Open the setup page.** It usually opens by itself, already signed in (the
   QR carried the code). If nothing opens, browse to `http://192.168.4.1`. Do
   not use `nimbus.local` for this first hop; it can point at a different Nimbus.

On that page, pick your home network, type its password, and click **Connect**.
The wizard records the device's exact LAN address, waits while your phone or
computer rejoins its normal Wi-Fi, then continues on its own at the provider
step (a manual continuation link stays visible).

:::note Wi-Fi requirements
- The radio is **2.4 GHz only** - a 5 GHz-only network won't appear in the scan.
- SSIDs are case-sensitive.
- "Home Wi-Fi connected" with "setup hotspot off" is the healthy state after
  handoff - the temporary `Nimbus-setup` network closes once the device joins
  your router, and returns if the LAN link is lost.
:::

---

## 4. Walk the wizard

The wizard asks for two required things and lets you skip the rest.

| Step | Required? | What to do |
|---|---|---|
| **Connect to Wi-Fi** | Yes | Tap **Scan**, pick your home network, type its password, **Connect**. |
| **Provider key** | Yes, one verified key | Paste an API key and click **Verify**. |
| Operating mode | Optional | Keep what you flashed, or change it (a change restarts the device). |
| Telegram | Optional | Add a bot token later. |
| Voice | Optional | Dictation and spoken replies default to Mistral. |
| Device name | Optional | Rename it now or later. |

**Which provider key?** Any one verified key runs the assistant. **Mistral is the
easiest start:** free tier, and the voice default. OpenAI and Anthropic work too.
Keys are write-only: a saved key shows as "set" and is never displayed again.

:::info On a touchscreen board, keep the wizard open
When the device joins your Wi-Fi, its setup network shuts down (to protect the
panel from its own radio) and the wizard **follows the device to its new address
automatically**. Seeing "home Wi-Fi connected, setup hotspot off" is the healthy
end state, not a failure.
:::

:::caution Using only a Mistral key? Set embeddings before your first turn
Long-term memory's embedding provider defaults to OpenAI, and it is a
**set-once** choice: changing it later erases the stored memory. If Mistral is
your only key, open **Memory & Files > Memory settings** and switch the embedding
provider to **Mistral before your first conversation**, while the memory is still
empty and there is nothing to lose.
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
   space; an unseen one shows "none, click to retry".

## Optional: connect Telegram

To reach the assistant from Telegram:

1. Create a bot with [@BotFather](https://t.me/BotFather) and paste its **bot
   token** into **Assistant > Connectors > Telegram**.
2. **Message your bot once** from your own Telegram account.
3. Your message appears as a **pending approval card**. Click **Approve**. No
   chat-ID hunting.

Token and allowlist changes take effect after a restart.

:::caution Open access is off for a reason
The **Open access** checkbox lets anyone who finds your bot use it, and your API
credits. Leave it off unless you mean to run a public bot.
:::

## Optional: routing and a local model

One verified key is enough. If you want a specific provider to lead, set the
**Primary provider** and **Fallback order** under **Assistant > Models > Routing**.
The defaults are sensible.

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
- **On the device:** press and hold the **on-screen mic bar** to talk. The ring
  breathes red while listening, then shows a spinner while it transcribes.
  Release to send.

That is it, the device is live. From here, the
[web UI reference](webui-reference.md) walks every settings tab, and
[Orchestrator World](../guides/orchestrator-world.md) explains memory, sessions,
and the tool loop.

---

## Signing in from another browser

First-time setup never makes you handle the access token; the wizard carried it
for you. For a **later** browser, or a device configured by older firmware:

1. **Scan the sign-in QR** on the device screen with your phone. It opens the
   settings page already signed in.
2. If you browse in without it, a **Sign in to Nimbus** page appears. Scan the QR
   (on the device: **Settings > Connectivity > Sign-in QR**). You identify once
   per browser; the device remembers it after that.

<details>
<summary>The screen is unavailable and I cannot scan or read the code</summary>

Connect the board's **UART** port and run:

```bash
python3 tools/setup_device.py --show-token
```

It identifies the board, prints only the existing sign-in token, and restores the
firmware afterward. It does not erase saved settings. Then open
`http://<device-address>/?t=<token>`.

</details>
