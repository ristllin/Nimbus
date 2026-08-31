<!-- audience: user -->
# Modes & signals - what the settings mean and what the device is telling you

Nimbus has three settings that shape everything you see and hear. Two of them
you choose; the third is the device's job:

| Setting | Choices | What it controls |
|---|---|---|
| **Battery mode** | Dark · Balanced · Full | How much light the device shows, and how much power it spends doing it |
| **Sound** | level Off / Low / Medium / High, sound theme (Pulse), volume 0-100 | Sound clips on device events |
| **Operating mode** | Notifier · Orchestrator | What drives the device: your AI coding sessions, or the hosted agent |

They are independent. A silent device can still be at Full light; a Dark device
still speaks if its sound level is up. What the battery mode does **not** mean:
it is not the battery's charge state (that rides the battery percentage on the
header - `^75%` charging, `=75%` steady on external power, bare `75%` draining),
and `power:Full` in the header means *the Full preset is active*, never "plugged
in".

A quick example: on a desk, most people run **Full** - every session gets a
color arc, the screen updates promptly. Unplugged for the evening, switch to
**Balanced** - the ring quiets to a single soft cue and the screen batches its
updates. Overnight on battery, **Dark** - no lights at all unless a job errors
or needs you.

One exception on Balanced, in Orchestrator mode only: while the assistant is
running sub-agents, the ring temporarily splits into one arc per sub-agent -
the same per-session view Full shows, at Balanced's dimmer brightness - so a
fan-out is glanceable while it works. The arcs fade out as each sub-agent
finishes and the ring returns to its single soft cue.

> Vocabulary changed in July 2026: the old separate light setting is gone - the
> battery mode now decides how much light the device shows. "Ring level"
> survives only as one of the mode's customizable reference values (§1).

---

## 1. Battery modes are preset bundles you can customize

Each battery mode is a **row of reference defaults**. Pick a mode and you get
the whole row; change any single value and your override survives mode
switches until you reset it.

- **On the device:** Settings > Battery mode to pick a mode; Settings >
  Customize to change individual values; Settings > Reset to defaults to drop
  every override.
- **On the web page:** Settings → Battery mode; Settings → Customize this mode;
  the **Revert to Defaults** button clears all overrides at once.

The reference defaults:

| Value | Dark | Balanced | Full |
|---|---|---|---|
| Ring level (§2) | Dark | Calm | Full |
| Ring brightness | 10 | 30 | 60 |
| Animation smoothness (FPS) | 20 | 30 | 60 |
| Screen update pace (batching) | 60 s | 30 s | 15 s |
| On-screen status refresh | 300 s | 120 s | 60 s |
| Low-battery Telegram alert | on | on | off - not needed on external power |
| Needs-you hold | 1 min | 2 min | 5 min |

The needs-you hold scales with the mode like everything else: a desk display
can insist for five minutes, a dim room clears sooner - and every value here
is tunable per mode in Customize.

<details>
<summary>For developers - machine keys and source</summary>

Source of truth: `lib/core/src/profile.cpp` (`kPresets`). Machine keys stay
`battery_saver` / `balanced` / `desk` (NVS, `/api`, the AI config schema) -
only the user-facing labels changed. "Revert to Defaults" is
`revert_overrides` on `POST /api/config`.

</details>

## 2. Ring level - what the ring shows in each mode

Each battery mode binds one **ring level** as its reference default (Dark →
Dark, Balanced → Calm, Full → Full). The "Ring level" row under Settings >
Customize is the only place you change it independently.

| | Dark | Calm | Full |
|---|---|---|---|
| Session arcs and animations | - | - | ✔ every session a color arc |
| Needs-you cue | ✔ one attention LED | ✔ one attention LED | shown as arcs |
| Orchestrator activity glow (working breathe) | - | ✔ | via arcs |
| Idle | dark | dark | dark - a lit ring always means something |
| How long ambient status lingers after the link goes quiet | 5 s | 30 s | 5 min |
| How long a needs-you cue holds | 1 min | 2 min | 5 min |

**Single click = wake the ring**, in every ring level including Full: a ~4 s
full-brightness reveal of live status, then back to normal. This is how you
glance state on a dark idle ring.

### The screen backlight

