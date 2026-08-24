# What you need

Everything required to go from parts to a working Nimbus. Budget roughly
**$60–75 in parts** and an afternoon of assembly.

## The checklist

1. **The electronics** - an ESP32-S3-DevKitC-1 (N16R8), a 2.8" color
   touchscreen, the 45-LED ring, mic, amp, speaker, microSD module, and the 2S
   battery section. Full parts list with sourcing links and prices:
   **[Bill of materials](../hardware/bom.md)**. *Prefer the least effort?* The
   [all-in-one board](../hardware/all-in-one-cyd.md) (Freenove CYD) replaces this
   whole list with one module: about $20-25, nothing to wire.
2. **A USB-C cable** (data-capable) for the first firmware install.
3. **A computer** for flashing - any OS with a Chromium-based browser (for the
   browser flasher), or Python 3 + [PlatformIO](https://platformio.org/) for
   the command-line path.
4. **A 2.4 GHz Wi-Fi network** - the radio does not see 5 GHz-only networks.
5. **One AI provider API key** for Orchestrator mode. **Start with
   [Mistral](https://console.mistral.ai/)** - it has a free tier that is enough to
   set Nimbus up and try it, and it is the default for voice. OpenAI and Anthropic
   work too and can be added later (a couple of extras, like image generation and
   spoken replies on the device speaker, are OpenAI-only). One verified key is
   required by the setup wizard. (Skip this if you only want the Notifier status
   light.)
6. **Optional, recommended:** a microSD card (formatted FAT32 - see the
   [build guide](../hardware/build-tft.md)) for long-term memory and media, and
   a Telegram account to chat with the assistant.

## Choose your build

| | Nimbus board | All-in-one *(lowest effort)* |
|---|---|---|
| Display | 2.8" color touchscreen | 2.8" / 3.5" / 4.0" color touchscreen |
| Ring | 45-LED status ring | None |
| Build | Hand-wired from parts | One off-the-shelf module, no wiring |
| Modes | Notifier + Orchestrator | Orchestrator |
| Guide | [Build guide](../hardware/build-tft.md) | [All-in-one guide](../hardware/all-in-one-cyd.md) |

Both builds run the same firmware. The Nimbus board adds the 45-LED ring and can
run as a Notifier status light or the Orchestrator assistant; the all-in-one
(Freenove CYD) is the fastest path to the Orchestrator: buy one board, plug in
USB-C, and flash - there is nothing to wire or solder.

Next step: **[Flash the firmware](flash.md)**.

---

*How it works → [Architecture](../architecture.md)*
