#include <unity.h>

#include <initializer_list>

#include "nimbus/ring_plan.h"
#include "nimbus_config.h"

using namespace nimbus;
using namespace nimbus::ring;
using solide::ring::Anim;
using solide::ring::Status;

void setUp() {}
void tearDown() {}

// ---- helpers: build router state ONLY via route() ---------------------------

// accent < 0 => no accent; 0..255 => a provider accent (255 = white).
static void jobState(attn::Router& r, uint32_t key, Status st, uint32_t t,
                     int accent = -1) {
  attn::Event e;
  e.type = attn::Event::Type::JobState;
  e.key = key;
  e.status = uint8_t(st);
  if (accent >= 0) {
    e.hasAccent = true;
    e.accentHue = uint8_t(accent);
  }
  r.route(e, t);
}

static void jobProgress(attn::Router& r, uint32_t key, uint8_t pct, uint32_t t) {
  attn::Event e;
  e.type = attn::Event::Type::JobProgress;
  e.key = key;
  e.value = pct;
  r.route(e, t);
}

static void lowBattery(attn::Router& r, uint32_t t) {
  attn::Event e;
  e.type = attn::Event::Type::LowBattery;
  e.value = 15;
  r.route(e, t);
}

static void voice(attn::Router& r, attn::VoiceStage s, uint32_t t) {
  attn::Event e;
  e.type = attn::Event::Type::Voice;
  e.stage = s;
  r.route(e, t);
}

// ---- cursor ------------------------------------------------------------------

static void test_cursor_wraps_both_directions() {
  Cursor c;
  TEST_ASSERT_EQUAL_UINT8(0, c.index());

  c.onDetent(-1, NIMBUS_RING_LEDS, 100);  // negative from 0 wraps to the end
  TEST_ASSERT_EQUAL_UINT8(NIMBUS_RING_LEDS - 1, c.index());

  c.onDetent(-1, NIMBUS_RING_LEDS, 200);
  TEST_ASSERT_EQUAL_UINT8(NIMBUS_RING_LEDS - 2, c.index());

  c.onDetent(+1, NIMBUS_RING_LEDS, 300);
  c.onDetent(+1, NIMBUS_RING_LEDS, 400);  // forward across the wrap point
  TEST_ASSERT_EQUAL_UINT8(0, c.index());

  for (int i = 0; i < NIMBUS_RING_LEDS; ++i)  // one full lap lands home
    c.onDetent(+1, NIMBUS_RING_LEDS, 500 + i);
  TEST_ASSERT_EQUAL_UINT8(0, c.index());

  c.onDetent(+1, 0, 600);  // degenerate ring: ignored, no divide-by-zero
  TEST_ASSERT_EQUAL_UINT8(0, c.index());
}

static void test_cursor_decay_expiry_and_reset() {
  Cursor c;
  TEST_ASSERT_FALSE(c.activeAt(0, 1500));  // never moved: inactive at any time

  c.onDetent(+1, NIMBUS_RING_LEDS, 1000);
  TEST_ASSERT_TRUE(c.activeAt(1000, 1500));   // immediately active
  TEST_ASSERT_TRUE(c.activeAt(2499, 1500));   // last live ms
  TEST_ASSERT_FALSE(c.activeAt(2500, 1500));  // strict <: expired exactly at decay
  TEST_ASSERT_FALSE(c.activeAt(9999, 1500));

  c.onDetent(+1, NIMBUS_RING_LEDS, 3000);  // a new detent revives the glow
  TEST_ASSERT_TRUE(c.activeAt(3100, 1500));
  c.reset();
  TEST_ASSERT_FALSE(c.activeAt(3100, 1500));  // reset clears moved outright
}

static void test_cursor_decay_survives_u32_clock_wrap() {
  Cursor c;
  c.onDetent(+1, NIMBUS_RING_LEDS, 0xFFFFFF00u);  // 256ms before uint32 wrap
  // 0x100 - 0xFFFFFF00 == 0x200 == 512ms elapsed across the wrap.
  TEST_ASSERT_TRUE(c.activeAt(0x00000100u, 1500));
  TEST_ASSERT_FALSE(c.activeAt(0x00000600u, 1500));  // 1792ms: expired
}

// ---- passive posture -----------------------------------------------------------

static void test_passive_dark_when_idle() {
  attn::Router r;
  Config cfg;  // Balanced default is now Calm; pin Dark for the minimal-ring case
  cfg.setOverride(Param::Posture, int(Posture::Dark));
  Cursor cur;

  Plan p = compose(r, cfg, cur, /*panelBusy=*/false, /*nowMs=*/0);
  TEST_ASSERT_EQUAL(int(Posture::Dark), int(p.posture));
  TEST_ASSERT_EQUAL_UINT8(30, p.brightness);
  TEST_ASSERT_EQUAL(0, p.segCount);
  TEST_ASSERT_FALSE(p.single.lit);
  TEST_ASSERT_FALSE(p.cursor.active);
  TEST_ASSERT_FALSE(p.cursor.syncing);
  TEST_ASSERT_EQUAL(int(attn::VoiceStage::None), int(p.voice));

  // Ambient job states (Running/Done) never light the passive LED.
  jobState(r, 1, Status::Running, 100);
  jobState(r, 2, Status::Done, 200);
  p = compose(r, cfg, cur, false, 300);
  TEST_ASSERT_EQUAL(0, p.segCount);
  TEST_ASSERT_FALSE(p.single.lit);
}

