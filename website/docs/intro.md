---
title: "Nimbus"
sidebar_label: "Overview"
slug: /
description: "A DIY ESP32-S3 desk device - an ambient status light for AI coding sessions, and a self-hosted AI assistant with memory, voice, and Telegram."
---

# Nimbus

<!-- HERO PHOTO PLACEHOLDER: assembled device on a desk, ring lit, screen
     showing the status screen. Drop the image into website/static/img/ and
     embed it here (another workstream is shooting hardware photos). -->

Nimbus is a small, battery-capable desk device you build yourself: a 45-LED
light ring, a color touchscreen, and a microphone +
speaker on an ESP32-S3. It sits at the edge of your attention and uses light
first, screen second, sound last - dark and silent until something actually
needs you.

The parts cost about **$60–75** ([bill of materials](guides/hardware-bom.md)),
the firmware is in this repository, and everything runs on keys you bring -
no cloud account, no subscription, nothing phones home.

:::info License - source-available, noncommercial
Nimbus is **source-available under noncommercial licenses**: code under the
[PolyForm Noncommercial License 1.0.0](https://polyformproject.org/licenses/noncommercial/1.0.0/),
documentation and hardware under
[CC BY-NC-SA 4.0](https://creativecommons.org/licenses/by-nc-sa/4.0/). Build
one, modify it, share your fork - just not commercially. Details in the
repository's `LICENSE`, `LICENSE-docs`, and `NOTICE`.
:::

## One device, two modes - choose your path

The same hardware runs in one of two operating modes. Pick one at setup;
switch any time (switching restarts the device).

### Notifier - a status light for AI coding sessions

Your computer reports coding-session state over an encrypted, paired
Bluetooth link, and the ring shows what every session is doing - running,
waiting on you, done, errored - each state with its own color role and
motion. Glanceable from across the room, no screen focus stolen.

**Start here → [Notifier quick start](quick-start/notifier-quick-start.md)**

### Orchestrator - a self-hosted AI assistant

A standalone assistant powered by a hosted LLM provider (Mistral, OpenAI,
Anthropic, or your own endpoint) with your own API key. Reach it over
Telegram, voice (hold-to-talk on the device), or the browser chat. It keeps
long-term memory on its SD card, runs tools mid-turn, spawns background
research agents, and runs scheduled routines - and it updates itself over the
air with signed releases.

**Start here → [What you need](quick-start/what-you-need.md)**

## Two display builds

A Nimbus is built in one of two display configurations. The hand-built Solide S3
drives a color touchscreen alongside its LED ring; the Freenove CYD is an
off-the-shelf all-in-one module.

| Configuration | Display | Input |
|---|---|---|
| **[Touch TFT](guides/hardware-touch-tft.md)** (Solide S3) | 2.8" color touchscreen (ILI9341, 240×320) | Resistive touch (XPT2046) |
| **[All-in-one (Freenove CYD)](guides/hardware-all-in-one-cyd.md)** | 2.8" color touchscreen (ILI9341, 240×320) | Capacitive touch |

Everything else - ring, audio, SD card, battery - is common. Pinouts and wiring:
[hardware reference](guides/hardware.md).

## Where to go

- **Build one** - [What you need](quick-start/what-you-need.md) →
  [BOM](guides/hardware-bom.md) → the
  [build guides](guides/hardware.md) →
  [Flash](quick-start/flash.md) → [Set up](quick-start/setup-wizard.md).
- **Use one** - [Modes & signals](guides/modes-and-signals.md) explains every
  light and sound; the
  [web UI reference](getting-started/webui-reference.md) walks every settings
  tab.
- **Understand it** - [Architecture](guides/architecture.md), then
  [Turn anatomy](guides/turn-anatomy.md) (what the model actually sees) and
  [Orchestrator World](guides/orchestrator-world.md) (the memory system).
- **Fork it** - the [development guide](contributing/development.md) and
  [self-hosted OTA](contributing/self-hosted-ota.md) (shipping firmware to
  your own devices).

:::note
These pages are a published *view* of the firmware repo's `docs/` tree. The
canonical source is the Markdown in the repository - see the
[docs map](https://github.com/ristllin/Nimbus/blob/main/docs/README.md).
:::
