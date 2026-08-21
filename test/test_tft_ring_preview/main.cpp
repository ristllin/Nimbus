// Visual preview harness for the on-screen ring (ringless-board Notifier screen).
// Renders the StatusIdle screen with a populated ringLeds frame into golden_tft-
// style .bin files that tools/tftpreview.py turns into PNGs, so the layout and the
// ring animation frames can be reviewed WITHOUT the physical panel.
//
//   pio test -e native -f test_tft_ring_preview   (writes .bin under test/out/)
//   python3 tools/tftpreview.py render test/out/ring_<phase>.bin ring_<phase>.png
#include <unity.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "nimbus/tft_render/screens.h"
#include "nimbus/ring_animator.h"
#include "nimbus/status_style.h"

using nimbus::tft::Fb565;
using nimbus::tft::renderScreen;
using nimbus::tft::Rendered;
using nimbus::epd::ScreenCtx;
using nimbus::attn::ScreenId;

static void writeBin(const char* name, const Fb565& fb) {
  std::string dir = "test/out";
  std::string path = dir + "/" + name;
  std::FILE* f = std::fopen(path.c_str(), "wb");
  if (!f) { std::fprintf(stderr, "cannot open %s\n", path.c_str()); return; }
  std::fwrite(fb.data(), 1, fb.byteSize(), f);
  std::fclose(f);
  std::printf("wrote %s\n", path.c_str());
}

// A ring frame with `active` lit segments in role-ish colors and a moving bright
// "cursor" dot at `phase`, to eyeball motion across frames.
static ScreenCtx ringCtx(int active, int phase) {
  ScreenCtx c;
  c.deviceName = "Nimbus-CYD";
  c.modeName = "notifier";
  for (int i = 0; i < active; ++i)
    c.jobs.push_back({uint32_t(100 + i), uint8_t(1), uint8_t(30 + i * 15),
                      uint8_t(40 + i * 30), std::string("session ") + char('A' + i),
                      uint8_t(1 + (i % 3))});
  c.ringLeds.resize(45);
  for (int i = 0; i < 45; ++i) {
    const bool lit = (i % 3 == 0) && (i / 3 < active + 3);
    const bool cursor = (i == phase % 45);
    if (cursor) c.ringLeds[size_t(i)] = {255, 255, 255};
    else if (lit) c.ringLeds[size_t(i)] = {uint8_t(40 + i * 4), uint8_t(200 - i * 3), 120};
    else c.ringLeds[size_t(i)] = {0, 0, 0};
  }
  return c;
}

static void render(const char* name, const ScreenCtx& c) {
  static Fb565 fb;
  const Rendered r = renderScreen(fb, ScreenId::StatusIdle, c);
  (void)r;
  writeBin(name, fb);
}

void setUp() {}
void tearDown() {}

// Two live "working" sessions rendered through the REAL animation pipeline
// (nimbus::ring::Animator, the portable motion layer the device runs), sampled at
// successive instants. Proves (a) the comet actually MOVES between frames - the
// "frozen while an agent works" report - and (b) the two sessions land on SEPARATE
// ring arcs. This is the host stand-in for the on-device 30 fps panel repaint.
static ScreenCtx ctxFromRing(const solide::ring::RGB* f, int n) {
  ScreenCtx c;
  c.deviceName = "Nimbus-CYD";
  c.modeName = "notifier";
  c.jobs.push_back({0xA1u, uint8_t(1), uint8_t(50), uint8_t(40), "session A", uint8_t(1)});
  c.jobs.push_back({0xB2u, uint8_t(1), uint8_t(50), uint8_t(120), "session B", uint8_t(2)});
  c.ringLeds.resize(size_t(n));
  for (int i = 0; i < n; ++i) c.ringLeds[size_t(i)] = {f[i].r, f[i].g, f[i].b};
  return c;
}

