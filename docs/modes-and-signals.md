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
it is not the battery's charge state. Charge shows as the battery icon in the
screen header (filled in proportion to the percentage), and the percentage with
its charging or discharging trend is on the device's **Settings > Battery**
screen. **Full** means the Full preset is active, never "plugged in".

A quick example: on a desk, most people run **Full** - every session gets a
color arc, the screen updates promptly. Unplugged for the evening, switch to
**Balanced** - the ring quiets to a single soft cue and the screen batches its
updates. Overnight on battery, **Dark** - no lights unless a job errors or you
hold to talk (the voice cue shows from hold-to-talk until the reply finishes).

One exception on Balanced, in Orchestrator mode only: while the assistant is
running sub-agents, the ring temporarily splits into one arc per sub-agent -
the same per-session view Full shows, at Balanced's dimmer brightness - so a
fan-out is glanceable while it works. The arcs fade out as each sub-agent
finishes and the ring returns to its single soft cue.

There is no separate light setting: the battery mode decides how much light the
device shows, and "Ring level" is one of the mode's customizable values (§1).

---

## 1. Battery modes are preset bundles you can customize

Each battery mode is a **row of reference defaults**. Pick a mode and you get
the whole row; change any single value and your override survives mode
switches until you reset it.

- **On the device:** Settings > Battery mode to pick a mode; Settings >
  Customize to change individual values; Settings > Reset > Reset settings to
  drop every override (it keeps Wi-Fi, keys, pairing, and memories). The same
  Reset screen also offers Factory reset: the full erase - Wi-Fi and passwords,
  provider keys, paired computers, settings, memories, routines, and logs,
  including the SD card - after which the device restarts into first-time setup.
  The device's internal storage is erased, not just its files deleted; files on
  the SD card are deleted but not overwritten, so before giving the device away,
  remove the card or erase it on a computer with an overwriting format (see the
  security doc).
- **On the web page:** Settings → Battery to pick a mode; Settings → Customize
  battery mode to change individual values; **Revert to Defaults** clears all
  overrides at once.

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

The device sometimes runs a lower mode than the one you picked: while the
battery is low and **Save power when low** is on, it switches to Dark, then
returns once the battery recovers. While that is happening, the web Battery
section shows one extra line, "Effective battery mode: Dark (adjusted
automatically for power)", using the same Dark / Balanced / Full vocabulary as
the mode picker. No line means the mode you picked is the one running.

<details>
<summary>For developers - machine keys and source</summary>

Source of truth: `lib/core/src/profile.cpp` (`kPresets`). Machine keys stay
`battery_saver` / `balanced` / `desk` (NVS, `/api`, the AI config schema) -
only the user-facing labels changed. "Revert to Defaults" is
`revert_overrides` on `POST /api/config`.

Selecting a mode on the web page also seeds two cross-cutting defaults
(screen-rest minutes and sound level) into any key the owner has not set
explicitly, via a one-time
migration that marks itself done in NVS. On a device whose NVS is full the marker
cannot persist, so the migration is deferred rather than re-run on every mode
switch: it fails loud once (a log line and a "Settings storage" health hint that
NVS is full), stops retrying until the next restart, and applies cleanly once a
restart finds NVS headroom. Freeing NVS space, then restarting, lets the profile
defaults land.

</details>

## 2. Ring level - what the ring shows in each mode

Each battery mode binds one **ring level** as its reference default (Dark →
Dark, Balanced → Calm, Full → Full). The "Ring level" row under Settings >
Customize is the only place you change it independently.

| | Dark | Calm | Full |
|---|---|---|---|
| Session arcs and animations | - | - | ✔ every session a color arc |
| Needs-you cue | errors only, a dim whole-ring breathe | ✔ a dim whole-ring breathe | shown as arcs |
| Working breathe (a session or turn running) | - | ✔ | via arcs |
| Idle | dark | dark | dark - a lit ring always means something |
| How long ambient status lingers after the link goes quiet | 5 s | 30 s | 5 min |
| How long a needs-you cue holds | 1 min | 2 min | 5 min |

**Single click = wake the ring**, in every ring level including Full: a ~4 s
full-brightness reveal of live status, then back to normal. This is how you
glance state on a dark idle ring.

### The screen backlight

On the color touch display the **backlight is the largest continuous draw**,
larger than the ring, so the battery mode sets it too:

| Battery mode | Backlight |
|---|---|
| Dark | 35% |
| Balanced | 65% |
| Full | 100% |

