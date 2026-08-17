---
title: "Web UI reference"
sidebar_label: "Web UI reference"
description: "Every tab of the Nimbus web UI: Dashboard, Sessions, Chat, Memory & Files, Capabilities, Usage, Routines, and Settings."
---

# Web UI Reference

The device serves its own web app, reachable at `http://nimbus.local` (or the
device's LAN IP) from any browser - nothing to install. On a desktop the tabs
sit in a left sidebar below the brand and the mode switch; on a phone the same
tabs dock as a bottom bar.

| Tab | What it's for |
|---|---|
| [Dashboard](#dashboard) | Health, live figures, hardware probes |
| [Sessions](#sessions) | What the assistant is running right now |
| [Chat](#chat) | Message the assistant from the browser |
| [Memory & Files](#memory--files) | What it knows and the files it has made |
| [Capabilities](#capabilities) | Providers, tools, connectors, skills |
| [Usage](#usage) | Token spend, rates, budgets |
| [Routines](#routines) | Tasks that run on a schedule |
| [Settings](#settings) | Mode, light, sound, power, connectivity |

Two controls sit outside the tabs:

- **Mode switch** - **Notifier** / **Orchestrator**, at the top of the sidebar.
  Switching modes restarts the device.
- **? buttons** - many fields carry a small **?** next to their label. Click it
  to expand a short explanation inline; click again to hide it.

On a fresh or factory-reset device started in **Orchestrator**, a **First-time
setup** wizard opens over the setup network and walks through Wi-Fi, one
provider key, and a few optional steps. Notifier intentionally keeps Wi-Fi and
this web UI off. See **[First-time setup](first-time-setup.md)**.

## Signing in

The page shows nothing until the browser is signed in. Normally you just scan
the device's sign-in QR (**Settings > Connectivity > Sign-in QR** on the
device): the QR carries the credential, so there is no token to copy or type.
You do this once per browser - the credential is stored locally and attached to
every request from then on.

If a QR cannot be scanned, the full-screen **Sign in to Nimbus** gate also
accepts the recovery access token. Select **Settings > Connectivity > Sign-in
code** on the device to show the complete code on its own screen; it is not
truncated into the compact menu row.

Generating a new token (**Settings → Connectivity**) signs out every browser
except the one that requested it. The model behind all this is in
**[Security posture](../guides/security.md)**.

---

## Dashboard

Health, activity, and anything that needs your attention. The panel refreshes
every few seconds.

- **Tiles** - live figures with their scale and thresholds: free RAM, PSRAM,
  battery (corrected voltage and time-left estimate), and the current mode with
  its active-session count.
- **Active sessions** - what the assistant is running right now, at a glance.
- **Health** - a component roster from `/api/health`, each row colored by state
  (**ok / degraded / absent / unknown**) with a one-line detail. Four probe
  buttons exercise hardware on demand: **Mic test**, **Speaker test**,
  **Loopback** (plays a tone and checks the mic hears it), and **Probe SD**
  (re-checks the card and upgrades the storage tier live).

---

## Sessions

Everything the assistant is working on right now - the live table of running
sessions, each with its provider, model, category, and state. Background
sessions end to end: **[Sub-sessions](../guides/sub-sessions.md)**.

---

## Chat

Message the assistant from the browser and see its replies. Replies also go to
your Telegram, if connected. Conversation history is re-synced from the
device's canonical store.

---

## Memory & Files

What the device knows and the files it has made - stored on its SD card, not
in a cloud. Three groups:

- **Memory** - the **Directive** (your standing instructions to the assistant,
  up to 600 characters; only you can change it) and **Assistant memory** (notes
  the assistant keeps for itself between conversations; viewable and
  clearable).
- **Long-term memory** - the associative store the assistant searches by
  meaning: **Recall** (search, **Deduplicate**, **Delete Temporary**), the
  **Scratchpad** (the assistant's working notes), **Recall tuning** (memories
  per turn, relevance threshold, storage limit - at the limit, the least
  valuable memory is dropped), and the **Embedding model**, which is verified
  with a real embedding call on save and locks once vectors are stored -
  changing it erases and re-embeds the store, and the UI warns first.
- **Files** - files persist across restarts and are never deleted
  automatically. The assistant saves reports here with `artifact.save`; you
  can filter by project, download, delete, or upload your own.

The conversation history the assistant can search reaches back months - well
past the recent messages loaded at startup - so it can recall something you told
it long ago instead of saying it doesn't remember.

Internals: **[Orchestrator World](../guides/orchestrator-world.md)** and
**[Storage tiering](../guides/orchestrator-storage.md)**.

---

## Capabilities

Everything the device can reach and run - providers, tools, connectors, and
skills. Keys are set by you, never by the model. Four sub-tabs: **Connectors ·
Tools · Models · Skills**.

### Connectors

External tools that run in your AI provider's cloud, on the assistant's own
turns and on the sessions it starts. Each connector is a card: create the
credential once, paste it in, enable it - secrets are write-only. An
**Advanced** section edits the raw JSON directly. Setup details per service:
**[Connectors](../guides/connectors.md)**.

The **Telegram** group also lives here: the bot token (from @BotFather), who
can message this device (new senders appear for one-tap approval; access is
enforced by chat ID), the **Open access** switch (anyone who finds the bot can
use it - and your API credits), and **Voice replies** (when on, the assistant
may answer with a spoken voice note when that fits).

### Tools

The live on-device tool surface - what the assistant can do right now, each
tool with a status badge. External MCP clients can call the same surface over
`POST /mcp`. A **Web search** section takes a Tavily API key so the assistant
can search the web live.

### Models

- **Providers & keys** - OpenAI, Anthropic, Mistral. Keys are write-only (a
  saved key shows as "set" and is never displayed again); model choices unlock
  after the key verifies.
- **Custom endpoint** - point the assistant at any OpenAI-, Anthropic-, or
  Mistral-compatible server: base URL, wire convention, and model ID. An
  `http://` base uses plain HTTP for a LAN server (for example a local
  Ollama).
- **Routing** - the **Primary provider** (or Automatic, the first verified
  provider in your fallback order), the **Fallback order**, and a separate
  **Session fallback order** for the sessions the assistant spawns.
- **Voice** - the **Dictation** provider (microphone and Telegram voice
  notes), the **Spoken replies** provider, and the voice itself. Works as soon
  as the provider's key is set, independent of chat verification.
- **Tool use** - when enabled, the assistant uses its tools mid-turn and
  iterates before answering; off means a single step. Caps: **Tool rounds**
  (1–32) and a **Time limit** in seconds (30–3600). **Switch providers mid-turn**
  lets a turn that loses its provider partway through continue on the next
  verified provider, carrying the work so far. **Concurrent connections** and
  **Validate provider TLS certificates** live here too - leave validation on
  unless you run a self-hosted server with a self-signed certificate. See the
  **[Turn contract](../reference/turn-contract.md)**.

### Skills

Saved instruction sets the assistant applies when it starts a matching session.
Skills live on the SD card at `/mem/skills/<id>/SKILL.md`. Two things can write
them:

- **You**, from this tab - created, edited, and deleted here at any time.
- **The assistant**, which can draft a skill it wants to reuse later. An
  assistant-written skill arrives **pending your approval** and does nothing
  until you approve it - from the card here, or by replying `/skill approve <id>`
  in Telegram. Approval is deliberately after the fact: a skill saved during one
  session is an investment in future sessions, never something that changes the
  session that wrote it. You can delete a skill of either origin; the assistant
  can only delete its own. Built-in skills (such as the deep-research routine)
  can't be overwritten.

Click a skill card to open it in the editor.

---

## Usage

Token usage reported by your providers, tracked since the device last
restarted. Token counts are actual billed usage; dollar figures are estimates.

- **Spend over time** - a 7/30/60-day chart, in tokens or estimated dollars.
- **Rates** - what each provider charges, used only for the estimates.
- **Budgets** - a monthly cap per provider (0 = unlimited). At the cap, that
  provider's turns and searches are refused until the reset day; the assistant
  fails over to another in-budget provider when it can.

---

## Routines

Tasks that run on a schedule - a morning digest, a reminder, nightly upkeep.
Create one with a name, a prompt, and a schedule (**On an interval**,
**Daily**, or **Weekly**). Routines the assistant creates for itself wait for
your approval before they can run, and spending limits pause anything that
misbehaves. From Telegram, the owner manages them with the `/loops` and
`/loop` commands.

---

Until the device clock syncs (it sets itself from the internet), daily and
weekly routines wait - the tab says so and offers **Sync now**. Once synced,
each routine row shows when it next fires ("in 6h").

## Settings

Mode, light, sound, power, and connectivity - all in one place, as collapsible
groups:

- **Mode & identity** - the operating mode (**Notifier - status light** or
  **Orchestrator - AI assistant**; switching restarts the device), the
  **Device name** (one name drives the setup Wi-Fi network, the network
  address, Bluetooth, and what the assistant calls itself; applies after
  restart), the **Timezone** (sets when daily and weekly routines - including
  nightly memory upkeep - fire; POSIX format with a picker of common zones,
  blank = UTC, applies immediately), the **Device clock** (set automatically
  from the internet once Wi-Fi connects - there is no manual clock; a badge
  shows **synced** or **waiting for internet time**, with a **Sync now**
  retry), and the **Recovery access token** (only needed when a QR cannot be
  scanned; tap to copy).
- **Battery mode** - **Dark / Balanced / Full**; the battery mode sets the
  light. Dark keeps the lights off except a red error breathe, Balanced shows
  a single soft cue, Full gives every job a color arc. The **Theme** picks the
  color family, and **Preview** shows any status pattern in the ring simulator
  - **Demo on Device** plays it on the physical ring for a few seconds. Full
  color and motion language: **[LED experience](../guides/led-ux.md)**.
- **Customize this mode** - every parameter starts at the mode's default; set
  a value to override it, or **Revert to Defaults** to clear all overrides.
- **Sound** - audio checks (**Mic Meter**, **Speaker Tone**, **Loopback
  Test**) and the **Sound effects** engine (**[SFX map](../guides/sfx-map.md)**):
  a per-mode level (**Off / Low / Medium / High**), a **sound theme**, a
  **Volume** slider, and a **Play** button to audition any
  clip.
- **Battery** - appears once battery telemetry is present: voltage, estimated
  time left, discharge rate, health, and power source, plus **Calibrate Full
  Charge** (anchors 100% to a fully charged pack). The protection thresholds
  and the two override checkboxes carry visible warnings - both overrides
  reset at restart.
- **Software update** - installed and latest versions, **Check for Updates**,
  **Install Update**, and **Automatic updates**. Updates are cryptographically
  signed and the device reverts on its own if a new version fails to start.
- **Connectivity** - everything needed to reach the device: its home-network
  address; the separate, temporary setup hotspot and password; the recovery
  access token and **Generate New Token**; **Bluetooth** status, paired
  devices, and **Forget Paired Devices**; and **Wi-Fi** (scan, pick a network,
  **Join** - 2.4 GHz networks only). Joining Wi-Fi is the one action allowed
  before signing in.
- **Danger zone** - **Factory Reset…** erases everything (Wi-Fi, API keys, the
  Telegram list, Bluetooth pairings, themes, sound settings, memory, and the
  access token) and restarts into first-time setup.
