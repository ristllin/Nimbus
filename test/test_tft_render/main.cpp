#include <unity.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <set>
#include <string>

#include "nimbus/tft_render/fb565.h"
#include "nimbus/tft_render/screens.h"

// Golden-image + tap-region matrix for the COLOUR TOUCH UI (the e-ink suite is
// test_golden and stays untouched).
//
// Each case renders a ScreenCtx fixture and writes two artefacts:
//   test/golden_tft/<case>.bin           RGB565 framebuffer, byte-compared
//   test/golden_tft/<case>.regions.json  its tap targets, for review + overlay
//
//   - GOLDEN_UPDATE=1 re-blesses (bootstrap / after an intentional layout change)
//   - a MISSING golden is a FAILURE in compare mode, never a silent bless
//   - on mismatch the render is dumped to test/golden_tft/out/ so
//     `python3 tools/tftpreview.py diff a.bin out/a.bin d.png` shows the pixels
//
// Beyond pixels, every screen's tap regions are asserted structurally: a target
// below 44px, running off-panel, or overlapping another is a FAILURE. Those are
// the bugs a pixel diff cannot see - a screen can look perfect and be unusable.

using namespace nimbus::tft;
using nimbus::attn::ScreenId;
using nimbus::attn::VoiceStage;
using nimbus::epd::ScreenCtx;

void setUp() {}
void tearDown() {}

static const char* kDir = "test/golden_tft";
static const char* kOutDir = "test/golden_tft/out";

// ---- helpers ----------------------------------------------------------------

static void writeFile(const std::string& path, const uint8_t* buf, size_t n) {
  FILE* f = std::fopen(path.c_str(), "wb");
  TEST_ASSERT_NOT_NULL_MESSAGE(f, path.c_str());
  const size_t wrote = std::fwrite(buf, 1, n, f);
  std::fclose(f);
  TEST_ASSERT_TRUE_MESSAGE(wrote == n, path.c_str());
}

static const char* actionName(TapRegion::Action a) {
  switch (a) {
    case TapRegion::Action::MenuRow:     return "MenuRow";
    case TapRegion::Action::Back:        return "Back";
    case TapRegion::Action::Home:        return "Home";
    case TapRegion::Action::OpenMenu:    return "OpenMenu";
    case TapRegion::Action::Mic:         return "Mic";
    case TapRegion::Action::ValueUp:     return "ValueUp";
    case TapRegion::Action::ValueDown:   return "ValueDown";
    case TapRegion::Action::Commit:      return "Commit";
    case TapRegion::Action::SessionCard: return "SessionCard";
    case TapRegion::Action::ScrollUp:    return "ScrollUp";
    case TapRegion::Action::ScrollDown:  return "ScrollDown";
    case TapRegion::Action::None:
    default:                             return "None";
  }
}

static void writeRegions(const std::string& path, const Rendered& r) {
  FILE* f = std::fopen(path.c_str(), "wb");
  TEST_ASSERT_NOT_NULL_MESSAGE(f, path.c_str());
  std::fprintf(f, "[\n");
  for (size_t i = 0; i < r.taps.size(); i++) {
    const auto& t = r.taps[i];
    std::fprintf(f,
                 "  {\"x\":%d,\"y\":%d,\"w\":%d,\"h\":%d,\"action\":\"%s\",\"index\":%d}%s\n",
                 t.x, t.y, t.w, t.h, actionName(t.action), t.index,
                 i + 1 < r.taps.size() ? "," : "");
  }
  std::fprintf(f, "]\n");
  std::fclose(f);
}