// Calm level = Dark + a soft orchestrator-activity cue when nothing more urgent owns
// the single LED: a "working" breathe while a turn runs, a brief blink just after a
// sub-agent starts/finishes. Dark ignores both (stays silent); Full uses arcs instead.
static void test_calm_activity_cue() {
  attn::Router r;
  Config cfg;
  cfg.setOverride(Param::Posture, int(Posture::Calm));
  Cursor cur;

  // Idle Calm (no turn, no recent activity) -> dark, same as Dark level.
  Plan p = compose(r, cfg, cur, false, 1000);
  TEST_ASSERT_EQUAL(int(Posture::Calm), int(p.posture));
  TEST_ASSERT_FALSE(p.single.lit);

  // A turn running -> a soft "working" breathe on the single LED.
  p = compose(r, cfg, cur, false, 1000, {.orchWorking = true});
  TEST_ASSERT_TRUE(p.single.lit);
  TEST_ASSERT_EQUAL_UINT8(uint8_t(Anim::Breathe), p.single.anim);

  // A very recent sub-agent start/finish -> a QUICK soft breathe (wins over the
  // slow working breathe; distinguished by tempo - ambient grammar: no strobes,
  // the old 4 Hz hard blink is banned).
  p = compose(r, cfg, cur, false, /*nowMs=*/1000, {.orchWorking = true, .lastActivityMs = 900});
  TEST_ASSERT_TRUE(p.single.lit);
  TEST_ASSERT_EQUAL_UINT8(uint8_t(Anim::Breathe), p.single.anim);
  TEST_ASSERT_EQUAL_UINT16(800, p.single.periodMs);   // quick swell, not the 2400 working pace

  // Stale activity (outside the window) with no turn -> back to dark.
  p = compose(r, cfg, cur, false, /*nowMs=*/5000, {.lastActivityMs = 900});
  TEST_ASSERT_FALSE(p.single.lit);

  // Same activity in DARK level -> ignored; Dark stays silent.
  cfg.setOverride(Param::Posture, int(Posture::Dark));
  p = compose(r, cfg, cur, false, 1000, {.orchWorking = true, .lastActivityMs = 900});
  TEST_ASSERT_FALSE(p.single.lit);
}

// Regression, the Balanced 24/7 breathe (owner x2): compose()'s old positional
// tail let a comment reflow swallow `working, lastActivityMs` and STILL COMPILE -
// 1500 shifted into the orchWorking bool and an idle ring breathed forever
// (d3a800d; resurrected by a pre-fix branch build). ComposeOpts must default to
// the FAIL-DARK direction so any dropped field can only ever UNDER-light.
static void test_compose_opts_defaults_are_fail_dark() {
  ComposeOpts d;
  TEST_ASSERT_FALSE(d.orchWorking);
  TEST_ASSERT_FALSE(d.reveal);
  TEST_ASSERT_EQUAL_UINT32(0, d.lastActivityMs);
  attn::Router r;
  Config cfg;
  cfg.setOverride(Param::Posture, int(Posture::Calm));
  Cursor cur;
  Plan p = compose(r, cfg, cur, false, 123456, ComposeOpts{});   // idle + all defaults
  TEST_ASSERT_FALSE(p.single.lit);
  TEST_ASSERT_EQUAL(0, p.segCount);   // fully dark - nothing composed anywhere
}

static void test_passive_single_follows_top_attention_auto_hue() {
  attn::Router r;
  Config cfg;  // AttnHue preset -1 (auto), index 0, anim Breathe, 2400ms
  Cursor cur;

  jobState(r, 1, Status::WaitingInput, 100);
  Plan p = compose(r, cfg, cur, false, 200);
  TEST_ASSERT_EQUAL(0, p.segCount);  // ring stays dark around the single LED
  TEST_ASSERT_TRUE(p.single.lit);
  TEST_ASSERT_EQUAL_UINT8(0, p.single.index);
  TEST_ASSERT_EQUAL_UINT8(213, p.single.hue);  // styleFor(WaitingInput)
  TEST_ASSERT_EQUAL_UINT8(uint8_t(Anim::Breathe), p.single.anim);
  TEST_ASSERT_EQUAL_UINT16(2400, p.single.periodMs);

  // Lower-priority Error joins: WaitingInput (pri 3) still wins over Error (2).
  jobState(r, 2, Status::Error, 300);
  p = compose(r, cfg, cur, false, 400);
  TEST_ASSERT_EQUAL_UINT8(213, p.single.hue);

  // AwaitingApproval (pri 4) takes over: amber.
  jobState(r, 3, Status::AwaitingApproval, 500);
  p = compose(r, cfg, cur, false, 600);
  TEST_ASSERT_EQUAL_UINT8(32, p.single.hue);  // styleFor(AwaitingApproval)

  // Attention sources resolved, but a finished (Done) job now shows a lowest-
  // precedence Calm glance cue (owner: "on balanced I can't see what finished").
  // job3 is Done, job1 Running (ambient, not surfaced), job2 Offline (gone).
  jobState(r, 1, Status::Running, 700);
  jobState(r, 2, Status::Offline, 800);
  jobState(r, 3, Status::Done, 900);
  p = compose(r, cfg, cur, false, 1000);
  TEST_ASSERT_TRUE(p.single.lit);                          // Done is glanceable in Calm now
  TEST_ASSERT_EQUAL_UINT8(85, p.single.hue);               // styleFor(Done)
  TEST_ASSERT_EQUAL_UINT8(uint8_t(Anim::Fade), p.single.anim);
}

