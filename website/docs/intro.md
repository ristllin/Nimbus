---
title: "Nimbus"
sidebar_label: "Overview"
slug: /
description: "A DIY ESP32-S3 desk device - an ambient status light for AI coding sessions, and a self-hosted AI assistant with memory, voice, and Telegram. Two builds: the hand-built classic with a real LED ring, or the Freenove all-in-one Nimbus Light."
---

# Nimbus

<div class="home-hero">
  <div class="home-hero__media">
    <video autoplay muted loop playsinline poster="/img/hardware/classic-ring-poster.webp">
      <source src="/img/hardware/classic-ring.mp4" type="video/mp4" />
      <img src="/img/hardware/classic-ring-poster.webp" alt="A Nimbus device with its LED ring lit, each arc a live coding session" />
    </video>
  </div>
  <div class="home-hero__lead">
    <p>A small desk device you build yourself: a color touchscreen, a microphone and speaker, and (on the classic build) a 45-LED light ring, all on an ESP32-S3. It sits at the edge of your attention and works light first, screen second, sound last. Dark and silent until something actually needs you.</p>
    <p>Bring your own AI provider key. It runs on your keys, on your network, with nothing required to phone home.</p>
    <div class="home-hero__cta">
      <a class="button button--primary button--lg" href="/quick-start/what-you-need">Get started</a>
      <a class="button button--secondary button--lg" href="/flash">Flash from your browser</a>
    </div>
  </div>
</div>