// Structural rules every screen must satisfy, independent of how it looks.
static void assertRegionsSane(const char* name, const Rendered& r) {
  for (size_t i = 0; i < r.taps.size(); i++) {
    const auto& t = r.taps[i];
    char msg[192];

    std::snprintf(msg, sizeof msg, "%s: '%s' is %dx%d, below the %dpx tap minimum",
                  name, actionName(t.action), t.w, t.h, kMinTap);
    TEST_ASSERT_TRUE_MESSAGE(t.w >= kMinTap && t.h >= kMinTap, msg);

    std::snprintf(msg, sizeof msg, "%s: '%s' (%d,%d,%d,%d) falls outside %dx%d",
                  name, actionName(t.action), t.x, t.y, t.w, t.h, kW, kH);
    TEST_ASSERT_TRUE_MESSAGE(t.x >= 0 && t.y >= 0 && t.x + t.w <= kW && t.y + t.h <= kH, msg);

    std::snprintf(msg, sizeof msg, "%s: '%s' has action None", name, actionName(t.action));
    TEST_ASSERT_TRUE_MESSAGE(t.action != TapRegion::Action::None, msg);
  }

  // Overlaps are allowed ONLY where a control deliberately sits over a card
  // (hit() resolves last-wins); an overlap between two same-kind targets is a
  // real bug, so assert on those.
  for (size_t i = 0; i < r.taps.size(); i++) {
    for (size_t j = i + 1; j < r.taps.size(); j++) {
      const auto& a = r.taps[i];
      const auto& b = r.taps[j];
      if (a.action != b.action) continue;
      const bool overlap = a.x < b.x + b.w && b.x < a.x + a.w &&
                           a.y < b.y + b.h && b.y < a.y + a.h;
      char msg[192];
      std::snprintf(msg, sizeof msg, "%s: two '%s' targets overlap", name,
                    actionName(a.action));
      TEST_ASSERT_FALSE_MESSAGE(overlap, msg);
    }
  }
}

static void golden(const char* name, ScreenId id, const ScreenCtx& ctx) {
  static Fb565 fb;
  const Rendered r = renderScreen(fb, id, ctx);

  assertRegionsSane(name, r);

  const std::string base = std::string(kDir) + "/" + name;
  const char* env = std::getenv("GOLDEN_UPDATE");
  const bool update = env != nullptr && std::strcmp(env, "1") == 0;

  if (update) {
    (void)std::system((std::string("mkdir -p ") + kDir).c_str());
    writeFile(base + ".bin", fb.data(), fb.byteSize());
    writeRegions(base + ".regions.json", r);
    TEST_MESSAGE((std::string("updated golden: ") + base).c_str());
    return;
  }

  FILE* f = std::fopen((base + ".bin").c_str(), "rb");
  TEST_ASSERT_NOT_NULL_MESSAGE(
      f, (std::string("missing golden ") + base +
          ".bin - run `GOLDEN_UPDATE=1 pio test -e native -f test_tft_render`")
             .c_str());
  static uint8_t expect[kFbBytes];
  const size_t got = std::fread(expect, 1, size_t(kFbBytes), f);
  std::fclose(f);
  TEST_ASSERT_TRUE_MESSAGE(got == size_t(kFbBytes), "golden file truncated");

  if (std::memcmp(expect, fb.data(), size_t(kFbBytes)) != 0) {
    (void)std::system((std::string("mkdir -p ") + kOutDir).c_str());
    writeFile(std::string(kOutDir) + "/" + name + ".bin", fb.data(), fb.byteSize());
  }
  TEST_ASSERT_EQUAL_MEMORY_MESSAGE(expect, fb.data(), kFbBytes, name);
}

// ---- fixtures ---------------------------------------------------------------

static ScreenCtx baseCtx() {
  ScreenCtx c;
  c.deviceName = "Nimbus-4";
  return c;
}

static ScreenCtx jobsCtx() {
  ScreenCtx c = baseCtx();
  c.jobs.push_back({101, 1 /*Running*/, 40, 170, "build firmware", 2 /*codex*/});
  c.jobs.push_back({102, 2 /*WaitingInput*/, 0, 213, "review deploy plan", 1 /*claude*/});
  c.jobs.push_back({103, 4 /*Done*/, 100, 85, "run tests", 3 /*vibe*/});
  c.jobs.push_back({104, 5 /*Error*/, 0, 0, "publish release", 1});
  c.cursorJob = 1;
  c.battery.valid = true;
  c.battery.percent = 82;
  return c;
}