// Owner field fix: on Balanced (Calm), a finished (Done) session must be glanceable -
// previously the Calm single LED only lit for CTAs/voice/activity, so "what finished"
// was invisible. Done is LOWEST precedence: a CTA or the orchestrator-working breathe
// still wins, and Dark is unchanged.
static void test_calm_done_is_glanceable() {
  Config cfg;   // Balanced default = Calm
  Cursor cur;

  // 1) Done-only -> single lit at the Done style (green / fade).
  {
    attn::Router r;
    jobState(r, 1, Status::Done, 100);
    Plan p = compose(r, cfg, cur, false, 200);
    TEST_ASSERT_EQUAL(0, p.segCount);                 // ring around it stays dark
    TEST_ASSERT_TRUE(p.single.lit);
    TEST_ASSERT_EQUAL_UINT8(85, p.single.hue);        // styleFor(Done)
    TEST_ASSERT_EQUAL_UINT8(uint8_t(Anim::Fade), p.single.anim);
  }
  // 2) A CTA alongside Done -> the CTA (attention) wins the single LED, not Done.
  {
    attn::Router r;
    jobState(r, 1, Status::Done, 100);
    jobState(r, 2, Status::WaitingInput, 200);
    Plan p = compose(r, cfg, cur, false, 300);
    TEST_ASSERT_TRUE(p.single.lit);
    TEST_ASSERT_EQUAL_UINT8(213, p.single.hue);       // styleFor(WaitingInput), not 85
  }
  // 3) Done + orchestrator actively working -> the "working" breathe wins (Done is
  //    placed AFTER the activity glow, so it can't suppress the "I'm working" signal).
  {
    attn::Router r;
    jobState(r, 1, Status::Done, 100);
    Plan p = compose(r, cfg, cur, false, 200, {.orchWorking = true});
    TEST_ASSERT_TRUE(p.single.lit);
    TEST_ASSERT_EQUAL_UINT8(uint8_t(Anim::Breathe), p.single.anim);  // working, not Fade
    TEST_ASSERT_EQUAL_UINT8(127, p.single.hue);       // themeHue default, not the Done hue
  }
  // 4) DARK posture is unchanged: Done does NOT light the single LED there.
  {
    Config dark;
    dark.setOverride(Param::Posture, int(Posture::Dark));
    attn::Router r;
    jobState(r, 1, Status::Done, 100);
    Plan p = compose(r, dark, cur, false, 200);
    TEST_ASSERT_FALSE(p.single.lit);
  }
}

// Owner 2026-07-16: Dark is a "disengage the LEDs" posture - it shows NOTHING for
// any ambient/CTA status EXCEPT Error, which still surfaces a calm red breathe so a
// failure can't silently vanish on a dark desk. The web ring simulator mirrors this
// exactly (Dark -> off unless Error). Calm keeps the single themed cue for all CTAs.
static void test_dark_disengages_except_error() {
  Config dark;
  dark.setOverride(Param::Posture, int(Posture::Dark));
  Cursor cur;

  // A CTA (WaitingInput) in Dark -> stays dark (was lit before this change).
  {
    attn::Router r;
    jobState(r, 1, Status::WaitingInput, 100);
    Plan p = compose(r, dark, cur, false, 200);
    TEST_ASSERT_EQUAL(int(Posture::Dark), int(p.posture));
    TEST_ASSERT_FALSE(p.single.lit);
  }
  // AwaitingApproval in Dark -> also dark (only Error breaks the silence).
  {
    attn::Router r;
    jobState(r, 1, Status::AwaitingApproval, 100);
    Plan p = compose(r, dark, cur, false, 200);
    TEST_ASSERT_FALSE(p.single.lit);
  }
  // Error in Dark -> the ONE exception: a single breathe lights at the alert hue.
  {
    attn::Router r;
    jobState(r, 1, Status::Error, 100);
    Plan p = compose(r, dark, cur, false, 200, {.attnThemedHue = 7});
    TEST_ASSERT_TRUE(p.single.lit);
    TEST_ASSERT_EQUAL_UINT8(7, p.single.hue);                        // themed alert hue
    TEST_ASSERT_EQUAL_UINT8(uint8_t(Anim::Breathe), p.single.anim);  // calm breathe, no strobe
  }
  // Calm is UNCHANGED: the same CTA still lights the themed cue there (regression guard).
  {
    Config calm;  // Balanced default = Calm
    attn::Router r;
    jobState(r, 1, Status::WaitingInput, 100);
    Plan p = compose(r, calm, cur, false, 200);
    TEST_ASSERT_TRUE(p.single.lit);
  }
}

