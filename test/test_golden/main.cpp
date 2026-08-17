#include <unity.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "nimbus/epd_render/fb.h"
#include "nimbus/epd_render/screens.h"
#include "nimbus/settings_menu.h"

// Golden-image matrix: render representative ScreenCtx fixtures through
// renderScreen() and byte-compare the 1-bit framebuffer against blessed
// buffers in test/golden/<case>.bin.
//
//   - GOLDEN_UPDATE=1 (exactly "1"): the rendered buffer is written as the new
//     golden and the test passes with a message (bootstrap the matrix on first
//     run, re-bless after intentional layout changes). Any other value - unset,
//     "0", "" - is compare mode.
//   - In compare mode a MISSING golden is a FAILURE, not a silent bless: an
//     uncommitted or deleted golden must never let a regression pass green.
//   - On mismatch the rendered buffer is also dumped to
//     test/golden/out/<case>.bin so `tools/golden.py diff` can show red pixels:
//       python3 tools/golden.py diff test/golden/X.bin test/golden/out/X.bin d.png
//
// pio test runs from the project root, so the golden paths are relative.

using namespace nimbus::epd;
using nimbus::Posture;
using nimbus::attn::ScreenId;
using nimbus::attn::VoiceStage;
using solide::ring::Status;

void setUp() {}
void tearDown() {}

static const char* kGoldenDir = "test/golden";
static const char* kOutDir = "test/golden/out";

static void writeFile(const std::string& path, const uint8_t* buf, size_t n) {
  FILE* f = std::fopen(path.c_str(), "wb");
  TEST_ASSERT_NOT_NULL_MESSAGE(f, path.c_str());
  const size_t wrote = std::fwrite(buf, 1, n, f);
  std::fclose(f);
  TEST_ASSERT_TRUE_MESSAGE(wrote == n, path.c_str());
}

static void checkGolden(const char* name, const Fb& fb) {
  const std::string golden = std::string(kGoldenDir) + "/" + name + ".bin";
  const char* env = std::getenv("GOLDEN_UPDATE");
  const bool update = env != nullptr && std::strcmp(env, "1") == 0;

  if (update) {  // explicit re-bless
    (void)std::system((std::string("mkdir -p ") + kGoldenDir).c_str());
    writeFile(golden, fb.data(), size_t(kFbBytes));
    TEST_MESSAGE((std::string("updated golden: ") + golden).c_str());
    return;
  }

  FILE* f = std::fopen(golden.c_str(), "rb");
  // Compare mode: a missing golden is a failure, never a silent bless.
  TEST_ASSERT_NOT_NULL_MESSAGE(
      f, (std::string("missing golden ") + golden +
          " - run `GOLDEN_UPDATE=1 pio test -e native -f test_golden` to bless")
             .c_str());
  static uint8_t expect[kFbBytes];
  const size_t got = std::fread(expect, 1, kFbBytes, f);
  std::fclose(f);
  TEST_ASSERT_TRUE_MESSAGE(got == size_t(kFbBytes), "golden file truncated");

  if (std::memcmp(expect, fb.data(), size_t(kFbBytes)) != 0) {
    // Dump the offending render for tools/golden.py diff before failing.
    (void)std::system((std::string("mkdir -p ") + kOutDir).c_str());
    writeFile(std::string(kOutDir) + "/" + name + ".bin", fb.data(),
              size_t(kFbBytes));
  }
  TEST_ASSERT_EQUAL_MEMORY_MESSAGE(expect, fb.data(), kFbBytes, name);
}

static void golden(const char* name, ScreenId id, const ScreenCtx& ctx) {
  static Fb fb;
  renderScreen(fb, id, ctx);
  checkGolden(name, fb);
}

// ---- fixtures ---------------------------------------------------------------

static ScreenCtx baseCtx() {
  return ScreenCtx{};  // notifier / balanced / passive, no battery telemetry
}

static ScreenCtx threeJobsCtx() {
  ScreenCtx c = baseCtx();
  c.jobs.push_back({101, uint8_t(Status::Running), 40, 170, "build firmware image"});
  c.jobs.push_back({102, uint8_t(Status::WaitingInput), 0, 213, "review deploy plan"});
  c.jobs.push_back({103, uint8_t(Status::Done), 100, 85, ""});
  return c;
}

