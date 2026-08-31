---
title: "Nimbus"
sidebar_label: "Overview"
slug: /
description: "Open firmware that turns an inexpensive ESP32-S3 board into a personal AI assistant. Two modes on one build: an Orchestrator you reach over Telegram, voice, or a local web UI with long-term memory, and a Notifier that tracks your coding agents on an LED ring over Bluetooth. Two hardware builds: a hand-built board with a physical ring, or the Freenove all-in-one."
---

# Nimbus

<div class="home-hero">
  <div class="home-hero__media">
    <img src="/img/hardware/hero-render-placeholder.webp" alt="Nimbus device render (placeholder)" />
  </div>
  <div class="home-hero__lead">
    <p>Nimbus is an open-firmware personal assistant that lives on your desk. In <b>Orchestrator</b> mode you reach it the way you already work: over Telegram, by hold-to-talk on the device, or from a local web UI. It remembers across conversations, runs your routines on its own clock, and uses tools mid-turn to get real work done. The agent runs on the device itself, an inexpensive commodity ESP32-S3 board, and drives the model you choose: a hosted AI lab, or an LLM you host yourself.</p>
    <p>In <b>Notifier</b> mode the same build is a visual way to track the coding agents running on your machine. Over an encrypted Bluetooth link, each session takes a spot on the LED ring, so a glance tells you what finished and what needs you, with no terminal to watch.</p>
    <p>Your source data stays on the device: memories, routines, and raw data live locally, not in a cloud account. It is local, not offline: the model call still goes out to whichever provider you pick, but the assistant itself is a small, always-on server you can leave next to your Wi-Fi router. Bring your own provider key (Mistral, OpenAI, Anthropic, or any OpenAI-compatible endpoint). Source-available under noncommercial licenses.</p>
    <div class="home-hero__cta">
      <a class="button button--primary button--lg" href="/quick-start/what-you-need">Get started</a>
      <a class="button button--secondary button--lg" href="/flash">Flash from your browser</a>
    </div>
  </div>
</div>