static void test_passive_single_user_overrides() {
  attn::Router r;
  Config cfg;
  Cursor cur;
  jobState(r, 1, Status::WaitingInput, 100);

  cfg.setOverride(Param::AttnHue, 96);  // user pins a hue: auto is off
  cfg.setOverride(Param::AttnLedIndex, 7);
  cfg.setOverride(Param::AttnAnim, int32_t(Anim::Blink));
  cfg.setOverride(Param::AttnPeriodMs, 800);
  Plan p = compose(r, cfg, cur, false, 200);
  TEST_ASSERT_TRUE(p.single.lit);
  TEST_ASSERT_EQUAL_UINT8(96, p.single.hue);
  TEST_ASSERT_EQUAL_UINT8(7, p.single.index);
  TEST_ASSERT_EQUAL_UINT8(uint8_t(Anim::Blink), p.single.anim);
  TEST_ASSERT_EQUAL_UINT16(800, p.single.periodMs);

  cfg.clearOverride(Param::AttnHue);  // back to auto: state hue returns
  p = compose(r, cfg, cur, false, 300);
  TEST_ASSERT_EQUAL_UINT8(213, p.single.hue);

  // Out-of-range index overrides clamp to the physical ring.
  cfg.setOverride(Param::AttnLedIndex, 200);
  p = compose(r, cfg, cur, false, 400);
  TEST_ASSERT_EQUAL_UINT8(NIMBUS_RING_LEDS - 1, p.single.index);
  cfg.setOverride(Param::AttnLedIndex, -3);
  p = compose(r, cfg, cur, false, 500);
  TEST_ASSERT_EQUAL_UINT8(0, p.single.index);
}

static void test_voice_takeover_per_stage_and_release() {
  attn::Router r;
  Config cfg;
  Cursor cur;
  cfg.setOverride(Param::AttnLedIndex, 7);
  jobState(r, 1, Status::WaitingInput, 100);  // live attention underneath

  struct { attn::VoiceStage stage; Anim anim; } steps[] = {
      {attn::VoiceStage::Recording, Anim::Breathe},
      {attn::VoiceStage::Processing, Anim::Comet},
      {attn::VoiceStage::Speaking, Anim::Solid},
  };
  for (auto& s : steps) {
    voice(r, s.stage, 200);
    Plan p = compose(r, cfg, cur, false, 300);
    TEST_ASSERT_EQUAL(int(s.stage), int(p.voice));
    TEST_ASSERT_TRUE(p.single.lit);
    TEST_ASSERT_EQUAL_UINT8(170, p.single.hue);  // voice blue, not 213
    TEST_ASSERT_EQUAL_UINT8(uint8_t(s.anim), p.single.anim);
    TEST_ASSERT_EQUAL_UINT8(7, p.single.index);       // configured index kept
    TEST_ASSERT_EQUAL_UINT16(2400, p.single.periodMs);  // configured period kept
  }

  // Release: the attention LED gets its own treatment back.
  voice(r, attn::VoiceStage::None, 400);
  Plan p = compose(r, cfg, cur, false, 500);
  TEST_ASSERT_EQUAL(int(attn::VoiceStage::None), int(p.voice));
  TEST_ASSERT_TRUE(p.single.lit);
  TEST_ASSERT_EQUAL_UINT8(213, p.single.hue);
  TEST_ASSERT_EQUAL_UINT8(uint8_t(Anim::Breathe), p.single.anim);

  // Voice lights the LED even with no attention source at all...
  jobState(r, 1, Status::Offline, 600);
  voice(r, attn::VoiceStage::Recording, 700);
  p = compose(r, cfg, cur, false, 800);
  TEST_ASSERT_TRUE(p.single.lit);
  TEST_ASSERT_EQUAL_UINT8(170, p.single.hue);

  // ...and releasing with nothing underneath goes fully dark.
  voice(r, attn::VoiceStage::None, 900);
  p = compose(r, cfg, cur, false, 1000);
  TEST_ASSERT_FALSE(p.single.lit);
}

// ---- active posture ------------------------------------------------------------

static void test_active_snapshot_passthrough() {
  attn::Router r;
  Config cfg;
  cfg.setProfile(ProfileId::Desk);  // Active posture, brightness 60
  Cursor cur;

  jobState(r, 11, Status::Running, 100, /*accent=*/10);
  jobState(r, 22, Status::WaitingInput, 200, /*accent=*/200);
  jobProgress(r, 11, 55, 300);

  Plan p = compose(r, cfg, cur, false, 400);
  TEST_ASSERT_EQUAL(int(Posture::Full), int(p.posture));
  TEST_ASSERT_EQUAL_UINT8(60, p.brightness);
  TEST_ASSERT_EQUAL(2, p.segCount);

  TEST_ASSERT_EQUAL_UINT32(11, p.segs[0].key);
  TEST_ASSERT_EQUAL(int(Status::Running), int(p.segs[0].status));
  TEST_ASSERT_TRUE(p.segs[0].hasAccent);
  TEST_ASSERT_EQUAL_UINT8(10, p.segs[0].accentHue);
  TEST_ASSERT_EQUAL_UINT8(55, p.segs[0].progress);

  TEST_ASSERT_EQUAL_UINT32(22, p.segs[1].key);
  TEST_ASSERT_EQUAL(int(Status::WaitingInput), int(p.segs[1].status));
  TEST_ASSERT_EQUAL_UINT8(200, p.segs[1].accentHue);

  TEST_ASSERT_FALSE(p.single.lit);  // single is a Passive-only concept

  // Voice stage is carried alongside the segments (glue applies it globally).
  voice(r, attn::VoiceStage::Processing, 500);
  p = compose(r, cfg, cur, false, 600);
  TEST_ASSERT_EQUAL(int(attn::VoiceStage::Processing), int(p.voice));
  TEST_ASSERT_EQUAL(2, p.segCount);

  // Offline frees the slot; the snapshot compacts in ring order.
  jobState(r, 11, Status::Offline, 700);
  p = compose(r, cfg, cur, false, 800);
  TEST_ASSERT_EQUAL(1, p.segCount);
  TEST_ASSERT_EQUAL_UINT32(22, p.segs[0].key);
}

