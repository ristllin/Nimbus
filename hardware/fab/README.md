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
| [`case_eink.stl`](case_eink.stl) | 3D-printable enclosure for the e-paper + knob build |
| [`case_touch_screen.stl`](case_touch_screen.stl) | 3D-printable enclosure for the touch TFT build |

Both PCB formats describe the same board; supply whichever your fab prefers.

## Ordering the PCB

This is a **2-layer** board. Any PCB house that accepts Gerber or ODB++ can make it.
These files were last ordered from **[JLCPCB](https://jlcpcb.com/)**, which was cheap
and straightforward for a board this size; the steps below are for that service, but
any fab works the same way.

1. **Zip the Gerbers.** Compress the whole [`gerbers/`](gerbers/) folder into one
   `.zip`. (Or skip this and upload [`odb/pcb3_v7.zip`](odb/pcb3_v7.zip) as-is -
   JLCPCB accepts either.)
2. Go to **[jlcpcb.com](https://jlcpcb.com/)**, click **Add gerber file**, and upload
   the zip. The board renders in the viewer so you can confirm it parsed correctly.
3. The auto-detected settings are right for this board. The ones worth checking:
   - **Layers:** 2
   - **Dimensions:** read from the profile layer automatically
   - **PCB thickness:** 1.6 mm
   - **Surface finish:** HASL (lead-free) is cheapest; ENIG if you prefer
   - **PCB Qty:** 5 is the usual minimum
   Leave the rest (colour, copper weight) at default unless you have a reason to change it.
4. **Add to cart** and check out. Turnaround is usually a few days plus shipping.

> This produces a **bare board** only. You still populate it with the parts in the
> [bill of materials](../../docs/hardware/bom.md). JLCPCB also offers assembly (PCBA),
> which needs a BOM and pick-and-place file that are not generated here.

## 3D-printing the enclosure

[`case_eink.stl`](case_eink.stl) and [`case_touch_screen.stl`](case_touch_screen.stl)
are the two enclosure options - print the one that matches your display (e-paper + knob,
or touch TFT). PLA or PETG both work; the main body prints without supports on most
machines.
