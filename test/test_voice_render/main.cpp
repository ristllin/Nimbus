// Golden renders for the hold-to-talk status on the home screen (CUM-456).
//
// The voice flow's line (nimbus::voice::Flow::line) replaces the idle legend while
// hold-to-talk is live: inside the on-screen ring on a board with no LED ring (the
// Freenove all-in-one the owner reported from), and in the mic bar on a ring board.
// The ring itself is drawn from the flow's cue frame (voice::cueFrame), so these
// goldens pin the ring cue AND the screen line together, at every supported panel.
//
// Same artefact contract as test_tft_render (kept a separate suite so a parallel
// change to that file cannot collide with this one):
//   test/golden_tft[/WxH]/<case>.bin            RGB565 framebuffer, byte-compared
//   test/golden_tft[/WxH]/<case>.regions.json   its tap targets
//   - GOLDEN_UPDATE=1 re-blesses; a MISSING golden is a FAILURE, never a silent bless
//   - on mismatch the render is dumped under test/golden_tft/out/

#include <unity.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "nimbus/tft_render/fb565.h"
#include "nimbus/tft_render/screens.h"
#include "nimbus/voice_flow.h"

using namespace nimbus::tft;
using nimbus::attn::ScreenId;
using nimbus::render::ScreenCtx;
namespace voice = nimbus::voice;

void setUp() {}
void tearDown() {}

static const char* kDir = "test/golden_tft";
static const char* kOutDir = "test/golden_tft/out";

struct PanelSize { const char* subdir; int w; int h; };
static const PanelSize kPanels[] = {
    {"", 320, 240}, {"480x320", 480, 320}, {"480x480", 480, 480}};

static void writeFile(const std::string& path, const uint8_t* buf, size_t n) {
  FILE* f = std::fopen(path.c_str(), "wb");
  TEST_ASSERT_NOT_NULL_MESSAGE(f, path.c_str());
  const size_t wrote = std::fwrite(buf, 1, n, f);
  std::fclose(f);
  TEST_ASSERT_TRUE_MESSAGE(wrote == n, path.c_str());
}

static const char* actionName(TapRegion::Action a) {
  switch (a) {
    case TapRegion::Action::Mic:         return "Mic";
    case TapRegion::Action::OpenMenu:    return "OpenMenu";
    case TapRegion::Action::SessionCard: return "SessionCard";
    case TapRegion::Action::Back:        return "Back";
    case TapRegion::Action::Home:        return "Home";
    default:                             return "Other";
  }
}

static void writeRegions(const std::string& path, const Rendered& r) {
  FILE* f = std::fopen(path.c_str(), "wb");
  TEST_ASSERT_NOT_NULL_MESSAGE(f, path.c_str());
  std::fprintf(f, "[\n");
  for (size_t i = 0; i < r.taps.size(); i++) {
    const auto& t = r.taps[i];
    std::fprintf(f, "  {\"x\":%d,\"y\":%d,\"w\":%d,\"h\":%d,\"action\":\"%s\",\"index\":%d}%s\n",
                 t.x, t.y, t.w, t.h, actionName(t.action), t.index,
                 i + 1 < r.taps.size() ? "," : "");
  }
  std::fprintf(f, "]\n");
  std::fclose(f);
}

// Structural: the Mic target is still there (a retry is a hold on the same spot),
// every target meets the 44px floor and stays on the panel.
static void assertMicTarget(const char* name, const Rendered& r, int w, int h) {
  int mics = 0;
  for (const auto& t : r.taps) {
    TEST_ASSERT_TRUE_MESSAGE(t.w >= kMinTap && t.h >= kMinTap, name);
    TEST_ASSERT_TRUE_MESSAGE(t.x >= 0 && t.y >= 0 && t.x + t.w <= w && t.y + t.h <= h, name);
    if (t.action == TapRegion::Action::Mic) ++mics;
  }
  TEST_ASSERT_EQUAL_MESSAGE(1, mics, name);
}