// Full (Desk) with no jobs and no reveal: the ring is FULLY DARK (owner 2026-07-15
// - an all-day desk ring glowing white with nothing happening reads as "stuck / on
// for no reason"; a bright steady white was the field symptom). A CTA or a reveal
// still lights it (covered below). Supersedes the earlier faint-heartbeat design.
static void test_full_idle_is_dark_when_no_jobs() {
  attn::Router r;
  Config cfg;
  cfg.setProfile(ProfileId::Desk);  // Full, brightness 60
  Cursor cur;

  Plan p = compose(r, cfg, cur, false, 100);
  TEST_ASSERT_EQUAL(int(Posture::Full), int(p.posture));
  TEST_ASSERT_EQUAL(0, p.segCount);                       // DARK, not a heartbeat
  TEST_ASSERT_FALSE(p.single.lit);

  // A real job lights an arc at full brightness (no phantom idle slot alongside).
  jobState(r, 7, Status::Running, 200, /*accent=*/30);
  p = compose(r, cfg, cur, false, 300);
  TEST_ASSERT_EQUAL(1, p.segCount);
  TEST_ASSERT_EQUAL_UINT32(7, p.segs[0].key);
  TEST_ASSERT_EQUAL(int(Status::Running), int(p.segs[0].status));
  TEST_ASSERT_EQUAL_UINT8(60, p.brightness);              // full brightness

  // Voice stage also leaves no idle slot (voice owns the ring globally in Full).
  jobState(r, 7, Status::Offline, 400);
  voice(r, attn::VoiceStage::Recording, 450);
  p = compose(r, cfg, cur, false, 500);
  TEST_ASSERT_EQUAL(0, p.segCount);                       // no idle slot under voice
  TEST_ASSERT_EQUAL(int(attn::VoiceStage::Recording), int(p.voice));
}

// "Wake the ring" reveal: a single click promotes ANY posture to the full segment
// treatment so live jobs are glanceable even from Dark (normally one LED / dark).
static void test_reveal_promotes_dark_to_full_segments() {
  attn::Router r;
  Config cfg;
  cfg.setProfile(ProfileId::BatterySaver);  // Dark
  Cursor cur;
  jobState(r, 1, Status::Running, 100, /*accent=*/170);
  jobState(r, 2, Status::AwaitingApproval, 100, /*accent=*/32);

  // Normal Dark: fully disengaged. The top CTA here is AwaitingApproval (non-error),
  // so Dark shows NOTHING - no segments AND no single LED (owner 2026-07-16: Dark is
  // a "turn the LEDs off" posture, only Error breaks the silence). The reveal below
  // is what makes these jobs glanceable from Dark.
  Plan p = compose(r, cfg, cur, false, 200);
  TEST_ASSERT_EQUAL(int(Posture::Dark), int(p.posture));
  TEST_ASSERT_EQUAL(0, p.segCount);
  TEST_ASSERT_FALSE(p.single.lit);

  // Revealing: promoted to Full - both jobs show as arcs, single LED goes unlit.
  p = compose(r, cfg, cur, false, 200, {.reveal = true});
  TEST_ASSERT_EQUAL(int(Posture::Full), int(p.posture));
  TEST_ASSERT_EQUAL(2, p.segCount);
  TEST_ASSERT_FALSE(p.single.lit);
}

// A single-click reveal LIGHTS an otherwise-dark idle Desk ring (wake feedback) at
// full brightness, so the click visibly registers; without the reveal the idle ring
// is dark (owner 2026-07-15).
static void test_reveal_lights_dark_desk_idle() {
  attn::Router r;
  Config cfg;
  cfg.setProfile(ProfileId::Desk);  // Full, brightness 60
  Cursor cur;

  Plan dark = compose(r, cfg, cur, false, 100);
  TEST_ASSERT_EQUAL(0, dark.segCount);                    // idle -> DARK

  Plan lit = compose(r, cfg, cur, false, 100, {.reveal = true});
  TEST_ASSERT_EQUAL(1, lit.segCount);                     // reveal lights the ring
  TEST_ASSERT_EQUAL(int(Status::Idle), int(lit.segs[0].status));
  TEST_ASSERT_EQUAL_UINT8(60, lit.brightness);            // full brightness while revealing
}

