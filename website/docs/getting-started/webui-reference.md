---
title: "Web UI reference"
sidebar_label: "Web UI reference"
description: "Every destination of the Nimbus web app: Home, Chat, Memory, Assistant, and Device, plus global search and sign-in."
---
<!-- audience: user -->

# Web UI Reference

The device serves its own web app at `http://nimbus.local` (or its LAN IP) from
any browser, nothing to install. Five destinations sit in a left sidebar on
desktop, collapse to icons in a narrow window, and dock as a bottom bar on a
phone. The content column centers up to 1,280 px.

| Destination | What it's for |
|---|---|
| [Home](#home) | Status tiles, quick actions, active sessions, alerts |
| [Chat](#chat) | Message the assistant, drop files into the conversation |
| [Memory](#memory) | Directive, long-term memory, scratchpad, and files |
| [Assistant](#assistant) | Models, connectors, tools, skills, routines, usage, safety (seven exclusive subtabs) |
| [Device](#device) | Display, sound, battery, network, updates, cloud, danger zone |

Controls that sit outside the destinations:

- **Search** - a command palette over everything (see [Global search](#global-search)).
- **Mode switch** - **Notifier** / **Orchestrator**, at the top of the sidebar.
  Switching modes restarts the device.
- **? buttons** - many fields carry a small **?** next to their label. Click it
  to expand a short explanation inline; click again to hide it.

Every action shows its progress: a pending state, then a result (success,
nothing found, or an error that names the next step). Nothing just disappears.

On a fresh or factory-reset device started in **Orchestrator**, a **First-time
setup** wizard opens over the setup network and walks through the display, Wi-Fi,
one provider key, and a few optional steps, then finishes into Home with a "what
next" card. Notifier intentionally keeps Wi-Fi and this web UI off. See
**[First-time setup](first-time-setup.md)**.

## Signing in

The page shows nothing until the browser is signed in. Normally you scan the
device's **Sign-in QR** (**Device > Connectivity** on the screen): it signs the
browser in with a one-time code, nothing to copy or type. Once per browser; the
credential is stored locally and sent as a request header, never in the URL.

If a QR cannot be scanned, the full-screen **Sign in to Nimbus** gate also
accepts the **Device sign-in code**, shown on the device screen. Generating a new
code (**Device > Connectivity > Generate New Code**) signs out every browser
except the one that requested it. The model behind all this is in
**[Security posture](../guides/security.md)**.

## Global search

Press **Ctrl/Cmd+K** or **/** (or click **Search** in the sidebar) to open one
search over everything: destinations and actions, files, long-term memory, active
sessions, and the device's embedded documentation. Results are grouped by source;
Up/Down move the selection, Enter opens it, Esc closes. The fastest way to a
setting, file, or memory without hunting through tabs.

---

## Home

Health, activity, and anything that needs your attention. The panel refreshes
every few seconds.

- **Tiles** - live figures with their scale and thresholds: free RAM, PSRAM,
  battery (corrected voltage and time-left estimate), die temperature, and the
  current mode with its active-session count.
- **Quick actions** - one-click jumps to the things people do most: open Chat,
  add a file, providers and models, check for updates.
- **Active sessions** - the live table of what the assistant is running right
  now, each row named by its session with its provider, model, category, and
  state.
- **Health** - a component roster from `/api/health`, each row colored by state
  (**ok / degraded / absent / unknown**) with a one-line detail. Four probe
  buttons exercise hardware on demand: **Mic test**, **Speaker test**,
  **Loopback** (plays a tone and checks the mic hears it), and **Probe SD**
  (re-checks the card and upgrades the storage tier live).

Background sessions end to end: **[Sub-sessions](../guides/sub-sessions.md)**.

---

## Chat

Message the assistant from the browser and see its replies; replies also go to
your Telegram, if connected. History re-syncs from the device's canonical store
and reaches back months, so the assistant can recall something you told it long
ago.

**Attach files** by dropping them onto the conversation or with the **Attach**
button - images, text, Markdown, CSV, logs, JSON, and PDF. The upload shows a
progress bar and then lands as a message in the log, saved to Files under the
`chat` project, so the assistant can work with it.

---

## Memory

What the device knows and the files it has made - stored on its SD card, not in a
cloud. Three groups:

- **Memory** - the **Directive** (your standing instructions to the assistant,
  up to 600 characters; only you can change it) and **Assistant memory** (notes
  the assistant keeps for itself between conversations; viewable and clearable).
- **Long-term memory** - the associative store the assistant searches by meaning:
  **Recall** (search, **Deduplicate**, **Delete Temporary**), the **Scratchpad**
  (the assistant's working notes), **Recall tuning** (memories per turn, relevance
  threshold, storage limit - at the limit, the least valuable memory is dropped),
  and the **Embedding model**, which is verified with a real embedding call on
  save and locks once vectors are stored - changing it erases and re-embeds the
  store, and the UI warns first.
- **Files** - files persist across restarts and are never deleted automatically.
  The caption shows the real card size and free space plus the file quota. The
  assistant saves reports here with `artifact.save`; you can filter by project,
  view, download (the token rides a header, never a link), delete, or upload your
  own.

Internals: **[Orchestrator World](../guides/orchestrator-world.md)** and
**[Storage tiering](../guides/orchestrator-storage.md)**.

---

## Assistant

One page, seven exclusive subtabs - **Models**, **Connectors**, **Tools**,
**Skills**, **Routines**, **Usage**, and **Safety**. Everything the assistant can
reach and run, plus what it costs and how it is governed. Keys are set by you,
never by the model.

### Models

- **Providers & keys** - Cumulo Nimbus, Anthropic, OpenAI, Mistral, Z.ai. Keys
  are write-only (a saved key shows as "set" and is never displayed again); model
  choices unlock after the key verifies.
- **Custom endpoint** - point the assistant at any OpenAI-, Anthropic-, or
  Mistral-compatible server: base URL, wire convention, and model ID.
- **Routing** - the **Primary provider** (or Automatic), the **Fallback order**,
  and a separate **Session fallback order** for spawned sessions.
- **Voice** - the **Dictation** provider, the **Spoken replies** provider, and
  the voice itself.

### Connectors

External tools that run in your AI provider's cloud, on the assistant's own turns
and on the sessions it starts. Each connector is a card: create the credential
once, paste it in, enable it - secrets are write-only. An **Advanced** section
edits the raw JSON directly. A device-dialed **MCP** server that the assistant
adds arrives **pending your approval** and shows **Approve / Deny** on its card;
nothing is dialed until you approve it. Setup details per service:
**[Connectors](../guides/connectors.md)**.

The **Telegram** group also lives here: the bot token (from @BotFather), who can
message this device (new senders appear for one-tap approval; access is enforced
by chat ID), the **Open access** switch, and **Voice replies**.

### Tools

The live on-device tool surface - what the assistant can do right now, each tool
with a status badge. External MCP clients can call the same surface over
`POST /mcp`. A **Web search** section takes a Tavily API key so the assistant can
search the web live.

**Tool use** governs that tool surface: when enabled, the assistant uses its
tools mid-turn and iterates before answering. Caps: **Tool rounds** (1-32) and a
**Time limit** (30-3600 s), plus **Switch providers mid-turn**, **Concurrent
connections**, **Validate provider TLS certificates**, and **Capability
validation**. See the **[Turn contract](../reference/turn-contract.md)**.

### Skills

Saved instruction sets the assistant applies when it starts a matching session,
on the SD card at `/mem/skills/<id>/SKILL.md`. You create, edit, and delete them
here; a skill the assistant drafts arrives **pending your approval** and does
nothing until you approve it (here, or `/skill approve <id>` in Telegram).

### Routines

- **Routines** - tasks that run on a schedule (a morning digest, a reminder,
  nightly upkeep). Create one with a name, a prompt, and a schedule (**On an
  interval**, **Daily**, or **Weekly**). Routines the assistant creates for
  itself wait for your approval, and spending limits pause anything that
  misbehaves. Until the device clock syncs, wall-clock routines wait and the card
  offers **Sync now**.
- **Wake-ups** - a wake-up is a turn the assistant schedules for itself to follow
  up later. The policy is **Allow silently** by default; **Ask me first** holds
  each new wake-up for a single yes/no approval card - never a repeating prompt.

### Usage

Token usage reported by your providers, tracked since the device last restarted.
Token counts are actual billed usage; dollar figures are estimates. A
7/30/60-day **Spend over time** chart, per-provider **Rates**, and a monthly
**Budget** per provider (0 = unlimited; at the cap that provider is refused until
the reset day and the assistant fails over when it can).

### Safety

Download trust and guest screening. Your own messages, the web page, and voice
are always exempt.

- **Downloads** - how much trust the assistant gets to download a file from the
  web: **Off**, **Ask me per link**, **Scan, then keep**, or **Full trust**.
- **Guest moderation** - screens people other than you who reach the bot.
  **Check guest messages before answering** (a flagged or uncheckable message is
  not answered), **Check replies sent to guests** (a flagged reply is held; if the
  check cannot run the reply still goes out), and **Flag suspicious fetched
  content** (marks web content that looks like a hidden instruction as data - it
  marks, never blocks, and runs on the device at no extra cost). The message and
  reply checks each cost one moderation call per screened item.

---

## Device

Display, sound, power, network, updates, and cloud - all in one place, as
collapsible groups:

- **Mode & identity** - the operating mode (**Notifier** or **Orchestrator**;
  switching restarts the device), the **Device name**, the **Timezone**, the
  **Device clock** (set automatically from the internet; a badge shows **synced**
  or **waiting**, with **Sync now**), and the **Device sign-in code** (only needed
  when a QR cannot be scanned; tap to copy).
- **Display** - a 180-degree **Display flip** for upside-down mounts, and **Touch
  calibration** for the touch screen (or the self-calibrating panel's swap/flip
  orientation toggles). Takes effect right away.
- **Battery mode** - **Dark / Balanced / Full**, which sets the light. The
  **Theme** picks the color family, and **Preview** shows any status pattern in
  the ring simulator; **Demo on Device** plays it on the physical ring. Full
  color and motion language: **[LED experience](../guides/led-ux.md)**.
- **Customize battery mode** - every parameter starts at the selected battery
  mode's default; set a value to override it, or **Revert to Defaults**.
- **Sound** - audio checks (**Mic Meter**, **Speaker Tone**, **Loopback Test**)
  and the **Sound effects** engine (**[SFX map](../guides/sfx-map.md)**): a
  per-mode level, a sound theme, a **Volume** slider, and **Play**.
- **Battery** - appears once battery telemetry is present: voltage, estimated
  time left, discharge rate, health, and power source, plus **Calibrate Full
  Charge** (anchors 100% to a fully charged pack, with a visible result).
- **Software update** - installed and latest versions, **Check for Updates**
  (which reports whether an update was found, you are up to date, or the check
  failed), **Install Update**, and **Automatic updates**. When an update needs
  more charge than the pack has, a battery-gate message names the threshold and
  Install stays disabled. Updates are cryptographically signed and the device
  reverts on its own if a new version fails to start.
- **Connectivity** - the home-network address; the temporary setup hotspot and
  password; the **Device sign-in code** and **Generate New Code**; **Bluetooth**
  status and **Forget Paired Devices**; and **Wi-Fi** (scan, pick a network,
  **Join** - 2.4 GHz only; the one action allowed before signing in).
- **Cloud access** - reach the device from anywhere through CumuloNimbus. Pairing
  shows a dedicated card with the **Cloud link code** large and high-contrast
  beside a QR: sign in at app.cumulo-nimbus.ai, then scan the QR or enter the
  code. **Copy code** copies it. Available in Orchestrator mode.
- **Danger zone** - **Erase Storage…** wipes the SD card (typed confirmation) and
  **Factory Reset…** erases everything (Wi-Fi, API keys, the Telegram list,
  Bluetooth pairings, themes, sound settings, memory, and the device sign-in
  code) and restarts into first-time setup. Both need the exact phrase typed.