When the screen rests (screensaver), it first dims to a faint glow rather than
going black, so a resting display is never mistaken for a dead one. At this glow
the backlight draws a few percent of its lit value, so nearly all of the saving
is kept. Any touch or activity brings it straight back to the current mode's
level.

On battery, if the device stays idle longer, the screen turns the **backlight
fully off**: 30 seconds after resting in Dark, 2 minutes in Balanced, 5 minutes
in Full. Touch stays awake, so a single tap lights the screen again at once. That
first tap only wakes the screen; it never presses whatever was under your finger.
On external power the screen holds at the faint glow instead, since there is no
battery to save.

The screen also **rests after a short idle**: the default idle delay is
**5 minutes**, because an hour of backlight at an empty desk is the most wasteful
thing the device can do. Choosing a battery mode on the web page sets the delay to
that mode's default unless you have set your own: 2 minutes in Dark, 5 in
Balanced, and 0 (always on) in Full.
Setting the delay yourself (Settings > Screensaver on the device, Settings →
Display on the web) always wins over either default. Set it to 0 to keep the
screen always on. The web control is also the way to change the delay when the
touch panel cannot be reached from the device menu.

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

The ring shows exactly what the connected Notifier broker sends. If the broker
still lists a session that has ended, the ring can show one more active arc than
you expect for a moment. The device clears a session that stops sending frames on
its own, within a hold window scaled to the battery mode (minutes in Full,
seconds in Dark); no reconnect is needed. If an arc never clears, check the
broker's session list rather than the device.

### Display flip

**Settings > Display > Display flip** on the device (or the Display flip toggle
under Settings → Display on the web) rotates the screen 180 degrees for an
upside-down mount. It applies right away, and touch follows the flip so taps still
land where you touch.

### Restart

**Settings > Restart** on the device (or **Restart** on the web, both in the Home
quick actions and under Settings → Power) restarts the device and brings it back
on its own in about a minute. It confirms first, so a stray tap can never restart
it. Nothing is erased, so it comes back exactly as you left it. Use it to apply a
setting that says it takes effect after a restart (for example, **Monitor the
battery**), which offers a **Restart now** action right beside the setting.

### Power off

**Settings > Power off** on the device (or **Power off** on the web, in the Home
quick actions and under Settings → Power) saves everything, turns the screen off,
and puts the device into deep sleep. It confirms first, so a stray tap can never
turn the device off. Settings are kept, so it comes back exactly as you left it.

