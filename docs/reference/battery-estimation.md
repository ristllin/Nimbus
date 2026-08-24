<!-- audience: user -->
# Battery settings and estimation

Nimbus reads a battery pack through a resistor divider on one ADC pin, not a fuel
gauge chip. That means the state-of-charge it shows is a **voltage estimate**, not
a coulomb count: good enough to plan around, approximate under load. These
settings tell it what pack you actually fitted so the estimate matches reality.
The defaults reproduce the packs the project ships with, so if you built a stock
board you can leave everything alone.

You set these on the device web page (Settings, Battery). None of it needs lab
gear: every value below can be read off the cell's datasheet, a multimeter, or a
USB charger's own display.

## What each value means

**Chemistry.** Which discharge curve to map voltage onto.

- **Li-ion / LiPo** (the default): a single cell runs about 4.2 V full to 3.0 V
  empty, with a sloped middle that makes voltage a decent fuel gauge.
- **LiFePO4** (lithium iron phosphate): a single cell runs about 3.65 V full to
  2.5 V empty, but sits near 3.2 to 3.3 V for most of its life. That flat plateau
  makes voltage a **coarse** gauge for this chemistry: expect the percent to hold
  steady for a long time and then move quickly near the ends. Picking the right
  chemistry matters most here, because a LiFePO4 pack read on the Li-ion curve
  would show nearly empty for almost its whole charge.

**Cells (1S / 2S).** How many cells are wired in series. The device divides the
measured pack voltage by this to get per-cell voltage before looking up the curve.
A stock hand-built board is 2S (two cells, about 8.4 V full); a Freenove all-in-one
is 1S. Set it to match your pack; a wrong count reads the pack at the wrong percent.

**Capacity (mAh).** The pack's rated charge, from the cell's datasheet or printed
on the cell. This does not change the percent; it sets the **runtime** estimate
(how long until empty) and the "usable capacity" readout, which is capacity scaled
by the learned health. A reclaimed vape cell might be 500 mAh; a LiitoKala 18650 is
about 3500 mAh (the default).

**Custom curve (advanced, optional).** If you know your pack's resting
voltage-to-percent points better than the built-in curve, enter them as
`mv:pct,mv:pct,...` per cell, highest voltage first (for example
`4200:100,3700:50,3200:0`). Leave it blank to use the chemistry's built-in curve.
A malformed entry is ignored, so a typo can never brick the reading.

**Divider resistors and the sleep thresholds** are covered in the
[config reference](config-and-nvs.md); adjust the divider only if you fitted
different resistors than the reference design.

## How to estimate the values without lab tooling

- **Chemistry and cells:** read them off the cell wrapper or its datasheet. "18650
  Li-ion 3.7 V" is Li-ion; "IFR/LFP 3.2 V" is LiFePO4. Count the cells in series
  for 1S vs 2S.
- **Capacity:** use the mAh printed on the cell or its datasheet. If a cell is
  unmarked, its capacity is unknown; pick the datasheet value for that size (a bare
  18650 is typically 2000 to 3500 mAh) and refine later.
- **Full-charge check:** charge the pack fully, let it rest a few minutes off the
  charger, then read the pack voltage on a multimeter. A healthy full Li-ion cell
  rests near 4.2 V (8.4 V for 2S); a LiFePO4 cell rests near 3.35 to 3.4 V. If the
  device shows less than 100 percent at a true full charge, use **Recalibrate to
  100 percent** (Battery, then Calibrate) so it anchors full to your reading.
- **Runtime, the honest way:** the device learns your real discharge rate as it
  runs, so the time-to-empty estimate gets steadier over the first few discharges.
  You do not need to measure current; just use it and let it learn.

## What the device does with them

The chosen curve (chemistry or your custom points) is what turns voltage into the
percent shown on screen, over the web page, and to the assistant. The capacity and
the learned health set the runtime estimate. Health is relative: the first full
discharge sets the baseline, and later discharges that traverse the same voltage
band faster read as reduced capacity.

The discharge-rate and health learning are tuned for Li-ion packs. On a LiFePO4
pack the **percent** follows the LiFePO4 curve correctly, but treat the runtime and
health numbers as rougher until you have run it through a few cycles.

## Where this is verified

The curve math, chemistry selection, and custom-curve parsing are host-tested in
`test/test_battery_model` (`pio test -e native -f test_battery_model`): the default
chemistry reproduces the shipped Li-ion curve, the LiFePO4 curve is monotonic and
hits its endpoints, a model set to LiFePO4 reports the LiFePO4 state of charge, and
malformed custom curves are rejected.
