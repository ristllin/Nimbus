# Set up the device

From first power-on to a configured device: join the setup network, walk the
onboarding wizard, land on the dashboard. About ten minutes; no serial console.

The whole flow at a glance - every step is detailed in the sections below.
On a touchscreen board the device shuts its setup hotspot down once it is on
your Wi-Fi and the wizard follows it to the new address automatically:

```mermaid
sequenceDiagram
  actor You as Your phone / laptop
  participant Dev as Nimbus
  participant LAN as Home Wi-Fi

  Note over Dev: first boot after flashing
  Dev->>Dev: broadcast setup network "Nimbus-setup"<br/>show its name + Sign-in QR on the screen
  You->>Dev: join Nimbus-setup (password shown on the device screen)
  You->>Dev: captive page opens → http://192.168.4.1<br/>(already signed in - the Sign-in QR carries the code)
  You->>Dev: wizard: pick your Wi-Fi + password (required)
  Dev->>LAN: join your home Wi-Fi
  Dev->>Dev: setup hotspot shuts down after handoff
  Dev-->>You: wizard carries you to the device's LAN address
  You->>Dev: wizard: pick a provider, paste its key, Save & Verify (required)
  opt skippable steps
    You->>Dev: display · mode · Telegram · voice · directive · device name
  end
  Dev-->>You: dashboard at the device's LAN address
```

## 1. Power on

Power the board over USB-C. What you see depends on the board's operating
mode. A board installed from the **browser flasher** always starts in
**Orchestrator**; the **command-line installer** asks which mode to start in.

- **Orchestrator** shows its own **setup Wi-Fi network name** and a Sign-in
  QR on the screen. Continue below.
- **Notifier** shows Bluetooth pairing guidance instead - there is no Wi-Fi
  or web UI in that mode. Jump to the
  **[Notifier quick start](notifier-quick-start.md)**.

## 2. Join the setup network

Two steps, in order - the same two the device screen shows:

1. **Join the setup network** shown on the screen:
   - **Network:** `Nimbus-setup` (a second device numbers itself
     `Nimbus-2-setup`, and so on)
   - **Password:** shown on the device screen (unique to your device) - or just
     scan the on-screen QR code to join automatically
2. **Open the setup page.** It usually opens on its own once you join. If
   nothing opens, visit `http://192.168.4.1` in your browser.

Do not use `nimbus.local` for this first hop: it is a LAN name and can resolve to
a different Nimbus. The setup page opens the wizard **already signed in**; there is
no code to copy.

## 3. Walk the wizard

The wizard asks for two required things and lets you skip the rest:

| Step | Required? | Notes |
|---|---|---|
| **Connect to Wi-Fi** | Yes | Scan, pick your home network, type its password. 2.4 GHz only; SSIDs are case-sensitive. |
| **Add an AI provider** | Yes - one verified key | Pick the provider, paste its API key, click **Save & Verify**. The device checks the key with the provider before you continue. Providers: Cumulo Nimbus, Anthropic, OpenAI, Mistral, Z.ai; [Get your provider key](provider-keys.md) helps you choose. Have the key ready before you start: you cannot fetch one while connected to the device's own Wi-Fi. The key stays on the device and is never shown again. |
| Choose a mode | Skippable | Keeps the mode the board started in (see [Power on](#1-power-on)). Applied when you finish; a mode change restarts the device. |
| Telegram | Skippable | Paste a bot token from @BotFather, or add it later under **Assistant → Connectors → Telegram**. |
| Voice | Skippable | Speech-to-text and text-to-speech provider: Mistral (default) or OpenAI. |
| Directive | Skippable | Standing guidance that shapes how the assistant behaves for you. Keep the default or edit it. |
| Name this device | Skippable | One name drives the setup network name, the LAN address, the Bluetooth name, and what the assistant calls itself. Applies after the next restart. |

On a **touchscreen board**, keep the wizard open while the device joins your
Wi-Fi: the setup network shuts down after handoff (to protect the display from
its own radio), and the wizard carries you to the device's new LAN address
automatically. "Home Wi-Fi connected + setup hotspot off" is the healthy end
state, not a failure.

:::danger Using only a Mistral key? Set the embedding provider before your first conversation
Long-term memory embeds with **OpenAI** by default. Once the first memory is
stored the choice locks: changing the provider, model, or dimensions later means
erasing every stored memory and starting fresh. If Mistral is your only key, open
**Memory → Embedding model**, set **Provider** to **Mistral**, and click **Save**
before your first conversation, while the memory is still empty. Anthropic and
Z.ai keys cannot embed; semantic memory needs a Cumulo Nimbus, OpenAI, or Mistral key.
:::

## What works with which provider

Any one verified provider runs the assistant. A few features are tied to a
specific provider or key:

| Feature | Mistral | OpenAI | Anthropic | Z.ai |
|---|---|---|---|---|
| Conversation, tools, sub-agents, routines | ✓ | ✓ | ✓ | ✓ |
| Dictation (speech-to-text) | ✓ *(default)* | ✓ | - | - |
| Spoken replies as Telegram audio (when the device cannot play them) | ✓ *(default)* | ✓ | - | - |
| Spoken replies on the device speaker | ✓ *(default)* | ✓ | - | - |
| Image generation (`image.generate`) | - | ✓ | - | - |
| Long-term memory embeddings | ✓ *(switch before first use)* | ✓ *(default)* | - | - |
| Web search | needs a separate **Tavily** key (Assistant → Tools → Web search) | same | same | same |

A Cumulo Nimbus key routes to these providers, so each feature works when the
matching provider is enabled for your account.

## 4. The dashboard

The wizard ends on the device's **dashboard** at its LAN address: health,
battery, memory, and sessions at a glance. The tabs are **Home**, **Chat**,
**Memory**, **Assistant** (Models, Connectors, Tools, Skills, Routines, Usage,
Safety), and **Device**. You sign in once per browser; the Sign-in QR on the
device screen gets any new browser in.

Full tab-by-tab detail: the
[web UI reference](https://docs.cumulo-nimbus.ai/getting-started/webui-reference).

Next: **[Your first conversation](first-conversation.md)**.

---

*How it works → [Orchestrator World](../orchestrator-world.md)*