On the color touch panel the **backlight is the largest continuous draw** -
bigger than the ring - so the battery mode reaches it too:

| Battery mode | Backlight |
|---|---|
| Dark | 35% |
| Balanced | 65% |
| Full | 100% |

When the screen rests (screensaver), it dims to a faint glow rather than going
fully dark - a black color panel is indistinguishable from a broken one, and at
this level the backlight draws a few percent of its lit value, so nearly all of
the saving is kept. Any touch or activity brings it straight back to the
current mode's level.

The screen also **rests after a short idle**: the default idle delay is
**5 minutes**, because an hour of backlight at an empty desk is the most wasteful
thing the device can do.
Setting the delay yourself (Settings > Screensaver on the device, Settings →
Mode & identity on the web) always wins over either default.

### The screen on a board with no ring

Some boards, like the [all-in-one](hardware/all-in-one-cyd.md) (Freenove CYD),
have a single RGB LED instead of the 45-pixel ring. On those, the **Notifier**
status ring is drawn on the screen: each active session shows as an arc in the
same colors and animations the physical ring would use, so the status language is
identical whichever board you have. Unlike a physical ring, the on-screen ring
always renders at full brightness and detail regardless of battery mode - there
is no LED power to save by dimming it, so dimming it would only make it harder to
read. The battery mode instead dims the **backlight** (see
[the screen backlight](#the-screen-backlight) above), which is
where this board's actual power draw is.

The ring shows exactly what the connected notifier broker sends. A broker with
stale sessions left over from earlier test runs can legitimately show more
"active" arcs than you expect for a moment; the device clears a session that
stops sending frames on its own, within a hold window scaled to the battery mode
(minutes in Full, seconds in Dark) - not something you need to reconnect or
trigger. If it never clears, check the broker's own session bookkeeping rather
than the device.

### Display flip

**Settings > Display > Display flip** on the device (or the Display flip toggle
under Settings → Display on the web) rotates the screen 180 degrees for an
upside-down mount. It applies right away, and touch follows the flip so taps still
land where you touch.

### Power off

**Settings > Power off** on the device (or **Power off** under Settings → Power
on the web) saves everything, turns the screen off, and puts the device into deep
sleep. It confirms first, so a stray tap can never turn the device off. Settings
are kept, so it comes back exactly as you left it.

How it wakes depends on the model. On a touch model whose touch controller can
wake it, a tap on the screen turns it back on, and the copy says so ("Tap the
screen to wake it"). On a model that cannot wake from a touch, it comes back when
you reconnect power, and the copy says that instead. The device knows which
applies and tells the truth on both the screen and the web page.

### Low battery

Two settings under **Settings → Battery mode**, both about a low battery
rather than about a mode:

| Setting | Default | What it does |
|---|---|---|
| Low-battery light | off | A dim red pulse on the ring, about three seconds each minute, while the battery is low |
| Save power when low | on | Switches to the Dark battery mode while the battery is low, then returns to your chosen mode once it recovers |

The light is off by default because a ring lit all night spends the power it is
warning about - and it is the brightest thing the device does. When on, it is
deliberately recessive: dimmed in Full, pulsed rather than continuous in every
mode, so it reads as information rather than an alarm.

Neither setting weakens protection. The screen notice, the Telegram message,
and the low-battery deep sleep all happen either way; a **job** error still
breathes red at full brightness in every mode, including Dark.

## 3. Sound - independent of everything above

Set under Settings > Sound (device) or Settings → Sound (web): a level per
**operating mode**, a sound theme (**Pulse**), and a master
volume. Notifier defaults to **Off**, Orchestrator to **Medium** - Notifier
receives a flood of session events, so its clip table is sparser at every
level. Sound pauses automatically while the microphone is recording, and it
never changes what the ring or screen do.

The full event → clip map is [docs/sfx-map.md](./sfx-map.md).

## 4. Notifier vs Orchestrator

| | Notifier | Orchestrator |
|---|---|---|
| Purpose | Status light for your AI coding sessions | The hosted agent (Telegram + voice) |
| Ring is driven by | Session frames from the host broker, over Bluetooth only | The agent's own sessions and turns |
| Bluetooth | on (advertising, bonded) | off - this is why the broker only finds a Notifier board |
| Telegram and providers | off | on |
| Sound default | Off | Medium |
| Status language (§5) | identical | identical |

Switch modes from Settings > Mode on the device (selecting it restarts the
device), Settings → Mode & identity on the web page, or the `MODE` console
command. Everything in §1–§3 applies to both modes.

## 5. The status language - colors, motion, themes

One rule to remember: **movement means it needs you or it is working; still
means nothing to do.**

| Status | Color (theme role) | Motion | Read it as |
|---|---|---|---|
| Running | role 0 - the theme's primary | comet (sliding) | working |
| Waiting for input | role 1 - cool analogous | breathe | it needs YOU |
| Awaiting approval | role 3 - amber-leaning | breathe | decision gate |
| Done | role 2 - green-leaning | fade → settles to dim static (38%) | finished, settling |
| Error | the theme's alert hue | breathe | broken, look at it |
| Idle | neutral white, theme-less | static, dim | nothing happening |

**Themes** (10): `teal ocean ember forest openai anthropic mistral rainbow
gemini perplexity` - each is a four-stop color family plus a per-theme alert
hue. Changing the theme recolors every role in both modes, live. The web
legend (Settings → Display) is generated from the same tables the
firmware uses, so it cannot drift from what the ring actually does.

Rules the firmware enforces (and tests assert):

- **Red belongs to errors only.** No theme may place a role in the red band.
- Every theme's four roles are hue-distinct.
- Idle never moves and never wears the theme - a dim, intentionally neutral white.
- One session fills the **full ring**; multiple sessions are arcs separated by
  dim static white divider LEDs - count dividers, count agents.

Full specification: [Notifier status language](./notifier-status-language.md).

## 6. Menu ring echo

While the settings menu is open, the ring becomes a **fill bar** that echoes the
menu. Browsing shows position and count; adjusting a value shows the value
itself. Dim by design; it holds about 2 s after the last change, then live status
resumes.

## 7. Wi-Fi: saved networks and failover (Orchestrator only)

Orchestrator remembers up to **five** Wi-Fi networks (home, office, a phone
hotspot, and so on), not just one, so moving the device between them no longer
erases the credentials that got it online last time. If the current network goes
away, the device behaves like a phone: it scans, picks the strongest saved network
in range, and joins it on its own, with no setup step and no restart. The setup
hotspot only comes back once **every** saved network has been tried and none is
reachable. While it is working through the list the screen says so honestly, for
example `Joining Office 2/3...`.

Order is a light **priority** hint: the device joins by signal strength, and the
saved order only breaks ties between equally strong networks. A network you
actually connect to moves to the top on its own.

**Where you manage it.** The saved-network list (add, forget, reorder, see which is
connected) lives on the web page under **Settings → Connectivity** - that is its
canonical home; a password is never shown back. The device screen's **Settings →
Wi-Fi** is the other surface: it shows the live status and a scan-and-join picker,
but not the full list editor. Networks save from either surface into the one shared
list.

---

## For developers - where the logic lives

If this page ever disagrees with the code, the code's single sources win:

| Thing | File |
|---|---|
| Battery-mode presets | `lib/core/src/profile.cpp` |
| Ring-level render rules | `lib/core/src/ring_plan.cpp` |
| Status → role/motion | `lib/core/src/status_style.cpp` |
| Palettes + alert hues | `lib/core/src/theme.cpp` |
| Arc animation envelopes + dividers | `lib/core/src/ring_animator.cpp` |
| Ambient/needs-you holds + tombstones | `lib/core/src/notifier_map.cpp`, `attention.cpp` |
| Backlight per mode + rest glow | `lib/core/include/nimbus/duty.h` |
| Sound event ranks | `lib/core/nimbus/sfx_map.*` + `docs/sfx-map.md` |
| Header (`ring:/sound:/power:`) | `lib/core/src/tft_screens.cpp` `drawHeader` |
| Saved-network list + selection/failover | `lib/core/src/wifi_known_networks.cpp`, `wifi_policy.cpp` |
| Failover wiring to the radio (the seam) | `src/net/wifi_link.cpp` |
| Setup-AP recovery interplay | `lib/core/src/setup_ap.cpp` (`decideSetupAp`) |

Full detail: [Wi-Fi resilience](./wifi-resilience.md).
