# What you need

Everything to go from parts to a working Nimbus. There are two builds: the
hand-built classic with a real LED ring (about **$60-75** and an afternoon), or
the **all-in-one** Freenove board (about **$20-25** and minutes, nothing to
wire).

## Choose your build

| | Classic Nimbus board | All-in-one (Freenove CYD) |
|---|---|---|
| Display | 2.8" color touchscreen | 2.8" / 3.5" / 4.0" color touchscreen |
| Touch | Resistive | Capacitive |
| Status ring | 45-LED physical ring | Drawn on the screen |
| Build | Hand-wired from parts | One off-the-shelf module, no wiring |
| Modes | Notifier + Orchestrator | Notifier + Orchestrator |
| Cost | About $60-75 | About $20-25 |
| Guide | [Build guide](../hardware/build-tft.md) | [All-in-one guide](../hardware/all-in-one-cyd.md) |

Both builds run the same firmware and do everything on this site. The classic
board has a real light ring you can see across the room; the all-in-one draws the
same status arcs on its screen and is the fastest path from box to running: buy
one board, plug in USB-C, flash.

## The checklist

1. **The board.** For the classic build, the parts list (an ESP32-S3-DevKitC-1
   N16R8, a 2.8" touchscreen, the 45-LED ring, mic, amp, speaker, microSD, and
   the 2S battery section) with sourcing and prices is in the
   **[bill of materials](../hardware/bom.md)**. For the all-in-one, it is one
   part: the **[Freenove CYD](../hardware/all-in-one-cyd.md)**.
2. **A data-capable USB-C cable** for the first firmware install. (Many cables
   are charge-only and never show a serial port.)
3. **A computer** to flash from: any OS with a Chromium-based browser for the
   [browser flasher](flash.md), or Python 3 and
   [PlatformIO](https://platformio.org/) for the command line.
4. **A 2.4 GHz Wi-Fi network.** The radio does not see 5 GHz-only networks.
5. **One AI provider key** (for Orchestrator mode). **Start with
   [Mistral](https://console.mistral.ai/):** its free tier is enough to set up and
   try Nimbus, and it is the voice default. OpenAI and Anthropic work too and can
   be added later (a few extras, like image generation and spoken replies on the
   device speaker, are OpenAI-only). The setup wizard needs one verified key. Skip
   this if you only want the Notifier status light. Prefer one prepaid balance
   across every provider? Use a [Cumulo key](../cloud/cumulo-key.md).
6. **Optional, recommended:** a microSD card (formatted FAT32) for long-term
   memory and media, and a Telegram account to chat with the assistant.

Next step: **[Flash the firmware](flash.md)**.

---

*How it works -> [Architecture](../architecture.md)*
