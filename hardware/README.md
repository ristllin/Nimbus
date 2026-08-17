# Hardware

Everything physical about a Nimbus build - what to source, how to assemble it, and
the board manufacturing files.

> **New here?** Start with the documentation, which walks the whole build:
> - [Bill of materials](../docs/hardware/bom.md) - what to buy, with sourcing links
> - [Build guide - E-paper + knob](../docs/hardware/build-eink.md)
> - [Build guide - Touch TFT](../docs/hardware/build-tft.md)
> - [Hardware reference](../docs/hardware.md) - pinout, wiring, power
>
> Those pages are also on the [documentation site](https://docs.cumulo-nimbus.ai/).

> **Safety and liability:** you build at your own risk. This is a DIY
> prototype involving lithium-ion batteries, not a certified product; no
> warranty, no liability. Read [DISCLAIMER.md](../DISCLAIMER.md) first.

## What's in this folder

| Path | Contents |
|---|---|
| [`fab/`](fab/) | PCB manufacturing outputs (Gerbers, drill, ODB++) for the Solide S3 carrier board, [how to order it (e.g. JLCPCB)](fab/README.md#ordering-the-pcb), and the [3D-printable enclosure STLs](fab/README.md#3d-printing-the-enclosure). Not required if you build from off-the-shelf modules. |

## Assembly notes, BOM extras, and CAD

This folder is the home for build material that isn't firmware: assembly photos and
step notes, a spreadsheet BOM, enclosure/CAD files, and any wiring harness details.
It starts light on purpose - add to it as the physical design settles. The canonical,
published build documentation stays under [`docs/hardware/`](../docs/hardware/) so it
ships to the docs site; this folder holds the source artifacts those pages describe.