// 30 words x 7 chars: exactly 6 words per 48-col line -> 5 lines -> 3 pages.
static ScreenCtx jobDetailCtx() {
  ScreenCtx c = baseCtx();
  std::string label;
  for (int i = 1; i <= 30; ++i) {
    char w[12];
    std::snprintf(w, sizeof w, "seg%04d", i);
    if (i > 1) label += ' ';
    label += w;
  }
  c.jobs.push_back({42, uint8_t(Status::Running), 37, 170, label});
  c.cursorJob = 0;
  return c;
}

static ScreenCtx sessionDetailCtx() {
  ScreenCtx c = baseCtx();
  c.modeName = "orchestrator";
  c.sessionTitle = "research the deploy plan for the new release";
  c.sessionProvider = "anthropic";
  c.sessionState = "running";
  return c;
}

// The Orchestrator root - focus index 0, always present ("you <-> Nimbus" home).
static ScreenCtx sessionDetailRootCtx() {
  ScreenCtx c = baseCtx();
  c.modeName = "orchestrator";
  c.sessionIsRoot = true;
  c.sessionTitle = "Nimbus";
  c.sessionProvider = "mistral";
  c.sessionState = "ready";
  return c;
}

static ScreenCtx battery15Ctx(Posture p) {
  ScreenCtx c = baseCtx();
  c.posture = p;
  c.profileName = "battery_saver";
  c.battery = {true, 15, 3435, false, false};
  c.jobs.push_back({7, uint8_t(Status::Running), 60, 30, "long-haul job"});
  return c;
}

// ---- cases ------------------------------------------------------------------

static void test_status_idle_empty() {
  golden("status_idle_empty", ScreenId::StatusIdle, baseCtx());
}

static void test_status_idle_three_jobs() {
  golden("status_idle_three_jobs", ScreenId::StatusIdle, threeJobsCtx());
}

static void test_status_idle_passive_batt15() {
  golden("status_idle_passive_batt15", ScreenId::StatusIdle,
         battery15Ctx(Posture::Dark));
}

static void test_status_idle_active_batt15() {
  golden("status_idle_active_batt15", ScreenId::StatusIdle,
         battery15Ctx(Posture::Full));
}

static void test_job_detail_page0() {
  ScreenCtx c = jobDetailCtx();
  c.detailPage = 0;
  golden("job_detail_page0", ScreenId::JobDetail, c);
}

static void test_job_detail_page2() {
  ScreenCtx c = jobDetailCtx();
  c.detailPage = 2;
  golden("job_detail_page2", ScreenId::JobDetail, c);
}

static void test_badge_input() {
  ScreenCtx c = threeJobsCtx();
  c.badgeActive = true;
  c.badgeText = "INPUT?";
  golden("badge_input", ScreenId::StatusIdle, c);
}

static void test_ask_two_lines() {
  ScreenCtx c = baseCtx();
  c.modeName = "orchestrator";
  c.askText =
      "Deploy the new firmware to the office device now, "
      "or wait for tonight's window?";
  golden("ask_two_lines", ScreenId::Ask, c);
}

static void test_battery_73_charging() {
  ScreenCtx c = baseCtx();
  c.battery = {true, 73, 3957, true, true};
  c.battChargeState = "charging";   // drawBattery v2: charge-state label + health line
  c.battHealthPct = 92;
  golden("battery_73_charging", ScreenId::Battery, c);
}

// drawBattery v2 discharging view: a time-to-empty projection + health, driven by
// the BatteryModel estimate (the owner-requested "time left / health" detail).
static void test_battery_discharging() {
  ScreenCtx c = baseCtx();
  c.battery = {true, 61, 3720, false, false};   // on battery, draining
  c.battChargeState = "discharging";
  c.battMinutesToEmpty = 143;                    // ~2h 23m left
  c.battHealthPct = 88;
  golden("battery_discharging", ScreenId::Battery, c);
}

static void test_battery_invalid() {
  golden("battery_invalid", ScreenId::Battery, baseCtx());
}