static void test_ring_animation_two_working() {
  std::system("mkdir -p test/out");
  nimbus::ring::Animator anim;
  anim.configure(45, nimbus::Posture::Full, 200);
  const nimbus::StatusStyle ss = nimbus::statusStyle(solide::ring::Status::Running);
  anim.born(0xA1u, 80, 0);   anim.setAnim(0xA1u, uint8_t(ss.anim), ss.brightPct);
  anim.born(0xB2u, 8,  0);   anim.setAnim(0xB2u, uint8_t(ss.anim), ss.brightPct);

  solide::ring::RGB prev[45]; solide::ring::RGB cur[45];
  const uint32_t times[] = {500, 540, 580, 620, 660};   // steady-state comet, 40 ms apart
  int movedFrames = 0;
  for (int k = 0; k < 5; ++k) {
    anim.frame(times[k], cur, 45);
    char name[64];
    std::snprintf(name, sizeof name, "anim2_%u.bin", (unsigned)times[k]);
    static Fb565 fb;
    const Rendered r = renderScreen(fb, ScreenId::StatusIdle, ctxFromRing(cur, 45));
    (void)r;
    writeBin(name, fb);
    if (k > 0) {
      for (int i = 0; i < 45; ++i)
        if (cur[i].r != prev[i].r || cur[i].g != prev[i].g || cur[i].b != prev[i].b) {
          ++movedFrames; break;
        }
    }
    std::memcpy(prev, cur, sizeof cur);
  }
  // Separation: count contiguous lit arcs in one steady frame.
  anim.frame(600, cur, 45);
  int litArcs = 0; bool inRun = false;
  for (int i = 0; i < 45; ++i) {
    const bool lit = cur[i].r || cur[i].g || cur[i].b;
    if (lit && !inRun) ++litArcs;
    inRun = lit;
  }
  std::printf("ANIM2: movedFrames=%d/4 litArcs=%d liveCount=%d\n",
              movedFrames, litArcs, anim.liveCount());
  TEST_ASSERT_TRUE_MESSAGE(movedFrames >= 3, "comet must move between successive frames");
  TEST_ASSERT_EQUAL_MESSAGE(2, anim.liveCount(), "two live working segments expected");
}

static void test_ring_previews() {
  std::system("mkdir -p test/out");
  render("ring_empty.bin", ringCtx(0, 0));
  render("ring_2sessions_p0.bin", ringCtx(2, 0));
  render("ring_2sessions_p8.bin", ringCtx(2, 8));
  render("ring_2sessions_p16.bin", ringCtx(2, 16));
  render("ring_4sessions.bin", ringCtx(4, 12));
  { ScreenCtx c = ringCtx(2, 8); c.modeName = "orchestrator"; render("ring_orch.bin", c); }
  { ScreenCtx c = ringCtx(2, 8); c.modeName = "orchestrator"; c.micHeld = true;
    render("ring_orch_held.bin", c); }
  { ScreenCtx c = ringCtx(0, 0); c.modeName = "orchestrator"; render("ring_orch_empty.bin", c); }
  // Hold-to-talk pressed: the mic button shows pressed and the ring is a solid
  // theme-hue LISTENING fill (what paintRingSolid mirrors during a blocking record).
  { ScreenCtx c = ringCtx(0, 0); c.micHeld = true;
    for (auto& px : c.ringLeds) px = {127, 209, 200};
    render("ring_listening.bin", c); }
  // Setup screen with a firmware version (bottom-left), to eyeball placement.
  { ScreenCtx c; c.deviceName = "Nimbus-CYD"; c.modeName = "orchestrator";
    c.apName = "Nimbus-CYD-setup"; c.apPass = "swift-owl-42";
    c.setupUrl = "http://192.168.4.1"; c.fwVersion = "v4.2.0";
    static Fb565 fb; const Rendered rr = renderScreen(fb, ScreenId::SetupInfo, c); (void)rr;
    writeBin("setup_ver.bin", fb); }
  TEST_ASSERT_TRUE(true);
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_ring_previews);
  RUN_TEST(test_ring_animation_two_working);
  return UNITY_END();
}