static void goldenAt(const char* name, const ScreenCtx& ctx, const PanelSize& p) {
  Fb565 fb(p.w, p.h);
  const Rendered r = renderScreen(fb, ScreenId::StatusIdle, ctx);
  assertMicTarget(name, r, p.w, p.h);
  const std::string dir = p.subdir[0] ? std::string(kDir) + "/" + p.subdir : std::string(kDir);
  const std::string base = dir + "/" + name;
  const size_t bytes = fb.byteSize();
  const char* env = std::getenv("GOLDEN_UPDATE");
  if (env != nullptr && std::strcmp(env, "1") == 0) {
    (void)std::system((std::string("mkdir -p ") + dir).c_str());
    writeFile(base + ".bin", fb.data(), bytes);
    writeRegions(base + ".regions.json", r);
    TEST_MESSAGE((std::string("updated golden: ") + base).c_str());
    return;
  }
  FILE* f = std::fopen((base + ".bin").c_str(), "rb");
  TEST_ASSERT_NOT_NULL_MESSAGE(
      f, (std::string("missing golden ") + base +
          ".bin - run `GOLDEN_UPDATE=1 pio test -e native -f test_voice_render`").c_str());
  std::vector<uint8_t> expect(bytes);
  const size_t got = std::fread(expect.data(), 1, bytes, f);
  std::fclose(f);
  TEST_ASSERT_TRUE_MESSAGE(got == bytes, "golden file truncated");
  if (std::memcmp(expect.data(), fb.data(), bytes) != 0) {
    const std::string outDir =
        p.subdir[0] ? std::string(kOutDir) + "/" + p.subdir : std::string(kOutDir);
    (void)std::system((std::string("mkdir -p ") + outDir).c_str());
    writeFile(outDir + "/" + name + ".bin", fb.data(), bytes);
  }
  TEST_ASSERT_EQUAL_MEMORY_MESSAGE(expect.data(), fb.data(), bytes, name);
}

static void golden(const char* name, const ScreenCtx& ctx) {
  for (const auto& p : kPanels) goldenAt(name, ctx, p);
}

// ---- fixtures -----------------------------------------------------------------

static const solide::ring::RGB kAccent{240, 120, 40};   // a warm theme (the owner's "orange")
static const solide::ring::RGB kAlert{240, 40, 60};

static ScreenCtx orchCtx() {
  ScreenCtx c;
  c.deviceName = "Lumi";
  c.modeName = "orchestrator";
  return c;
}

// A ringless board: the ring is on the panel, drawn from the flow's cue frame.
static ScreenCtx ringlessCtx(voice::Cue cue, uint32_t elapsedMs) {
  ScreenCtx c = orchCtx();
  solide::ring::RGB frame[45];
  voice::cueFrame(cue, elapsedMs, kAccent, kAlert, frame, 45);
  c.ringLeds.resize(45);
  for (int i = 0; i < 45; ++i) c.ringLeds[size_t(i)] = {frame[i].r, frame[i].g, frame[i].b};
  return c;
}

static void applyFlow(ScreenCtx& c, const voice::Flow& f) {
  c.voiceTitle = f.line().title;
  c.voiceDetail = f.line().detail;
  c.voiceTone = f.tone();
  c.micHeld = f.phase() == voice::Phase::Recording;
  c.micBusy = f.busy();
}

static voice::Flow thinking() {
  voice::Flow f;
  f.press(0);
  f.release(1000);
  f.transcribed("what is on my calendar this afternoon", 2500);
  return f;
}

static voice::Flow noticeFor(voice::Outcome o, const voice::SttResult& r) {
  voice::Flow f;
  f.press(0);
  f.release(1000);
  f.fail(o, voice::lineFor(o, r), 1000);
  return f;
}

// ---- cases: ringless (the ring IS the display) ---------------------------------

static void test_ring_listening() {
  voice::Flow f;
  f.press(0);
  ScreenCtx c = ringlessCtx(f.cue(), 0);
  applyFlow(c, f);
  golden("voice_ring_listening", c);
}

static void test_ring_transcribing() {
  voice::Flow f;
  f.press(0);
  f.release(1000);
  ScreenCtx c = ringlessCtx(f.cue(), 0);
  applyFlow(c, f);
  golden("voice_ring_transcribing", c);
}

static void test_ring_thinking() {
  const voice::Flow f = thinking();
  ScreenCtx c = ringlessCtx(f.cue(), 400);
  applyFlow(c, f);
  golden("voice_ring_thinking", c);
}