static void test_selftest_mixed() {
  ScreenCtx c = baseCtx();
  // A realistic mix: one FAIL (boxed), some SKIPs (no SD / no battery), rest ok.
  c.selfTest = {
      {"heap", 0}, {"psram", 0}, {"sd", 2}, {"screen", 0}, {"led", 0},
      {"input", 0}, {"memory", 0}, {"battery", 0}, {"wifi", 0}, {"ble", 1},
      {"sfx", 0}, {"audio", 2},
  };
  c.selfTestSummary = "9P/1F/2S";
  golden("selftest_mixed", ScreenId::SelfTest, c);
}

static void test_menu_five_items_sel3() {
  ScreenCtx c = baseCtx();
  c.menuTitle = "Settings";  // breadcrumb band above the list
  c.menuItems = {"posture", "profile", "brightness", "voice ask", "setup"};
  c.menuSelected = 3;
  golden("menu_five_items_sel3", ScreenId::Menu, c);
}

static void test_menu_scroll_keeps_selection_visible() {
  ScreenCtx c = baseCtx();
  c.menuTitle = "Settings";
  for (int i = 1; i <= 15; ++i) {
    char item[16];
    std::snprintf(item, sizeof item, "item %02d", i);
    c.menuItems.push_back(item);
  }
  c.menuSelected = 14;  // forces the 11-row window to scroll to rows 4..14
  golden("menu_scroll_15_items_sel14", ScreenId::Menu, c);
}

// TuneList exactly as the FSM renders it - the rows/title/help come from a
// REAL SettingsMenu (looping paramLabel()/valueLabel() via view()), so this
// fixture can never drift from the menu again (it once pinned a stale
// "Ring style: calm" row list).
static void test_menu_tune_help() {
  ScreenCtx c = baseCtx();
  nimbus::Config cfg;
  nimbus::SettingsMenu m(cfg);
  m.open();
  while (m.view().selected != 2) m.onRotate(+1);  // Customize row
  m.onClick();                                    // -> TuneList
  m.onRotate(+1);                                 // param 1 (Brightness)
  const auto v = m.view();
  c.menuTitle = v.title;
  c.menuItems = v.items;
  c.menuSelected = v.selected;
  c.menuHelp = m.helpText();
  golden("menu_tune_help", ScreenId::Menu, c);
}

// Edit screen: deepest breadcrumb path + the edited param's help pane -
// also rendered from a real SettingsMenu (override set so the reset row shows).
static void test_menu_edit_help() {
  ScreenCtx c = baseCtx();
  nimbus::Config ecfg;
  ecfg.setOverride(nimbus::Param::RingBrightness, 35);
  nimbus::SettingsMenu em(ecfg);
  em.open();
  while (em.view().selected != 2) em.onRotate(+1);  // Customize row
  em.onClick();                                     // -> TuneList
  em.onRotate(+1);                                  // param 1 (Brightness)
  em.onClick();                                     // -> Edit
  em.onClick();                                     // value row -> adjusting ("< v >")
  const auto ev = em.view();
  c.menuTitle = ev.title;
  c.menuItems = ev.items;
  c.menuSelected = ev.selected;
  c.menuHelp = nimbus::paramDescription(nimbus::Param::RingBrightness);
  golden("menu_edit_help", ScreenId::Menu, c);
}

// A synthetic >48-char title must left-elide keeping the tail (".." + last 46):
// the deepest breadcrumb level is the informative part.
static void test_menu_title_elided() {
  ScreenCtx c = baseCtx();
  c.menuTitle = "Settings > Tune > some_hypothetical_param_with_a_long_name";
  c.menuItems = {"< 1 >", "< Back"};
  c.menuSelected = 0;
  golden("menu_title_elided", ScreenId::Menu, c);
}

static ScreenCtx voiceCtx(VoiceStage st) {
  ScreenCtx c = baseCtx();
  c.modeName = "orchestrator";
  c.voice = st;
  return c;
}

static void test_voice_glyph_none() {
  golden("voice_glyph_none", ScreenId::VoiceGlyph, voiceCtx(VoiceStage::None));
}