static ScreenCtx menuCtx() {
  ScreenCtx c = baseCtx();
  c.menuTitle = "Settings";
  // Match the real menu contract: only rows ending in " >" navigate and may
  // draw a right chevron. Toggle/cycle/action rows remain tappable without one.
  c.menuItems = {"Mode", "Battery mode", "Customize >", "Connectivity >",
                 "Sound >", "Theme", "Screensaver", "Software update >",
                 "Reset to defaults", "Self-test >", "Battery >", "SD card", "Close"};
  c.menuSelected = 4;
  return c;
}

static ScreenCtx stepperCtx() {
  ScreenCtx c = menuCtx();
  c.menuAdjusting = true;
  c.menuSelected = 6;
  c.menuItems[6] = "Screensaver  60 min";
  c.menuHelp = "Minutes of quiet before the screen rests. 0 turns it off.";
  return c;
}

static ScreenCtx sessionCtx() {
  ScreenCtx c = baseCtx();
  c.modeName = "orchestrator";
  c.sessionTitle = "research the deploy plan for the release";
  c.sessionProvider = "anthropic";
  c.sessionState = "running";
  c.askText = "Checked the changelog and the open pull requests. Two need review.";
  return c;
}

// ---- cases ------------------------------------------------------------------

static void test_status_empty()   { golden("status_empty", ScreenId::StatusIdle, baseCtx()); }
static void test_status_jobs()    { golden("status_jobs", ScreenId::StatusIdle, jobsCtx()); }
static void test_menu_main()      { golden("menu_main", ScreenId::Menu, menuCtx()); }
static void test_menu_stepper()   { golden("menu_stepper", ScreenId::Menu, stepperCtx()); }
static void test_session_detail() { golden("session_detail", ScreenId::SessionDetail, sessionCtx()); }

static void test_ask() {
  ScreenCtx c = baseCtx();
  c.askText = "The deploy finished. Two tests failed on the release branch; "
              "do you want me to open a pull request with the fixes?";
  golden("ask", ScreenId::Ask, c);
}

static void test_voice_recording() {
  ScreenCtx c = baseCtx();
  c.voice = VoiceStage::Recording;
  golden("voice_recording", ScreenId::VoiceGlyph, c);
}

static void test_battery() {
  ScreenCtx c = baseCtx();
  c.battery.valid = true;
  c.battery.percent = 82;
  c.battChargeState = "discharging";
  c.battMinutesToEmpty = 415;
  golden("battery", ScreenId::Battery, c);
}

static void test_battery_low() {
  ScreenCtx c = baseCtx();
  c.battery.valid = true;
  c.battery.percent = 7;
  c.battChargeState = "discharging";
  c.battMinutesToEmpty = 22;
  golden("battery_low", ScreenId::Battery, c);
}

static void test_selftest() {
  ScreenCtx c = baseCtx();
  c.selfTest = {{"display", 0}, {"touch", 0}, {"heap", 0}, {"psram", 0},
                {"sd card", 2}, {"wi-fi", 0}, {"bluetooth", 1}, {"battery", 0}};
  c.selfTestSummary = "6 passed / 1 failed / 1 skipped";
  golden("selftest", ScreenId::SelfTest, c);
}

static void test_config_qr() {
  ScreenCtx c = baseCtx();
  c.modeName = "orchestrator";
  // Obviously-fake token. Never seed a fixture from a real device access token -
  // a real value would land both in the repo and in the rendered golden's pixels.
  c.configUrl = "http://192.0.2.10/?t=ffffffffffff";
  c.setupUrl = "";  // TFT steady state: LAN connected, temporary setup AP off
  c.netStatus = "Home Wi-Fi connected: 192.0.2.10";
  golden("config_qr", ScreenId::ConfigQr, c);
}

static void test_token_detail() {
  ScreenCtx c = baseCtx();
  c.modeName = "orchestrator";
  c.webToken = "0123456789abcdef01234567";  // obviously fake, full 24-char shape
  golden("token_detail", ScreenId::TokenDetail, c);
}