:::info License - source-available, noncommercial
Nimbus is **source-available under noncommercial licenses**: code under the
[PolyForm Noncommercial License 1.0.0](https://polyformproject.org/licenses/noncommercial/1.0.0/),
documentation and hardware under
[CC BY-NC-SA 4.0](https://creativecommons.org/licenses/by-nc-sa/4.0/). Build
one, modify it, share your fork, just not commercially. Details in the
repository's `LICENSE`, `LICENSE-docs`, and `NOTICE`.
:::

## Two builds, one firmware

Pick the build that suits you. Both run the exact same firmware, both do
everything on this site, and both cost far less than a smart speaker. The
difference is how much you make by hand, and whether you want a real light ring.

<div class="variant-grid">
  <div class="variant-card">
    <div class="variant-card__media">
      <img src="/img/hardware/classic-glow.webp" alt="The classic Nimbus, a translucent printed case with its LED ring glowing" />
    </div>
    <div class="variant-card__body">
      <p class="variant-card__kicker">The classic build</p>
      <h3>Nimbus with a real light ring</h3>
      <p>Hand-built on an ESP32-S3 dev board and a small carrier PCB, in a 3D-printed case. The signature 45-LED ring wraps the device and glows across the room.</p>
      <ul class="variant-card__specs">
        <li>2.8" color touchscreen, resistive touch</li>
        <li>45-LED WS2812B light ring</li>
        <li>Microphone, speaker, microSD, optional battery</li>
        <li>Some soldering and a printed case</li>
        <li>About $60 to $75 in parts</li>
      </ul>
      <p><a href="/guides/hardware-touch-tft">Build the classic Nimbus &rarr;</a></p>
    </div>
  </div>
  <div class="variant-card">
    <div class="variant-card__media">
      <img src="/img/hardware/light-setup.webp" alt="Nimbus Light, the Freenove all-in-one board in a compact printed case" />
    </div>
    <div class="variant-card__body">
      <p class="variant-card__kicker">The all-in-one build</p>
      <h3>Nimbus Light (Freenove CYD)</h3>
      <p>One off-the-shelf module with the screen, touch, microSD, mic, and speaker already on board. Nothing to wire, nothing to solder. The status ring is drawn on the screen instead of a physical ring.</p>
      <ul class="variant-card__specs">
        <li>2.8" color touchscreen, capacitive touch</li>
        <li>On-screen status ring (no physical ring)</li>
        <li>Microphone, speaker, microSD, optional battery</li>
        <li>Buy one part, plug in USB-C, flash</li>
        <li>About $20 to $25 for the board</li>
      </ul>
      <p><a href="/guides/hardware-all-in-one-cyd">Build Nimbus Light &rarr;</a></p>
    </div>
  </div>
</div>

<div class="photo-strip">
  <figure>
    <img src="/img/hardware/light-onscreen-ring.webp" alt="Nimbus Light drawing the status ring on its screen" />
    <figcaption>Nimbus Light draws the ring on its screen.</figcaption>
  </figure>
  <figure>
    <img src="/img/hardware/light-board.webp" alt="The Freenove all-in-one board, everything on one module" />
    <figcaption>All-in-one: nothing to wire.</figcaption>
  </figure>
  <figure>
    <img src="/img/hardware/classic-desk.webp" alt="The classic Nimbus on a desk stand" />
    <figcaption>The classic build, ring around the case.</figcaption>
  </figure>
</div>

Every panel is a 2.8" ILI9341 (240x320) on an ESP32-S3 with 16 MB flash and
8 MB PSRAM, and the Freenove comes in larger sizes too. Full specs and wiring
are in the [hardware reference](guides/hardware.md), and the parts list is in
the [bill of materials](guides/hardware-bom.md).

## One device, two modes

The same hardware runs in one of two modes. Pick one at setup and switch any
time. Switching restarts the device.

### Notifier - a status light for AI coding sessions

Your computer reports coding-session state over an encrypted, paired Bluetooth
link, and the ring shows what every session is doing: running, waiting on you,
done, errored. Each state has its own color and motion, glanceable from across
the room without stealing your focus. On Nimbus Light the same arcs are drawn
on the screen.

**Start here: [Notifier quick start](quick-start/notifier-quick-start.md)**

### Orchestrator - a self-hosted AI assistant

A standalone assistant powered by a hosted LLM provider (Mistral, OpenAI,
Anthropic, or your own endpoint) with your own API key. Reach it over Telegram,
by voice (hold-to-talk on the device), or in the browser. It keeps long-term
memory on its SD card, runs tools mid-turn, spawns background research agents,
runs scheduled routines, and updates itself over the air with signed releases.

**Start here: [What you need](quick-start/what-you-need.md)**

<div class="cumulo-callout">
  <h2>Cumulo Nimbus - the official companion service</h2>
  <p>Nimbus is built to run entirely on your own keys and your own network. When you want more, <a href="https://app.cumulo-nimbus.ai">Cumulo Nimbus</a> is the official service that plugs straight into the firmware. It is optional and off by default.</p>
  <ul>
    <li><b>One key for every provider.</b> A <a href="/cloud/cumulo-key">Cumulo key</a> draws on a single prepaid balance that covers Mistral, OpenAI, and Anthropic. Top up once, use any model, no per-provider accounts or invoices. Paste it under <b>Assistant &rarr; Models</b> and pick a model.</li>
    <li><b>Reach your device from anywhere.</b> <a href="/cloud/cloud-access">Cloud access</a> opens one outbound secure tunnel so you can open the device's own web UI remotely, with no port forwarding. Pair it with a Cloud link code from the device screen.</li>
  </ul>
  <p>Both are opt-in, both check ownership before anything reaches the device, and neither replaces the local web UI. It is the same device, reachable two ways.</p>
</div>

## Where to go

- **Build one:** [What you need](quick-start/what-you-need.md), then the
  [bill of materials](guides/hardware-bom.md), the
  [build guides](guides/hardware.md),
  [flash](quick-start/flash.md), and [set up](quick-start/setup-wizard.md).
- **Use one:** [Modes & signals](guides/modes-and-signals.md) explains every
  light and sound. The [web UI reference](getting-started/webui-reference.md)
  walks every settings tab.
- **Understand it:** [Architecture](guides/architecture.md), then
  [turn anatomy](guides/turn-anatomy.md) (what the model actually sees) and
  [Orchestrator World](guides/orchestrator-world.md) (the memory system).
- **Fork it:** the [development guide](contributing/development.md) and
  [self-hosted OTA](contributing/self-hosted-ota.md) (shipping firmware to your
  own devices).

:::note
These pages are a published *view* of the firmware repo's `docs/` tree. The
canonical source is the Markdown in the repository. See the
[docs map](https://github.com/ristllin/Nimbus/blob/main/docs/README.md).
:::