static void test_voice_glyph_recording() {
  golden("voice_glyph_recording", ScreenId::VoiceGlyph,
         voiceCtx(VoiceStage::Recording));
}

static void test_voice_glyph_processing() {
  golden("voice_glyph_processing", ScreenId::VoiceGlyph,
         voiceCtx(VoiceStage::Processing));
}

static void test_voice_glyph_speaking() {
  golden("voice_glyph_speaking", ScreenId::VoiceGlyph,
         voiceCtx(VoiceStage::Speaking));
}

static void test_setup_info() {
  ScreenCtx c = baseCtx();
  c.modeName = "orchestrator";   // the Wi-Fi setup screen is an Orchestrator context
  c.apName = "nimbus-setup";
  c.apPass = "cumulus99";
  c.portalUrl = "http://192.168.4.1";
  c.webToken = "a1b2c3d4e5f6a7b8";   // carried by the setup URL, never typed separately
  golden("setup_info", ScreenId::SetupInfo, c);
}

// Notifier connects over Bluetooth, not Wi-Fi - its setup screen points at the
// broker and must never show a Wi-Fi network (Notifier runs no radio).
static void test_setup_info_notifier() {
  ScreenCtx c = baseCtx();
  c.modeName = "notifier";
  c.apName = "nimbus-setup";   // present, but must NOT be shown in Notifier
  c.apPass = "cumulus99";
  golden("setup_info_notifier", ScreenId::SetupInfo, c);
}

static void test_config_qr() {
  ScreenCtx c = baseCtx();
  c.modeName = "orchestrator";
  c.apName = "Nimbus-setup";
  c.apPass = "nimbus1234";
  c.webToken = "a1b2c3d4e5f6a7b8";
  // A joined device's Config QR uses its reachable LAN URL. SetupInfo owns the
  // separate AP-first onboarding flow.
  c.setupUrl = "http://192.168.4.1/?t=a1b2c3d4e5f6a7b8";
  c.configUrl = "http://192.168.1.50/?t=a1b2c3d4e5f6a7b8";
  c.netStatus = "Home Wi-Fi connected: 192.168.1.50";
  golden("config_qr", ScreenId::ConfigQr, c);
}

static void test_token_detail() {
  ScreenCtx c = baseCtx();
  c.webToken = "0123456789abcdef01234567";  // obviously fake, full 96-bit shape
  golden("token_detail", ScreenId::TokenDetail, c);
}

// The same screen WITH the live network line. ctx.netStatus was computed on every
// render and then thrown away, so this screen said nothing about the network in the
// one situation you reach it for: locked out, wondering why. The string comes from
// nimbus::wifi::netStatusLine (pre-clipped, ASCII); this golden proves it reaches
// the panel.
// NOTE: blessed only AFTER drawConfigQr was fixed to render the field. Blessing it
// first would have permanently certified the bug.
static void test_config_qr_netstatus() {
  ScreenCtx c = baseCtx();
  c.modeName = "orchestrator";
  c.apName = "Nimbus-3-setup";
  c.apPass = "nimbus1234";
  c.webToken = "a1b2c3d4e5f6a7b8";
  c.setupUrl = "http://192.168.4.1/?t=a1b2c3d4e5f6a7b8";
  c.configUrl = c.setupUrl;  // offline: current reachable interface is the setup AP
  c.netStatus = "No known Wi-Fi found - use Nimbus-3-setup";
  golden("config_qr_netstatus", ScreenId::ConfigQr, c);
}

