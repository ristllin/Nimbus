# What you need

Everything required to go from parts to a working Nimbus. Budget roughly
**$60–75 in parts** and an afternoon of assembly.

## The checklist

1. **The electronics** - an ESP32-S3-DevKitC-1 (N16R8), a display (2.9" e-ink
   + knob, or a 2.8" color touchscreen), the 45-LED ring, mic, amp, speaker,
   microSD module, and the 2S battery section. Full parts list with sourcing
   links and prices: **[Bill of materials](../hardware/bom.md)**.
2. **A USB-C cable** (data-capable) for the first firmware install.
3. **A computer** for flashing - any OS with a Chromium-based browser (for the
   browser flasher), or Python 3 + [PlatformIO](https://platformio.org/) for
   the command-line path.
4. **A 2.4 GHz Wi-Fi network** - the radio does not see 5 GHz-only networks.
5. **One AI provider API key** for Orchestrator mode - Mistral, OpenAI, or
   Anthropic. One verified key is required by the setup wizard; you can add
   others later. (Skip this if you only want the Notifier status light.)
6. **Optional, recommended:** a microSD card (formatted FAT32 - see the
   [build guide](../hardware/build-eink.md#known-traps)) for long-term memory
   and media, and a Telegram account to chat with the assistant.

## Choose your build

| | E-ink + knob *(default)* | Touch TFT |
|---|---|---|
| Display | 2.9" e-paper, always readable | 2.8" color touchscreen |
| Input | Rotary knob | Touch |
| Guide | [Build guide](../hardware/build-eink.md) | [Build guide](../hardware/build-tft.md) |

Both builds run the same firmware and support both operating modes
(Notifier status light, Orchestrator assistant).

Next step: **[Flash the firmware](flash.md)**.

---

*How it works → [Architecture](../architecture.md)*
