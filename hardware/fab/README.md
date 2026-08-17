# `fab/` - PCB manufacturing outputs

Fabrication files for the **Solide S3 carrier PCB** - the artifacts a board house needs
to manufacture the board.

**You do not need anything in here to build, flash, or use the Nimbus firmware.** For
wiring and pinouts see the [hardware reference](../../docs/hardware.md) and the two
configuration pages ([e-paper + knob](../../docs/hardware/eink-knob.md),
[touch TFT](../../docs/hardware/touch-tft.md)).

| Path | Contents |
|---|---|
| [`gerbers/`](gerbers/) | Gerber layers (copper, silkscreen, soldermask, solderpaste, profile), the drill file, and the Gerber job file |
| [`odb/pcb3_v7.zip`](odb/pcb3_v7.zip) | ODB++ export of the same board (zipped - most fabs accept either format) |

Both formats describe the same PCB; supply whichever your fab prefers.