// A failed access point reports 0.0.0.0, which used to encode into a perfectly valid
// QR resolving to nothing. deviceUrl() now yields "", so the QR block must be BLANK.
// Asserted directly rather than by byte-compare: a golden states an ABSENCE badly,
// and the absence is the whole point.
static void test_config_qr_ap_down_draws_no_qr() {
  ScreenCtx c = baseCtx();
  c.modeName = "orchestrator";
  c.apName = "Nimbus-3-setup";
  c.webToken = "a1b2c3d4e5f6a7b8";
  c.setupUrl = "";                 // what deviceUrl() returns for 0.0.0.0
  c.configUrl = "";
  c.portalUrl = "";
  c.netStatus = "Setup network is down. Restart the device.";

  auto qrAreaInk = [](const ScreenCtx& ctx) {
    Fb fb;
    renderScreen(fb, ScreenId::ConfigQr, ctx);
    int n = 0;
    for (int y = 30; y < 108; y++)
      for (int x = 160; x < nimbus::epd::kW; x++)
        if (fb.get(x, y)) n++;
    return n;
  };

  // Compared against a real QR rather than asserted at zero: the empty-URL path
  // legitimately draws a small "no url" label, so "no ink at all" would be the wrong
  // property. What must hold is that no QR MODULE GRID is rendered - an order of
  // magnitude more ink than a few words.
  ScreenCtx withQr = c;
  withQr.setupUrl = "http://192.168.4.1/?t=a1b2c3d4e5f6a7b8";
  const int inkQr = qrAreaInk(withQr);
  const int inkNone = qrAreaInk(c);
  TEST_ASSERT_TRUE_MESSAGE(inkQr > 500, "fixture no longer renders a QR - test is blind");
  TEST_ASSERT_TRUE_MESSAGE(inkNone * 4 < inkQr,
      "a QR was drawn with no URL - the code-to-nowhere bug is back");
}

// UTF-8 -> printable-ASCII transliteration (so LLM smart-quotes/dashes/emoji stop
// rendering as '?'). Bytes are the UTF-8 encodings of the code points named.
static void test_ascii_sanitize() {
  using nimbus::epd::asciiSanitize;
  TEST_ASSERT_EQUAL_STRING("don't",  asciiSanitize("don\xE2\x80\x99t").c_str());     // U+2019 '
  TEST_ASSERT_EQUAL_STRING("a-b",    asciiSanitize("a\xE2\x80\x94""b").c_str());     // U+2014 em-dash
  TEST_ASSERT_EQUAL_STRING("hi...",  asciiSanitize("hi\xE2\x80\xA6").c_str());       // U+2026 ellipsis
  TEST_ASSERT_EQUAL_STRING("\"q\"", asciiSanitize("\xE2\x80\x9Cq\xE2\x80\x9D").c_str()); // U+201C/D
  TEST_ASSERT_EQUAL_STRING("cafe",   asciiSanitize("caf\xC3\xA9").c_str());          // U+00E9 e-acute
  TEST_ASSERT_EQUAL_STRING("naive",  asciiSanitize("na\xC3\xAFve").c_str());         // U+00EF i-umlaut
  TEST_ASSERT_EQUAL_STRING("ok ",    asciiSanitize("ok \xF0\x9F\x98\x80").c_str());  // U+1F600 emoji dropped
  TEST_ASSERT_EQUAL_STRING("Hello, world!", asciiSanitize("Hello, world!").c_str()); // ASCII untouched
}

static void test_idle_art_placeholder() {
  golden("idle_art_placeholder", ScreenId::IdleArt, baseCtx());
}

static void test_screensaver_default() {
  golden("screensaver_default", ScreenId::Screensaver, baseCtx());  // "" -> "Nimbus"
}

static void test_screensaver_named() {
  ScreenCtx c = baseCtx();
  c.deviceName = "Nimbus-2";
  golden("screensaver_named", ScreenId::Screensaver, c);
}

static void test_session_detail() {
  golden("session_detail", ScreenId::SessionDetail, sessionDetailCtx());
}

static void test_mode_switch_transition() {
  ScreenCtx c = baseCtx();  // the mode-switch confirmation screen reuses Ask + askText
  c.askText = "Switching to Orchestrator mode...";
  golden("mode_switch_transition", ScreenId::Ask, c);
}

static void test_session_detail_none() {
  ScreenCtx c = baseCtx();
  c.modeName = "orchestrator";  // no sessionTitle -> "no active session" screen
  golden("session_detail_none", ScreenId::SessionDetail, c);
}

static void test_session_detail_root() {
  // The always-present Orchestrator head (focus 0) - never "no active session".
  golden("session_detail_root", ScreenId::SessionDetail, sessionDetailRootCtx());
}