How it wakes depends on the model. On a touch model that can wake from touch, a
tap on the screen turns it back on, and the confirmation says so ("Tap the screen
to wake it"). On a model that cannot, it comes back when you reconnect power, and
the confirmation says that instead. The screen and the web page both show the
message that applies to your model.

### Low battery

Two settings under **Settings → Battery** on the web page, both about a low
battery rather than about a mode:

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

Everything in this section needs a battery the device can read. With **Monitor
the battery** on but no valid reading (no pack fitted, or a sense line the board
cannot read), the low-battery light, the power saving switch, the Telegram
low-battery message, and the low-battery deep sleep are all inactive, and the
Battery section says so inline. The two toggles still save and apply once a pack
is fitted and monitored.

Health shows which case you are in:

| Monitoring | Reading | Health row |
|---|---|---|
| off | - | "no gauge (desk-powered)" |
| on | valid | battery percentage |
| on | invalid, and a pack worked before (a valid reading since the last restart, or saved battery history) | "battery sense not detected" (degraded) |
| on | invalid, and no pack ever read | "no gauge (desk-powered)" |

## 3. Sound - independent of everything above

Set under Settings > Sound (device) or Settings → Sound (web): a level per
**operating mode**, a sound theme (**Pulse**), and a master
volume. Notifier defaults to **Off**, Orchestrator to **Medium** - Notifier
receives a flood of session events, so its clip table is sparser at every
level. Sound pauses automatically while the microphone is recording, and it
never changes what the ring or screen do.

The full event → clip map is [docs/sfx-map.md](./sfx-map.md).

### Action feedback - you always know a button worked

Every device-menu button that *does* something - forget paired devices, rescan
for the SD card, forget a network, reset to defaults, re-pair to the cloud,
restart - answers the press on the spot, across three channels so the answer
reaches you even if one is unavailable:

- a short **sound**: a settled tone on success, a distinct tone on failure (it
  plays whenever the sound level is above Off),
- a brief **ring** swell: green for success, red for failure, a soft accent for
  "started" when the real result lands later,
- an on-screen **line** naming the outcome, when the display is alive.

The sound and the ring do not depend on the screen, so the confirmation still
reaches you on a unit whose display has failed. A failure always names the next
step ("No SD card found. Reseat it and rescan."). Actions whose result only
arrives later - joining a network, requesting a cloud link code - acknowledge the
press at once and let the later Wi-Fi or pairing cue carry the real result. The
ring swell follows the same battery-mode and lights-off rules as every other
cue, so in Dark mode or with the ring silenced the sound and the screen carry it.

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

### The directive (Orchestrator)

The **directive** is your standing guidance for how the assistant behaves for you
- tone, how much it says, when to volunteer suggestions. It ships with an
owner-authored default, so the assistant has a sensible personality out of the box;
you can edit it or restore the default at any time. Only you can change it, and it
shapes style without ever overriding the device's safety, moderation, or access
rules.

| Setting | Canonical home | Other surface |
|---|---|---|
| **Directive** | Web app: setup wizard (Directive step) and the Directive box under Memory | Device menu: not editable there (free text; web-only) |
| **Music tracks** | Web app: an `.mp3` uploaded under Memory → Files (or dropped into Chat) goes to the card's music folder; there is no list, play, or remove control | Device menu: not managed there; play from Telegram `/play` or the assistant. |
| **Safety activity and allowlist** | Web app: Assistant, Safety, Activity (review what the scanner blocked or suspected; approve scoped, dismiss, or report) and Allowed items (revoke) | Device menu: not shown there; the `SAFETY?` serial command prints a one-line summary. |
| **Voice provider** (dictation and spoken replies) | Web app: Assistant, Models, Voice (Dictation and Spoken replies pickers). Each offers Mistral, OpenAI, and Cumulo Nimbus; Cumulo Nimbus routes voice through the metered router and needs a Cumulo key. | Device menu: Settings, Sound, Dictation and Spoken replies rows cycle the same three providers. |
| **Model prices** (dollars per million input and output tokens, per model) | Web app: Assistant, Usage, Prices - one row per model of the chosen provider; a price marked ~ is a built-in estimate, "Cumulo price" is what Cumulo bills | Device menu: not shown there. A hosted Virtual Nimbus shows no Prices editor (it keeps no usage ledger yet). |
| **Provider fallback price** (any other model of a provider; Tavily: per 1,000 searches) | Web app: Assistant, Usage, Prices, the "Any other <provider> model" row (Tavily: Searches) | Device menu: not shown there. Hosted: no editor. |
| **Budgets** (tokens, dollars, or calls per provider per month, reset day) | Web app: Assistant, Usage, Budgets - the dollar cap counts every model at its price, built-in estimates included | Device menu: not shown there; the assistant reads them in `device.status` (`usage.budget[]`). Hosted: no editor. |
| **Usage graphs** (By model or By provider, tokens or dollars, 7/30/60 days) | Web app: Assistant, Usage, Spend over time (the By model / By provider choice is remembered in the browser) | Device menu: not shown there. Hosted: the graph shows its empty state. |

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
legend (Settings → Ring) is generated from the same tables the firmware uses,
so it cannot drift from what the ring does.

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
canonical home; a password is never shown back. The device screen's
**Settings > Connectivity > Wi-Fi** is the other surface: the Wi-Fi row shows the
live status, and the screen offers Publish setup network, Choose network (scan and
join), and Forget network, but not the full list editor. Networks save from either surface into the one shared
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
| Backlight per mode + rest glow + deep-dim delay | `lib/core/include/nimbus/duty.h` |
| Screensaver idle clock + stages (rest, deep-dim) | `lib/core/include/nimbus/saver.h` |
| Sound event ranks | `lib/core/include/nimbus/sfx_map.h`, `lib/core/src/sfx_map.cpp` + `docs/sfx-map.md` |
| Header (title, Wi-Fi glyph, battery icon) | `lib/core/src/tft_screens.cpp` `drawHeader` |
| Saved-network list + selection/failover | `lib/core/src/wifi_known_networks.cpp`, `wifi_policy.cpp` |
| Failover wiring to the radio (the seam) | `src/net/wifi_link.cpp` |
| Setup-AP recovery interplay | `lib/core/src/setup_ap.cpp` (`decideSetupAp`) |

Full detail: [Wi-Fi resilience](./wifi-resilience.md).