:::info License - source-available, noncommercial
Code is under the
[PolyForm Noncommercial License 1.0.0](https://polyformproject.org/licenses/noncommercial/1.0.0/);
documentation and hardware are under
[CC BY-NC-SA 4.0](https://creativecommons.org/licenses/by-nc-sa/4.0/). Build it,
modify it, and share your fork, for any noncommercial use. Details in the
repository's `LICENSE`, `LICENSE-docs`, and `NOTICE`.
:::

## Two builds, one firmware

Nimbus targets two hardware builds. Both flash the same firmware and expose the
same features. They differ in assembly effort and in whether the status ring is
a physical ring or rendered on the screen.

<div class="variant-grid">
  <div class="variant-card">
    <div class="variant-card__media">
      <img src="/img/hardware/classic-desk.webp" alt="The hand-built Nimbus board in a 3D-printed case" />
    </div>
    <div class="variant-card__body">
      <p class="variant-card__kicker">Hand-built</p>
      <h3>Classic Nimbus</h3>
      <p>An ESP32-S3-DevKitC-1 on a custom carrier PCB in a 3D-printed case, driving a discrete WS2812B LED ring and a 2.8-inch touchscreen. Involves soldering and assembly.</p>
      <ul class="variant-card__specs">
        <li>ESP32-S3-DevKitC-1 (N16R8) + carrier PCB</li>
        <li>WS2812B LED ring</li>
        <li>2.8" ILI9341 240x320, resistive touch (XPT2046)</li>
        <li>Mic, speaker, microSD, optional 2S battery</li>
        <li>Around $60 to $75 in parts</li>
      </ul>
      <p><a href="/guides/hardware-touch-tft">Build guide &rarr;</a></p>
    </div>
  </div>
  <div class="variant-card">
    <div class="variant-card__media">
      <img src="/img/hardware/light-render.webp" alt="Render of Nimbus Light in its printed case on the desk stand" />
    </div>
    <div class="variant-card__body">
      <p class="variant-card__kicker">All-in-one</p>
      <h3>Nimbus Light (Freenove CYD)</h3>
      <p>A single Freenove ESP32-S3 module with the display, capacitive touch, microSD, mic, and speaker on one board. No wiring, no soldering. There is no discrete ring; the status arcs render on the panel.</p>
      <ul class="variant-card__specs">
        <li>Freenove ESP32-S3 Display (FNK0104B)</li>
        <li>2.8" ILI9341 240x320, capacitive touch (FT6336U)</li>
        <li>Status ring rendered on-screen</li>
        <li>ES8311 mic + speaker, microSD, optional 1S battery</li>
        <li>Around $20 to $25, one board</li>
      </ul>
      <p><a href="/guides/hardware-all-in-one-cyd">All-in-one guide &rarr;</a></p>
    </div>
  </div>
</div>

Every panel is a 2.8" ILI9341 (240x320) on an ESP32-S3 N16R8 (16 MB flash,
8 MB PSRAM); the Freenove also comes in 3.5" and 4.0" sizes. Pinouts and wiring
are in the [hardware reference](guides/hardware.md); the parts list is in the
[bill of materials](guides/hardware-bom.md).

## One device, two modes

The same firmware boots into one of two modes. Pick one at flash time and switch
later from the settings menu; a switch restarts the device.

### Notifier

A broker on your computer speaks the `nimbus-notify` protocol to the device over
an encrypted, paired Bluetooth link. Each active coding session maps to a ring
segment, and its color and animation encode the session state: running, waiting
for input, done, errored. On Nimbus Light the segments render on the screen
instead of a physical ring.

**[Notifier quick start](quick-start/notifier-quick-start.md)**

### Orchestrator

A self-hosted agent backed by a hosted LLM provider and your own key. Reach it
over Telegram, hold-to-talk on the device, or the local web UI. It keeps
long-term memory on the SD card, runs tools mid-turn, spawns background
sub-agents, runs scheduled routines, and updates itself over the air with signed
releases.

**[What you need](quick-start/what-you-need.md)**

## What stays on the device

Nimbus keeps the assistant's source data local. However you run it, the memory,
your routines, and the tool actions all happen on the device you own. Only the
model call ever leaves, and only when you point it at a hosted provider.

<div class="boundary-grid">
  <div class="boundary">
    <p class="boundary__kicker">Self-hosted model</p>
    <div class="boundary__zone">
      <span class="boundary__label">Your home network</span>
      <div class="bnode">
        <b>Nimbus</b>
        <span>memory + routines</span>
      </div>
      <span class="boundary__wire">&#8595; model call</span>
      <div class="bnode">
        <b>Your computer</b>
        <span>self-hosted LLM, MCP tools</span>
      </div>
    </div>
    <p class="boundary__cap">Run a model you host yourself and even the model call stays on your network. Nothing crosses out.</p>
  </div>
  <div class="boundary">
    <p class="boundary__kicker">Hosted provider</p>
    <div class="boundary__zone">
      <span class="boundary__label">Your device</span>
      <div class="bnode">
        <b>Nimbus</b>
        <span>memory + routines + connector tools</span>
      </div>
    </div>
    <div class="boundary__cross">
      <span class="boundary__wire boundary__wire--out">&#8595; model call</span>
      <div class="bnode bnode--remote">
        <b>AI lab</b>
        <span>Mistral, OpenAI, or Anthropic</span>
      </div>
    </div>
    <p class="boundary__cap">Point it at a hosted provider and only the model call leaves. Your memory, routines, and actions stay on the device.</p>
  </div>
</div>

<div class="cumulo-callout">
  <h2>Cumulo Nimbus - the first-party service</h2>
  <p>Nimbus runs entirely on your own keys and network. <a href="https://app.cumulo-nimbus.ai">Cumulo Nimbus</a> is the optional first-party service the firmware integrates with directly, when you want it. It is off by default.</p>
  <ul>
    <li><b>One key across providers.</b> A <a href="/cloud/cumulo-key">Cumulo key</a> draws on a single prepaid balance that routes to Mistral, OpenAI, and Anthropic. Paste it under <b>Assistant &rarr; Models</b>; it also works from your own code as an OpenAI- or Anthropic-compatible base URL.</li>
    <li><b>Remote access without port forwarding.</b> <a href="/cloud/cloud-access">Cloud access</a> opens one outbound secure tunnel so you can reach the device's own web UI from anywhere. Pair it with a Cloud link code shown on the device.</li>
  </ul>
  <p>Both are opt-in, both verify ownership before anything reaches the device, and neither replaces the local web UI.</p>
</div>

## Where to go

- **Build one:** [What you need](quick-start/what-you-need.md), then the
  [bill of materials](guides/hardware-bom.md), the
  [build guides](guides/hardware.md),
  [flash](quick-start/flash.md), and [set up](quick-start/setup-wizard.md).
- **Use one:** [Modes & signals](guides/modes-and-signals.md) documents every
  light and sound; the [web UI reference](getting-started/webui-reference.md)
  covers every settings tab.
- **Understand it:** [Architecture](guides/architecture.md), then
  [turn anatomy](guides/turn-anatomy.md) (what the model sees) and
  [Orchestrator World](guides/orchestrator-world.md) (the memory system).
- **Fork it:** the [development guide](contributing/development.md) and
  [self-hosted OTA](contributing/self-hosted-ota.md) (shipping firmware to your
  own devices).

:::note
These pages are a published *view* of the firmware repo's `docs/` tree. The
canonical source is the Markdown in the repository; see the
[docs map](https://github.com/ristllin/Nimbus/blob/main/docs/README.md).
:::