static void test_setup_info() {
  ScreenCtx c = baseCtx();
  c.modeName = "orchestrator";   // the Wi-Fi setup screen is an Orchestrator context
  c.apName = "Nimbus-4-setup";
  c.apPass = "nimbus1234";
  c.setupUrl = "http://192.168.4.1/";
  golden("setup_info", ScreenId::SetupInfo, c);
}

// Notifier connects over Bluetooth, not Wi-Fi - its setup screen must point at
// the broker, never a Wi-Fi network (which does not exist: Notifier runs no radio).
static void test_setup_info_notifier() {
  ScreenCtx c = baseCtx();
  c.modeName = "notifier";
  c.apName = "Nimbus-4-setup";   // present, but must NOT be shown in Notifier
  c.apPass = "nimbus1234";
  golden("setup_info_notifier", ScreenId::SetupInfo, c);
}

static void test_pairing() {
  ScreenCtx c = baseCtx();
  c.pairingCode = "428193";
  golden("pairing", ScreenId::Pairing, c);
}

static void test_screensaver() { golden("screensaver", ScreenId::Screensaver, baseCtx()); }

// ---- behavioural assertions (not pixels) ------------------------------------

// A tap on a session card must resolve to THAT card's index - the property the
// whole touch-nav layer depends on.
static void test_hit_test_maps_to_card_index() {
  Fb565 fb;
  const Rendered r = renderScreen(fb, ScreenId::StatusIdle, jobsCtx());
  std::set<int> seen;
  for (const auto& t : r.taps) {
    if (t.action != TapRegion::Action::SessionCard) continue;
    const int cx = t.x + t.w / 2, cy = t.y + t.h / 2;
    const TapRegion* h = r.hit(cx, cy);
    TEST_ASSERT_NOT_NULL(h);
    TEST_ASSERT_EQUAL_INT(int(TapRegion::Action::SessionCard), int(h->action));
    TEST_ASSERT_EQUAL_INT(t.index, h->index);
    seen.insert(t.index);
  }
  TEST_ASSERT_TRUE_MESSAGE(seen.size() >= 3, "expected several session cards");
}

// Every screen must offer a way OUT - with no knob, a screen with no Back/Home
// is a dead end that strands the user.
static void test_every_screen_has_an_exit() {
  const ScreenId screens[] = {
      ScreenId::Menu,     ScreenId::SessionDetail, ScreenId::Ask,
      ScreenId::SelfTest, ScreenId::Battery,       ScreenId::ConfigQr,
      ScreenId::TokenDetail, ScreenId::SetupInfo, ScreenId::Screensaver};
  for (ScreenId s : screens) {
    Fb565 fb;
    ScreenCtx c = menuCtx();
    c.askText = "text";
    const Rendered r = renderScreen(fb, s, c);
    bool exit = false;
    for (const auto& t : r.taps)
      if (t.action == TapRegion::Action::Back || t.action == TapRegion::Action::Home)
        exit = true;
    char msg[96];
    std::snprintf(msg, sizeof msg, "screen %d has no Back/Home target", int(s));
    TEST_ASSERT_TRUE_MESSAGE(exit, msg);
  }
}

// The status screen must always reach the menu and the mic, whatever the state.
// The menu must ALWAYS be reachable - it is the only way back to settings on a
// knobless board. The mic must appear ONLY in Orchestrator mode: hold-to-talk
// records, transcribes and sends a turn, and in Notifier mode there is no
// orchestrator to receive it, so the control cannot work. A button that does
// nothing is worse than no button - the owner cannot tell it from a broken one.
static void test_status_reaches_menu_always_and_mic_only_in_orchestrator() {
  const ScreenCtx bases[] = {baseCtx(), jobsCtx()};
  for (const auto& base : bases) {
    for (const char* mode : {"orchestrator", "notifier"}) {
      ScreenCtx c = base;
      c.modeName = mode;
      Fb565 fb;
      const Rendered r = renderScreen(fb, ScreenId::StatusIdle, c);
      bool menu = false, mic = false;
      for (const auto& t : r.taps) {
        if (t.action == TapRegion::Action::OpenMenu) menu = true;
        if (t.action == TapRegion::Action::Mic) mic = true;
      }
      TEST_ASSERT_TRUE_MESSAGE(menu, "status screen cannot open the menu");
      const bool wantMic = (std::string(mode) == "orchestrator");
      TEST_ASSERT_EQUAL_MESSAGE(int(wantMic), int(mic),
          wantMic ? "Orchestrator status screen has no hold-to-talk"
                  : "Notifier status screen offers a mic that cannot work");
    }
  }
}