static void test_pairing() {
  // BLE secure-pairing screen: the 6-digit passkey the Mac must enter. On a
  // production silent-serial unit this e-ink screen is the ONLY channel for it.
  ScreenCtx c = baseCtx();
  c.pairingCode = "042137";
  golden("pairing", ScreenId::Pairing, c);
}

static void test_header_radios() {
  // Header radio glyphs: WiFi up (wi+) + BT advertising (bt*) + battery, all in
  // the right cluster. Exercises the non-default states drawHeader renders.
  ScreenCtx c = baseCtx();
  c.wifiState = 2;                     // up      -> wi+
  c.btState   = 1;                     // advertising -> bt*
  c.battery   = {true, 73, 4010, false, true};
  golden("header_radios", ScreenId::StatusIdle, c);
}

// Regression (owner-reported "small empty box right of the text"): a job at 1-2%
// progress used to render a HOLLOW 56x7 outline - fill=(w-2)*p/100 rounds to 0 px
// below 2%. drawBar now guarantees a >=1 px fill whenever progress > 0. Direct
// pixel assertions (not a golden) so the invariant is explicit: some interior
// pixel of the bar must be set.
static bool px(const Fb& fb, int x, int y) {
  return (fb.data()[y * kStride + x / 8] >> (7 - (x & 7))) & 1;
}
static void test_progress_bar_never_hollow() {
  ScreenCtx c = baseCtx();
  c.jobs.push_back({7, uint8_t(Status::Running), 1, 170, "one percent job"});
  static Fb fb;
  renderScreen(fb, ScreenId::StatusIdle, c);
  // Bar drawn at (232, y+1=27, 56, 7) - jobs start y=26 under the 2-line header.
  TEST_ASSERT_TRUE_MESSAGE(px(fb, 232, 27), "bar outline missing");
  // ...and the INTERIOR must not be empty (the old bug): scan the fill region.
  bool interior = false;
  for (int y = 28; y <= 32 && !interior; ++y)
    for (int x = 233; x <= 286 && !interior; ++x) interior = px(fb, x, y);
  TEST_ASSERT_TRUE_MESSAGE(interior, "1% progress bar rendered as a hollow box");
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_status_idle_empty);
  RUN_TEST(test_status_idle_three_jobs);
  RUN_TEST(test_progress_bar_never_hollow);
  RUN_TEST(test_status_idle_passive_batt15);
  RUN_TEST(test_status_idle_active_batt15);
  RUN_TEST(test_job_detail_page0);
  RUN_TEST(test_job_detail_page2);
  RUN_TEST(test_badge_input);
  RUN_TEST(test_ask_two_lines);
  RUN_TEST(test_battery_73_charging);
  RUN_TEST(test_battery_discharging);
  RUN_TEST(test_battery_invalid);
  RUN_TEST(test_selftest_mixed);
  RUN_TEST(test_menu_five_items_sel3);
  RUN_TEST(test_menu_scroll_keeps_selection_visible);
  RUN_TEST(test_menu_tune_help);
  RUN_TEST(test_menu_edit_help);
  RUN_TEST(test_menu_title_elided);
  RUN_TEST(test_voice_glyph_none);
  RUN_TEST(test_voice_glyph_recording);
  RUN_TEST(test_voice_glyph_processing);
  RUN_TEST(test_voice_glyph_speaking);
  RUN_TEST(test_setup_info);
  RUN_TEST(test_setup_info_notifier);
  RUN_TEST(test_config_qr);
  RUN_TEST(test_token_detail);
  RUN_TEST(test_config_qr_netstatus);
  RUN_TEST(test_config_qr_ap_down_draws_no_qr);
  RUN_TEST(test_ascii_sanitize);
  RUN_TEST(test_idle_art_placeholder);
  RUN_TEST(test_screensaver_default);
  RUN_TEST(test_screensaver_named);
  RUN_TEST(test_session_detail);
  RUN_TEST(test_session_detail_none);
  RUN_TEST(test_session_detail_root);
  RUN_TEST(test_pairing);
  RUN_TEST(test_header_radios);
  RUN_TEST(test_mode_switch_transition);
  return UNITY_END();
}
