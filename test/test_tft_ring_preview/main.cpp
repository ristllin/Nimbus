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

static void test_ring_previews() {
  std::system("mkdir -p test/out");
  render("ring_empty.bin", ringCtx(0, 0));
  render("ring_2sessions_p0.bin", ringCtx(2, 0));
  render("ring_2sessions_p8.bin", ringCtx(2, 8));
  render("ring_2sessions_p16.bin", ringCtx(2, 16));
  render("ring_4sessions.bin", ringCtx(4, 12));
  { ScreenCtx c = ringCtx(2, 8); c.modeName = "orchestrator"; render("ring_orch.bin", c); }
  { ScreenCtx c = ringCtx(0, 0); c.modeName = "orchestrator"; render("ring_orch_empty.bin", c); }
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
  return UNITY_END();
}
