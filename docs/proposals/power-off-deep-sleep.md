# Software power-off (deep sleep, wake on touch)

Status: option A implemented (CUM-224). This note records the options that were
weighed and the hardware reality that shaped the final behavior.

## The ask

A "Power off" control, on both the device screen and the web UI, that puts the
device into a genuine low-power state and can be brought back without a wall
switch. No physical power button is exposed on the case.

## Options weighed

- **Option A (chosen): screen-only software power-off -> deep sleep, wake on
  touch where the hardware allows.** A "Power off" row in the device Settings
  menu and a "Power off" button in the web UI both run a clean shutdown (persist
  config, flush the memory journal), show a readable notice, turn the ring and
  backlight off, and enter ESP32-S3 deep sleep. Wake is per board (see below).
- **Option B: timer / scheduled wake.** Deep sleep with a periodic self-wake, or
  a "sleep until 8am" schedule. Deferred as future work: it is a different
  feature (a schedule, not an off switch) and the low-battery path already owns
  the periodic-wake machinery.
- **Option C: light sleep.** Keeps RAM and peripherals powered so touch can be
  polled to wake on any board, but draws far more than deep sleep and so is a
  poor fit for "off". Noted as a possible future Solide-only wake path if
  wake-on-touch there ever becomes a hard requirement.

## Hardware reality: wake-on-touch is per variant

The two supported boards wire their touch controller's interrupt line
differently, and deep-sleep wake (`ext0`) needs that line on an RTC-capable
GPIO (0-21 on the ESP32-S3):

| Board | Touch controller | INT line | Deep-sleep wake on touch |
|---|---|---|---|
| Freenove CYD 2.8" | FT6336U (capacitive) | INT on GPIO17 (RTC-capable, idles high, pulses low) | **Yes** - `ext0` wake on level 0 |
| Solide S3 | XPT2046 (resistive) | T_IRQ left unconnected (`-1`) | **No** - not routed to any GPIO |

So the device wakes on a screen tap on the Freenove CYD. On the Solide S3 the
pen-IRQ is not wired, so a touch cannot wake it; it returns on a power-cycle.
This is not a firmware choice: the existing low-battery sleep path already relies
on the Solide panel having no wake gesture.

The copy is honest about this on both surfaces. Where a tap can wake the device
it says "Tap the screen to wake it"; where it cannot it says "Reconnect power to
turn it back on". The device derives which applies from the board's touch INT
pin, so the same firmware tells the truth on either board.

## Future work

- Route the XPT2046 T_IRQ to a spare RTC GPIO on a Solide hardware respin to give
  it wake-on-touch too.
- Timer / scheduled wake (option B) as a separate "sleep schedule" setting.