static void test_ring_no_network() {
  voice::SttResult r;
  r.kind = voice::SttResult::Kind::NoNetwork;
  const voice::Flow f = noticeFor(voice::Outcome::NoNetwork, r);
  ScreenCtx c = ringlessCtx(f.cue(), voice::kOfflineBreatheMs / 2);
  applyFlow(c, f);
  golden("voice_ring_no_network", c);
}

static void test_ring_http_error() {
  voice::SttResult r;
  r.kind = voice::SttResult::Kind::Http;
  r.provider = "mistral";
  r.http = 401;
  const voice::Flow f = noticeFor(voice::Outcome::SttHttp, r);
  ScreenCtx c = ringlessCtx(f.cue(), 0);
  applyFlow(c, f);
  golden("voice_ring_http_error", c);
}

static void test_ring_empty_transcript() {
  const voice::Flow f = noticeFor(voice::Outcome::EmptyTranscript, voice::SttResult{});
  // Calm outcome: the ring is handed back to the normal idle composition (dark).
  ScreenCtx c = ringlessCtx(voice::Cue::None, 0);
  applyFlow(c, f);
  golden("voice_ring_empty_transcript", c);
}

// ---- cases: ring board (the mic bar carries the line) ---------------------------

static void test_bar_thinking() {
  const voice::Flow f = thinking();
  ScreenCtx c = orchCtx();
  applyFlow(c, f);
  golden("voice_bar_thinking", c);
}

static void test_bar_no_network() {
  voice::SttResult r;
  r.kind = voice::SttResult::Kind::NoNetwork;
  const voice::Flow f = noticeFor(voice::Outcome::NoNetwork, r);
  ScreenCtx c = orchCtx();
  applyFlow(c, f);
  golden("voice_bar_no_network", c);
}

// ---- structural -------------------------------------------------------------------

// The device repaints ONLY the ring square at animation cadence; the voice line
// must live inside that square or the processing text would never refresh, and it
// must never spill onto the mic button.
static void test_ringless_status_stays_inside_the_ring_square() {
  for (const auto& p : kPanels) {
    const voice::Flow f = thinking();
    ScreenCtx withLine = ringlessCtx(f.cue(), 400);
    applyFlow(withLine, f);
    ScreenCtx without = withLine;
    without.voiceTitle.clear();
    without.voiceDetail.clear();
    Fb565 a(p.w, p.h), b(p.w, p.h);
    const Rendered ra = renderScreen(a, ScreenId::StatusIdle, withLine);
    renderScreen(b, ScreenId::StatusIdle, without);
    TEST_ASSERT_TRUE(ra.ringW > 0 && ra.ringH > 0);
    int changed = 0;
    for (int y = 0; y < p.h; ++y)
      for (int x = 0; x < p.w; ++x) {
        if (a.get(x, y) == b.get(x, y)) continue;
        ++changed;
        const bool inRing = x >= ra.ringX && x < ra.ringX + ra.ringW && y >= ra.ringY &&
                            y < ra.ringY + ra.ringH;
        TEST_ASSERT_TRUE_MESSAGE(inRing, "voice line drawn outside the ring square");
      }
    TEST_ASSERT_TRUE_MESSAGE(changed > 0, "voice line drew nothing");
  }
}

// Default-constructed voice fields leave the home render untouched (byte-identical),
// so no existing screen moved.
static void test_empty_voice_fields_change_nothing() {
  for (const auto& p : kPanels) {
    ScreenCtx base = ringlessCtx(voice::Cue::None, 0);
    ScreenCtx same = base;
    same.voiceTone = 1;   // tone alone (no title) must not draw anything
    Fb565 a(p.w, p.h), b(p.w, p.h);
    renderScreen(a, ScreenId::StatusIdle, base);
    renderScreen(b, ScreenId::StatusIdle, same);
    TEST_ASSERT_EQUAL_MEMORY(a.data(), b.data(), a.byteSize());
  }
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_ring_listening);
  RUN_TEST(test_ring_transcribing);
  RUN_TEST(test_ring_thinking);
  RUN_TEST(test_ring_no_network);
  RUN_TEST(test_ring_http_error);
  RUN_TEST(test_ring_empty_transcript);
  RUN_TEST(test_bar_thinking);
  RUN_TEST(test_bar_no_network);
  RUN_TEST(test_ringless_status_stays_inside_the_ring_square);
  RUN_TEST(test_empty_voice_fields_change_nothing);
  return UNITY_END();
}
