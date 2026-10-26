# What you need

Everything to go from parts to a working Nimbus. There are two builds: the
hand-built classic with a real LED ring (about **$65-77** and an afternoon), or
the **all-in-one** Freenove board (about **$20-25** and minutes, nothing to
wire).

:::tip Get your provider key before you start
Setup puts your phone or laptop on the device's own Wi-Fi, which has no internet,
so you cannot fetch a key partway through. Create it first and keep it pasteable on
the device you will set up from. See [Get your provider key](provider-keys.md).
:::

## Choose your build

| | Classic Nimbus board | All-in-one (Freenove CYD) |
|---|---|---|
| Display | 2.8" color touchscreen | 2.8" color touchscreen (3.5" and 4.0" builds exist but have not been run on real hardware yet) |
| Touch | Resistive | Capacitive |
| Status ring | 45-LED physical ring | Drawn on the screen |
| Build | Hand-wired from parts | One off-the-shelf module, no wiring |
| Modes | Notifier + Orchestrator | Notifier + Orchestrator |
| Cost | About $65-77 | About $20-25 |
| Guide | [Build guide](../hardware/build-tft.md) | [All-in-one guide](../hardware/all-in-one-cyd.md) |

Both builds run the same firmware and support every feature documented on this
site. The classic board has a physical light ring you can read across the room.
The all-in-one draws the same status arcs on its screen and is the fastest path
from box to running: buy one board, plug in USB-C, flash.

## The checklist

1. **The board.** For the all-in-one, it is one part: the
   **[Freenove CYD](../hardware/all-in-one-cyd.md)**. For the classic build, the
   **[bill of materials](../hardware/bom.md)** lists every part with sourcing and
   prices: an ESP32-S3-DevKitC-1 N16R8, a 2.8" touchscreen, the 45-LED ring,
   microphone, amplifier, speaker, microSD, and the 2S battery section.
2. **A data-capable USB-C cable** for the first firmware install. Charge-only
   cables never show a serial port.
3. **A computer** to flash from: any OS with a Chromium-based browser for the
   [browser flasher](flash.md), or Python 3, git, and
   [PlatformIO](https://platformio.org/) for the command line
   ([Set up your computer](computer-setup.md) installs them). The command-line
   path downloads about 0.6 GB of build tools on its first run and needs about
   6 GB of free disk space.
4. **A 2.4 GHz Wi-Fi network** for Orchestrator mode. The radio does not see
   5 GHz-only networks. Skip this if you only want the Notifier status light:
   Notifier mode uses Bluetooth instead and needs a computer with Bluetooth and
   Python 3 (see [Notifier quick start](notifier-quick-start.md)).
5. **One AI provider key** for Orchestrator mode. The setup wizard needs one
   verified key; skip this if you only want the Notifier status light. The
   recommended key is a [Cumulo key](../cloud/cumulo-key.md): one prepaid balance
   that covers every provider, voice and long-term memory. To try Nimbus for free,
   use [Mistral](https://console.mistral.ai/): its free tier is enough to set up,
   and it covers voice and memory too. OpenAI and Anthropic also work and can be
   added later; image generation needs an OpenAI key. Step-by-step instructions for
   each provider are in **[Get your provider key](provider-keys.md)**.
6. **Optional, recommended:** a microSD card (formatted FAT32) for long-term
   memory and media, and a Telegram account to chat with the assistant.

Next step: **[Flash the firmware](flash.md)**.

---

*To see how the pieces fit together, read [Architecture](../architecture.md).*