// Phase 4 voice UX: the session cursor's hue is the accent of the segment it
// points at, and it clamps to white(255) when the index falls past a shrunk list.
static void test_cursor_hue_tracks_focused_session() {
  attn::Router r;
  Config cfg;
  cfg.setProfile(ProfileId::Desk);  // Active posture -> segments present
  Cursor cur;
  jobState(r, 11, Status::Running, 100, /*accent=*/23);   // segs[0]
  jobState(r, 22, Status::Running, 200, /*accent=*/140);  // segs[1]

  Plan p = compose(r, cfg, cur, false, 300);
  TEST_ASSERT_EQUAL(2, p.segCount);
  TEST_ASSERT_EQUAL_UINT8(23, p.cursor.hue);   // cursor at index 0 -> segs[0]

  cur.onDetent(+1, p.segCount, 400);           // move to index 1
  p = compose(r, cfg, cur, false, 500);
  TEST_ASSERT_EQUAL_UINT8(1, p.cursor.index);
  TEST_ASSERT_EQUAL_UINT8(140, p.cursor.hue);  // -> segs[1]

  // A cursor index left past the end (list shrank on another task) -> white, no OOB.
  Cursor far;
  for (int i = 0; i < 5; i++) far.onDetent(+1, NIMBUS_RING_LEDS, 600 + i);  // index 5
  p = compose(r, cfg, far, false, 700);
  TEST_ASSERT_TRUE(far.index() >= p.segCount);
  TEST_ASSERT_EQUAL_UINT8(255, p.cursor.hue);
}

// ---- brightness ---------------------------------------------------------------

static void test_brightness_preset_vs_override_with_clamping() {
  attn::Router r;
  Config cfg;
  Cursor cur;
  // A live job so this stays a pure test of the brightness param pipeline: on
  // Full, an IDLE ring dims to the heartbeat level (covered separately in
  // test_full_idle_glow_when_no_jobs), which would otherwise mask the preset.
  jobState(r, 1, Status::Running, 0);

  cfg.setProfile(ProfileId::BatterySaver);
  TEST_ASSERT_EQUAL_UINT8(10, compose(r, cfg, cur, false, 0).brightness);
  cfg.setProfile(ProfileId::Balanced);
  TEST_ASSERT_EQUAL_UINT8(30, compose(r, cfg, cur, false, 0).brightness);

  cfg.setOverride(Param::RingBrightness, 200);  // override beats preset
  TEST_ASSERT_EQUAL_UINT8(200, compose(r, cfg, cur, false, 0).brightness);
  cfg.setProfile(ProfileId::Desk);  // and survives a profile switch
  TEST_ASSERT_EQUAL_UINT8(200, compose(r, cfg, cur, false, 0).brightness);

  cfg.setOverride(Param::RingBrightness, 999);  // out-of-range clamps, not wraps
  TEST_ASSERT_EQUAL_UINT8(255, compose(r, cfg, cur, false, 0).brightness);
  cfg.setOverride(Param::RingBrightness, -5);
  TEST_ASSERT_EQUAL_UINT8(0, compose(r, cfg, cur, false, 0).brightness);

  cfg.clearOverride(Param::RingBrightness);  // preset shines through again
  TEST_ASSERT_EQUAL_UINT8(60, compose(r, cfg, cur, false, 0).brightness);
}

// ---- cursor glow / panel sync ---------------------------------------------------

static void test_cursor_glow_and_syncing_while_panel_busy() {
  attn::Router r;
  Config cfg;
  Cursor cur;

  // A busy panel alone never invents a glow before the first detent.
  Plan p = compose(r, cfg, cur, /*panelBusy=*/true, 0);
  TEST_ASSERT_FALSE(p.cursor.active);
  TEST_ASSERT_TRUE(p.cursor.syncing);  // syncing mirrors panelBusy regardless

  cur.onDetent(+1, NIMBUS_RING_LEDS, 1000);
  p = compose(r, cfg, cur, false, 1200);
  TEST_ASSERT_TRUE(p.cursor.active);
  TEST_ASSERT_EQUAL_UINT8(1, p.cursor.index);
  TEST_ASSERT_FALSE(p.cursor.syncing);

  // Past decay with an idle panel: glow gone.
  p = compose(r, cfg, cur, false, 5000);
  TEST_ASSERT_FALSE(p.cursor.active);

  // Past decay but the panel is rendering INSIDE the interaction's own sync
  // window (4000 ms after the detent < NIMBUS_CURSOR_SYNC_HOLD_MS): glow held
  // as the syncing shimmer.
  p = compose(r, cfg, cur, true, 5000);
  TEST_ASSERT_TRUE(p.cursor.active);
  TEST_ASSERT_TRUE(p.cursor.syncing);
  TEST_ASSERT_EQUAL_UINT8(1, p.cursor.index);

  // Beyond the sync-hold window: a busy panel is doing unrelated work (an
  // ambient flush or attention screen), and must NOT resurrect the stale glow.
  // Detent was at t=1000; NIMBUS_CURSOR_SYNC_HOLD_MS past that is 5700, so at
  // t=6000 the Passive ring stays dark even while the panel renders.
  p = compose(r, cfg, cur, true, 6000);
  TEST_ASSERT_FALSE(p.cursor.active);
  TEST_ASSERT_TRUE(p.cursor.syncing);

  // Same contract in Active posture.
  cfg.setProfile(ProfileId::Desk);
  p = compose(r, cfg, cur, true, 5000);
  TEST_ASSERT_EQUAL(int(Posture::Full), int(p.posture));
  TEST_ASSERT_TRUE(p.cursor.active);
  TEST_ASSERT_TRUE(p.cursor.syncing);

  // Custom decay window is honoured.
  p = compose(r, cfg, cur, false, 1400, {.cursorDecayMs = 500});
  TEST_ASSERT_TRUE(p.cursor.active);
  p = compose(r, cfg, cur, false, 1500, {.cursorDecayMs = 500});
  TEST_ASSERT_FALSE(p.cursor.active);

  // reset() forgets the cursor entirely: even a busy panel shows no glow.
  cur.reset();
  p = compose(r, cfg, cur, true, 5000);
  TEST_ASSERT_FALSE(p.cursor.active);
  TEST_ASSERT_TRUE(p.cursor.syncing);
}