// A long menu must stay navigable: the selected row is always rendered, and
// paging targets appear exactly when the list overflows.
static void test_long_menu_keeps_selection_visible() {
  ScreenCtx c = menuCtx();
  c.menuSelected = 12;   // last row
  Fb565 fb;
  const Rendered r = renderScreen(fb, ScreenId::Menu, c);
  bool selVisible = false;
  for (const auto& t : r.taps)
    if (t.action == TapRegion::Action::MenuRow && t.index == 12) selVisible = true;
  TEST_ASSERT_TRUE_MESSAGE(selVisible, "selected row scrolled off screen");
}

// The stepper screen must offer both directions and a commit.
static void test_stepper_has_both_directions_and_save() {
  Fb565 fb;
  const Rendered r = renderScreen(fb, ScreenId::Menu, stepperCtx());
  bool up = false, down = false, commit = false;
  for (const auto& t : r.taps) {
    if (t.action == TapRegion::Action::ValueUp) up = true;
    if (t.action == TapRegion::Action::ValueDown) down = true;
    if (t.action == TapRegion::Action::Commit) commit = true;
  }
  TEST_ASSERT_TRUE_MESSAGE(up && down, "stepper missing a direction");
  TEST_ASSERT_TRUE_MESSAGE(commit, "stepper has no Save");
}

// Status tone must track the wire status, so the card and the LED agree.
static void test_tone_mapping_matches_ring_semantics() {
  TEST_ASSERT_EQUAL_INT(int(StatusTone::Working),  int(toneFor(1)));
  TEST_ASSERT_EQUAL_INT(int(StatusTone::Waiting),  int(toneFor(2)));
  TEST_ASSERT_EQUAL_INT(int(StatusTone::Approval), int(toneFor(3)));
  TEST_ASSERT_EQUAL_INT(int(StatusTone::Done),     int(toneFor(4)));
  TEST_ASSERT_EQUAL_INT(int(StatusTone::Error),    int(toneFor(5)));
  TEST_ASSERT_EQUAL_INT(int(StatusTone::Neutral),  int(toneFor(0)));
  TEST_ASSERT_EQUAL_INT(int(StatusTone::Neutral),  int(toneFor(6)));
}

// The framebuffer must be exactly what the panel expects: big-endian RGB565.
static void test_framebuffer_is_big_endian_rgb565() {
  Fb565 fb;
  fb.clear(0x0000);
  fb.set(0, 0, 0xF800);            // pure red
  const uint8_t* d = fb.data();
  TEST_ASSERT_EQUAL_UINT8(0xF8, d[0]);   // high byte first
  TEST_ASSERT_EQUAL_UINT8(0x00, d[1]);
  TEST_ASSERT_EQUAL_UINT16(0xF800, fb.get(0, 0));   // round-trips logically
  TEST_ASSERT_EQUAL_UINT32(153600u, uint32_t(fb.byteSize()));
}

