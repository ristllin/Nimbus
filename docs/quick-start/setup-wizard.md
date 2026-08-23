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
  Dev->>Dev: broadcast setup network "Nimbus-setup"<br/>show its name + sign-in QR on the panel
  You->>Dev: join Nimbus-setup (password shown on the device screen)
  You->>Dev: captive page opens → http://192.168.4.1<br/>(already signed in - the QR carries the token)
  You->>Dev: wizard: pick your Wi-Fi + password (required)
  Dev->>LAN: join your home Wi-Fi
  Dev->>Dev: setup hotspot shuts down after handoff
  Dev-->>You: wizard carries you to the device's LAN address
  You->>Dev: wizard: provider key + Verify (required)
  opt skippable steps
    You->>Dev: operating mode · Telegram · voice · device name
  end
  Dev-->>You: dashboard at the device's LAN address
```

## 1. Power on

Power the board over USB-C. What you see depends on the mode chosen during
flashing:

- **Orchestrator** shows its own **setup Wi-Fi network name** and a sign-in
  QR on the display. Continue below.
- **Notifier** shows Bluetooth pairing guidance instead - there is no Wi-Fi
  or web UI in that mode. Jump to the
  **[Notifier quick start](notifier-quick-start.md)**.

## 2. Join the setup network

On your phone or laptop, join the Wi-Fi network shown on the panel:

- **Network:** `Nimbus-setup` (a second device numbers itself
  `Nimbus-2-setup`, and so on)
- **Password:** shown on the device screen (unique to your device) - or just scan the QR to join automatically

A **captive page opens automatically**; if it doesn't, browse to
**`http://192.168.4.1`**. Don't use `nimbus.local` for this first hop - it is
a LAN name and can resolve to a different Nimbus. The captive page opens the
wizard **already signed in**; there is no token to copy.

## 3. Walk the wizard

The wizard asks for two required things and lets you skip the rest:

| Step | Required? | Notes |
|---|---|---|
| **Connect to Wi-Fi** | Yes | Scan, pick your home network, type its password. 2.4 GHz only; SSIDs are case-sensitive. |
| **Provider key** | Yes - one verified key | Paste an API key and click **Verify**. **Mistral is the recommended starting point** - free tier, and the voice default; OpenAI and Anthropic work too. Keys are write-only: shown as "set", never displayed again. |
| Operating mode | Skippable | Defaults to what the installer chose; changeable later (a change restarts the device). |
| Telegram | Skippable | Add a bot token later under Capabilities → Connectors → Telegram. |
| Voice | Skippable | Dictation and spoken replies default to Mistral. |
| Device name | Skippable | One name drives the setup network SSID, the LAN address, the Bluetooth name, and what the assistant calls itself. |

On a **touchscreen board**, keep the wizard open while the device joins your
Wi-Fi: the setup network shuts down after handoff (to protect the panel from
its own radio), and the wizard carries you to the device's new LAN address
automatically. "Home Wi-Fi connected + setup hotspot off" is the healthy end
state, not a failure.

:::danger Using only a Mistral key? Set embeddings BEFORE first use
The long-term memory's **embedding provider defaults to OpenAI**, and it is a
**set-once** choice: changing it later **erases the vector memory** (every
stored embedding becomes unreadable to the new model). If Mistral is your only
key, open **Memory & Files → Memory settings** and switch the embedding
provider to **Mistral before your first conversation** - while the memory is
still empty and there is nothing to lose.
:::

## What works with which provider

Any one verified provider runs the assistant. A few features are tied to a
specific provider or key - honestly, today:

| Feature | Mistral | OpenAI | Anthropic |
|---|---|---|---|
| Conversation, tools, sub-agents, routines | ✓ | ✓ | ✓ |
| Dictation (speech-to-text) | ✓ *(default)* | ✓ | - |
| Spoken replies over Telegram | ✓ *(default)* | ✓ | - |
| Spoken replies on the device speaker | - | ✓ | - |
| Image generation (`image.generate`) | - | ✓ | - |
| Long-term memory embeddings | ✓ *(switch before first use)* | ✓ *(default)* | - |
| Web search | needs a separate **Tavily** key (Usage → Downloads & search) | same | same |

## 4. The dashboard

The wizard ends on the device's **dashboard** at its LAN address - health,
battery, memory, and sessions at a glance, with tabs for **Capabilities**
(providers, connectors, skills), **Memory & Files**, **Routines**, and
**Settings**. You sign in once per browser; the sign-in QR on the device
screen gets any new browser in.

Full tab-by-tab detail: the
[web UI reference](https://docs.cumulo-nimbus.ai/getting-started/webui-reference).

Next: **[Your first conversation](first-conversation.md)**.

---

*How it works → [Orchestrator World](../orchestrator-world.md)*