// ---- low-battery cue (owner 2026-07-29) --------------------------------------
// The warning used to be a CONTINUOUS FULL-BRIGHTNESS red breathe held for as long
// as the battery stayed low - the brightest thing the device does, running exactly
// when there is least power for it. It is now owner-opt-in and, when on, recessive.

static void test_low_battery_light_is_off_by_default() {
  Cursor cur;
  // Default ComposeOpts (lowBattCue == false) in EVERY battery mode.
  for (Posture post : {Posture::Dark, Posture::Calm, Posture::Full}) {
    Config cfg;
    cfg.setOverride(Param::Posture, int(post));
    attn::Router r;
    lowBattery(r, 100);
    Plan p = compose(r, cfg, cur, false, 200);
    TEST_ASSERT_FALSE_MESSAGE(p.single.lit, "low battery lit the single cue by default");
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, p.segCount, "low battery lit an arc by default");
    TEST_ASSERT_FALSE(p.lowBattCue);
  }
}

static void test_low_battery_light_when_opted_in() {
  Cursor cur;
  // Full: an arc, armed for the periodic envelope AND recessive in brightness.
  {
    Config full;
    full.setOverride(Param::Posture, int(Posture::Full));
    attn::Router r;
    lowBattery(r, 100);
    Plan p = compose(r, full, cur, false, 200, {.lowBattCue = true});
    TEST_ASSERT_EQUAL_INT(1, p.segCount);
    TEST_ASSERT_TRUE(p.lowBattCue);
    TEST_ASSERT_EQUAL_UINT8(nimbus::kLowBattCueBrightPct, p.cueBrightPct);
  }
  // Dark and Calm: the single cue lights and is armed, but keeps FULL relative
  // brightness - their profile brightness (10/255, 30/255) is already the dimming,
  // and a further x0.25 there lands at ~1/255, i.e. invisible.
  for (Posture post : {Posture::Dark, Posture::Calm}) {
    Config cfg;
    cfg.setOverride(Param::Posture, int(post));
    attn::Router r;
    lowBattery(r, 100);
    Plan p = compose(r, cfg, cur, false, 200, {.lowBattCue = true});
    TEST_ASSERT_TRUE_MESSAGE(p.single.lit, "opted-in low battery did not light");
    TEST_ASSERT_TRUE(p.lowBattCue);
    TEST_ASSERT_EQUAL_UINT8(100, p.cueBrightPct);
  }
}

// THE regression guard for the whole feature: the mute keys off the attention
// SOURCE, not off Status::Error. A job that failed is not a low battery, and must
// keep its full-brightness arc with the low-battery light switched off.
static void test_job_error_is_unaffected_by_the_low_battery_setting() {
  Cursor cur;
  Config full;
  full.setOverride(Param::Posture, int(Posture::Full));
  Cursor c2;
  attn::Router r;
  jobState(r, 1, Status::Error, 100);
  Plan p = compose(r, full, c2, false, 200);      // cue OFF (the default)
  TEST_ASSERT_EQUAL_INT_MESSAGE(1, p.segCount, "a failed job stopped rendering");
  TEST_ASSERT_FALSE_MESSAGE(p.lowBattCue, "a job error was treated as a low battery");
  TEST_ASSERT_EQUAL_UINT8(100, p.cueBrightPct);

  // Dark, too: Error is Dark's one exemption and must survive the mute.
  Config dark;
  dark.setOverride(Param::Posture, int(Posture::Dark));
  attn::Router r2;
  jobState(r2, 1, Status::Error, 100);
  Plan pd = compose(r2, dark, cur, false, 200);
  TEST_ASSERT_TRUE_MESSAGE(pd.single.lit, "a job error went dark in Dark mode");
  TEST_ASSERT_FALSE(pd.lowBattCue);
}

// An ask shares kAttnGlowKey with the low-battery arc, so keying the dim off the
// key instead of the source would silently make "I have a question" subtle too.
static void test_ask_stays_bright_and_unarmed() {
  Cursor cur;
  Config full;
  full.setOverride(Param::Posture, int(Posture::Full));
  attn::Router r;
  attn::Event e;
  e.type = attn::Event::Type::IncomingAsk;
  r.route(e, 100);
  Plan p = compose(r, full, cur, false, 200);
  TEST_ASSERT_EQUAL_INT(1, p.segCount);
  TEST_ASSERT_FALSE_MESSAGE(p.lowBattCue, "the ask arc got the low-battery envelope");
  TEST_ASSERT_EQUAL_UINT8(100, p.cueBrightPct);
}

// Voice owns the LED while a stage is live; leaving the envelope armed would make
// hold-to-talk strobe on the cue's 3 s / 60 s cycle.
static void test_voice_disarms_the_envelope() {
  Cursor cur;
  Config calm;
  attn::Router r;
  lowBattery(r, 100);
  voice(r, attn::VoiceStage::Recording, 150);
  Plan p = compose(r, calm, cur, false, 200, {.lowBattCue = true});
  TEST_ASSERT_TRUE(p.single.lit);
  TEST_ASSERT_FALSE_MESSAGE(p.lowBattCue, "voice ran with the low-battery envelope armed");
}