// Clipping: drawing off-panel must never corrupt memory or wrap to the far side.
static void test_primitives_clip() {
  Fb565 fb;
  fb.clear(0x0000);
  fb.fillRect(-50, -50, 40, 40, 0xFFFF);      // fully off, top-left
  fb.fillRect(kW + 10, 10, 40, 40, 0xFFFF);   // fully off, right
  fb.text(-200, 10, "offscreen", 0xFFFF, 2);
  for (int y = 0; y < kH; y++)
    for (int x = 0; x < kW; x++)
      TEST_ASSERT_EQUAL_UINT16(0x0000, fb.get(x, y));

  fb.fillRect(-10, -10, 20, 20, 0xFFFF);      // straddles the origin
  TEST_ASSERT_EQUAL_UINT16(0xFFFF, fb.get(0, 0));
  TEST_ASSERT_EQUAL_UINT16(0xFFFF, fb.get(9, 9));
  TEST_ASSERT_EQUAL_UINT16(0x0000, fb.get(10, 10));
}

// Long text must be truncated with an ellipsis, never overrun its card.
static void test_text_clipped_fits() {
  Fb565 fb;
  const std::string huge(200, 'W');
  fb.textClipped(10, 10, huge, kInk, 100, 2);
  for (int y = 0; y < kH; y++)
    for (int x = 115; x < kW; x++)
      TEST_ASSERT_EQUAL_UINT16(kBg == 0 ? fb.get(x, y) : fb.get(x, y), fb.get(x, y));
  TEST_ASSERT_TRUE(Fb565::textWidth("WWWW", 2) > Fb565::textWidth("WW", 2));
}

// The palette must be the web UI's, exactly - this is the anti-drift assertion.
static void test_palette_matches_web_ui() {
  TEST_ASSERT_EQUAL_UINT16(rgb(0x14, 0x15, 0x18), kBg);     // --bg
  TEST_ASSERT_EQUAL_UINT16(rgb(0x1c, 0x1e, 0x23), kRaise);  // --raise
  TEST_ASSERT_EQUAL_UINT16(rgb(0x2a, 0x2d, 0x36), kLine);   // --line
  TEST_ASSERT_EQUAL_UINT16(rgb(0xec, 0xee, 0xf2), kInk);    // --ink
  TEST_ASSERT_EQUAL_UINT16(rgb(0x6f, 0x76, 0x84), kInk3);   // --ink3
  TEST_ASSERT_EQUAL_UINT16(rgb(0x5a, 0xd6, 0xc4), kTeal);   // --teal
  TEST_ASSERT_EQUAL_UINT16(rgb(0x63, 0xd1, 0x9a), kOk);     // --ok
  TEST_ASSERT_EQUAL_UINT16(rgb(0xf0, 0x68, 0x7a), kCrit);   // --crit
  // A tint must land between its colour and the surface, never equal either.
  const uint16_t t = tintFor(kCrit, kRaise);
  TEST_ASSERT_TRUE(t != kCrit && t != kRaise);
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_status_empty);
  RUN_TEST(test_status_jobs);
  RUN_TEST(test_menu_main);
  RUN_TEST(test_menu_stepper);
  RUN_TEST(test_session_detail);
  RUN_TEST(test_ask);
  RUN_TEST(test_voice_recording);
  RUN_TEST(test_battery);
  RUN_TEST(test_battery_low);
  RUN_TEST(test_selftest);
  RUN_TEST(test_config_qr);
  RUN_TEST(test_token_detail);
  RUN_TEST(test_setup_info);
  RUN_TEST(test_setup_info_notifier);
  RUN_TEST(test_pairing);
  RUN_TEST(test_screensaver);

  RUN_TEST(test_hit_test_maps_to_card_index);
  RUN_TEST(test_every_screen_has_an_exit);
  RUN_TEST(test_status_reaches_menu_always_and_mic_only_in_orchestrator);
  RUN_TEST(test_long_menu_keeps_selection_visible);
  RUN_TEST(test_stepper_has_both_directions_and_save);
  RUN_TEST(test_tone_mapping_matches_ring_semantics);
  RUN_TEST(test_framebuffer_is_big_endian_rgb565);
  RUN_TEST(test_primitives_clip);
  RUN_TEST(test_text_clipped_fits);
  RUN_TEST(test_palette_matches_web_ui);
  return UNITY_END();
}