// ---- v4.1: Orchestrator fan-out split (Balanced shows per-session arcs) -----

static void test_fanout_split_calm_shows_segments() {
  attn::Router r;
  jobState(r, 1, Status::Running, 100, /*accent=*/40);
  jobState(r, 2, Status::Running, 110, /*accent=*/200);
  jobState(r, 3, Status::Running, 120, /*accent=*/90);
  Config cfg;
  cfg.setOverride(Param::Posture, int(Posture::Calm));
  Cursor cur;
  Plan p = compose(r, cfg, cur, false, 200, {.fanoutSegments = true});
  // The Balanced ring promotes to the segment treatment: three arcs, no
  // single-LED collapse - the "see the split of active subsessions" ask.
  TEST_ASSERT_EQUAL(int(Posture::Full), int(p.posture));
  TEST_ASSERT_EQUAL(3, p.segCount);
  TEST_ASSERT_FALSE(p.single.lit);
  // Brightness stays the CALM profile's own - the split is ambient, not a reveal.
  TEST_ASSERT_EQUAL(cfg.effective(Param::RingBrightness), p.brightness);
}

static void test_fanout_split_clears_when_jobs_drain() {
  attn::Router r;
  jobState(r, 1, Status::Running, 100);
  jobState(r, 1, Status::Offline, 200);          // job reaped -> table empty
  Config cfg;
  cfg.setOverride(Param::Posture, int(Posture::Calm));
  Cursor cur;
  Plan p = compose(r, cfg, cur, false, 300, {.fanoutSegments = true});
  TEST_ASSERT_EQUAL(int(Posture::Calm), int(p.posture));   // back to the grammar
  TEST_ASSERT_EQUAL(0, p.segCount);
}

static void test_notifier_calm_grammar_unchanged_without_the_flag() {
  // The NOTIFIER never sets fanoutSegments: Calm with live jobs must stay the
  // single-LED treatment (the all-day-peripheral contract) - this is the guard
  // that the orchestrator feature cannot leak into notifier behavior.
  attn::Router r;
  jobState(r, 1, Status::Running, 100);
  jobState(r, 2, Status::Running, 110);
  Config cfg;
  cfg.setOverride(Param::Posture, int(Posture::Calm));
  Cursor cur;
  Plan p = compose(r, cfg, cur, false, 200, {});
  TEST_ASSERT_EQUAL(int(Posture::Calm), int(p.posture));
  TEST_ASSERT_EQUAL(0, p.segCount);
}

static void test_fanout_split_never_touches_dark() {
  // Dark is the owner's "fully dark unless a job needs you" contract - live
  // sub-sessions do NOT light it, even with the flag set.
  attn::Router r;
  jobState(r, 1, Status::Running, 100);
  Config cfg;
  cfg.setOverride(Param::Posture, int(Posture::Dark));
  Cursor cur;
  Plan p = compose(r, cfg, cur, false, 200, {.fanoutSegments = true});
  TEST_ASSERT_EQUAL(int(Posture::Dark), int(p.posture));
  TEST_ASSERT_EQUAL(0, p.segCount);
  TEST_ASSERT_FALSE(p.single.lit);
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_cursor_wraps_both_directions);
  RUN_TEST(test_cursor_decay_expiry_and_reset);
  RUN_TEST(test_cursor_decay_survives_u32_clock_wrap);
  RUN_TEST(test_passive_dark_when_idle);
  RUN_TEST(test_calm_activity_cue);
  RUN_TEST(test_passive_single_follows_top_attention_auto_hue);
  RUN_TEST(test_calm_done_is_glanceable);
  RUN_TEST(test_dark_disengages_except_error);
  RUN_TEST(test_passive_single_user_overrides);
  RUN_TEST(test_voice_takeover_per_stage_and_release);
  RUN_TEST(test_active_snapshot_passthrough);
  RUN_TEST(test_full_idle_is_dark_when_no_jobs);
  RUN_TEST(test_compose_opts_defaults_are_fail_dark);
  RUN_TEST(test_reveal_promotes_dark_to_full_segments);
  RUN_TEST(test_reveal_lights_dark_desk_idle);
  RUN_TEST(test_cursor_hue_tracks_focused_session);
  RUN_TEST(test_brightness_preset_vs_override_with_clamping);
  RUN_TEST(test_cursor_glow_and_syncing_while_panel_busy);
  RUN_TEST(test_low_battery_light_is_off_by_default);
  RUN_TEST(test_low_battery_light_when_opted_in);
  RUN_TEST(test_job_error_is_unaffected_by_the_low_battery_setting);
  RUN_TEST(test_ask_stays_bright_and_unarmed);
  RUN_TEST(test_voice_disarms_the_envelope);
  RUN_TEST(test_fanout_split_calm_shows_segments);
  RUN_TEST(test_fanout_split_clears_when_jobs_drain);
  RUN_TEST(test_notifier_calm_grammar_unchanged_without_the_flag);
  RUN_TEST(test_fanout_split_never_touches_dark);
  return UNITY_END();
}
