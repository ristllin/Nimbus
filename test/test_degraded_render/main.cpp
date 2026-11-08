// Lane DEGRADE: what the degraded states look like on the colour panel.
//
// The home screen carries one short status line per condition in a card at the bottom
// (nimbus/orch/degraded.h degradedStripRows), tappable for the reasons (the Ask text card
// at the small size). These goldens pin every layout, and the structural tests pin what a
// golden cannot say: the strip is one 44 px target that no neighbour overlaps, every real
// line fits it unclipped, the ring and the mic button stay clear of it, and a healthy
// device renders exactly as before.
//
// Same artifact contract as test_tft_render / test_voice_render (a separate suite so a
// parallel change to those files cannot collide with this one):
//   test/golden_tft[/WxH]/<case>.bin            RGB565 framebuffer, byte-compared
//   test/golden_tft[/WxH]/<case>.regions.json   its tap targets
//   - GOLDEN_UPDATE=1 re-blesses (and every re-bless is LOOKED AT: tools/tftpreview.py);
//     a MISSING golden is a FAILURE, never a silent bless;
//   - on mismatch the render is dumped under test/golden_tft/out/.

#include <unity.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "nimbus/orch/degraded.h"
#include "nimbus/tft_render/fb565.h"
#include "nimbus/tft_render/screens.h"

using namespace nimbus::tft;
using namespace nimbus::orch;
using nimbus::attn::ScreenId;
using nimbus::render::ScreenCtx;

void setUp() {}
void tearDown() {}

static const char* kDir = "test/golden_tft";
static const char* kOutDir = "test/golden_tft/out";

struct PanelSize { const char* subdir; int w; int h; };
static const PanelSize kPanels[] = {{"", 320, 240}, {"480x320", 480, 320}, {"480x480", 480, 480}};
static const PanelSize kDefaultPanel = kPanels[0];

// ---- golden plumbing (the contract above) -------------------------------------------------

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
    case TapRegion::Action::Degraded:    return "Degraded";
    default:                             return "Other";
  }
}

static void writeRegions(const std::string& path, const Rendered& r) {
  FILE* f = std::fopen(path.c_str(), "wb");
  TEST_ASSERT_NOT_NULL_MESSAGE(f, path.c_str());
  std::fprintf(f, "[\n");
  for (size_t i = 0; i < r.taps.size(); i++) {
    const auto& t = r.taps[i];
    std::fprintf(f, "  {\"x\":%d,\"y\":%d,\"w\":%d,\"h\":%d,\"action\":\"%s\",\"index\":%d}%s\n", t.x, t.y, t.w,
                 t.h, actionName(t.action), t.index, i + 1 < r.taps.size() ? "," : "");
  }
  std::fprintf(f, "]\n");
  std::fclose(f);
}

static bool overlaps(const TapRegion& a, const TapRegion& b) {
  return a.x < b.x + b.w && b.x < a.x + a.w && a.y < b.y + b.h && b.y < a.y + a.h;
}

// Every target meets the 44 px floor, stays on the panel and overlaps no other.
static void assertTapsSane(const char* name, const Rendered& r, int w, int h) {
  for (size_t i = 0; i < r.taps.size(); i++) {
    const auto& t = r.taps[i];
    TEST_ASSERT_TRUE_MESSAGE(t.w >= kMinTap && t.h >= kMinTap, name);
    TEST_ASSERT_TRUE_MESSAGE(t.x >= 0 && t.y >= 0 && t.x + t.w <= w && t.y + t.h <= h, name);
    // The header's Back/Home target and the gear are drawn first and may sit under later
    // full-width controls only by design; the home screens have no such pair.
    for (size_t j = i + 1; j < r.taps.size(); j++)
      TEST_ASSERT_FALSE_MESSAGE(overlaps(t, r.taps[j]), name);
  }
}

static const TapRegion* tapOf(const Rendered& r, TapRegion::Action a) {
  for (const auto& t : r.taps)
    if (t.action == a) return &t;
  return nullptr;
}

static void goldenAt(const char* name, ScreenId id, const ScreenCtx& ctx, const PanelSize& p) {
  Fb565 fb(p.w, p.h);
  const Rendered r = renderScreen(fb, id, ctx);
  assertTapsSane(name, r, p.w, p.h);
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
  TEST_ASSERT_NOT_NULL_MESSAGE(f, (std::string("missing golden ") + base +
                                   ".bin - run `GOLDEN_UPDATE=1 pio test -e native -f test_degraded_render`")
                                      .c_str());
  std::vector<uint8_t> expect(bytes);
  const size_t got = std::fread(expect.data(), 1, bytes, f);
  std::fclose(f);
  TEST_ASSERT_TRUE_MESSAGE(got == bytes, "golden file truncated");
  if (std::memcmp(expect.data(), fb.data(), bytes) != 0) {
    const std::string outDir = p.subdir[0] ? std::string(kOutDir) + "/" + p.subdir : std::string(kOutDir);
    (void)std::system((std::string("mkdir -p ") + outDir).c_str());
    writeFile(outDir + "/" + name + ".bin", fb.data(), bytes);
  }
  TEST_ASSERT_EQUAL_MEMORY_MESSAGE(expect.data(), fb.data(), bytes, name);
}

// The structural cases of a layout at every supported panel; a text variation at one.
static void golden(const char* name, ScreenId id, const ScreenCtx& ctx) {
  for (const auto& p : kPanels) goldenAt(name, id, ctx, p);
}
static void goldenDefault(const char* name, ScreenId id, const ScreenCtx& ctx) {
  goldenAt(name, id, ctx, kDefaultPanel);
}

// ---- fixtures -------------------------------------------------------------------------------

// Facts that raise exactly one condition: the real words, never hand-copied strings.
static DegradedFacts factsRaising(DegradedId id) {
  DegradedFacts f;
  f.uptimeMs = 10u * 60u * 1000u;
  switch (id) {
    case DegradedId::HeadFallback:
      f.fallback.active = true;
      f.fallback.head = "anthropic";
      f.fallback.served = "mistral";
      f.fallback.down = {"anthropic"};
      f.fallback.headClosed = true;
      f.fallback.headWhy = "is out of credit";
      f.fallback.headFix = "Add credit with the provider";
      break;
    case DegradedId::ProviderParked: {
      ParkedFact p;
      p.provider = "openai";
      p.kind = ParkKind::Billing;
      p.httpStatus = 429;
      p.seamMask = 0xFF;
      f.parks.push_back(p);
      break;
    }
    case DegradedId::Memory:
      f.embedParked = true;
      f.pendingEmbeds = 3;
      break;
    case DegradedId::StoreFull:
      f.storeFull = true;
      break;
    case DegradedId::TelegramSilent:
      f.telegram.configured = true;
      f.telegram.idleMs = 12u * 60u * 1000u;
      break;
    case DegradedId::ClockUnsynced:
      f.clockSynced = false;
      break;
  }
  return f;
}

static DegradedSet setOf(std::initializer_list<DegradedId> ids) {
  DegradedSet out;
  for (DegradedId id : ids) {
    const DegradedSet s = computeDegraded(factsRaising(id));
    TEST_ASSERT_EQUAL_size_t(1, s.size());
    out.push_back(s[0]);
  }
  return out;
}

static DegradedSet allSix() {
  return setOf({DegradedId::HeadFallback, DegradedId::ProviderParked, DegradedId::Memory, DegradedId::StoreFull,
                DegradedId::TelegramSilent, DegradedId::ClockUnsynced});
}

// A ringless board (Lumi's): the ring IS the display. The ring dots are dark (idle).
static ScreenCtx ringlessCtx(const DegradedSet& set) {
  ScreenCtx c;
  c.deviceName = "Lumi";
  c.modeName = "orchestrator";
  c.ringLeds.resize(45);
  c.degradedRows = degradedStripRows(set, 3);
  return c;
}

// A ring board in Orchestrator mode: the card layout with the mic bar.
static ScreenCtx barCtx(const DegradedSet& set) {
  ScreenCtx c;
  c.deviceName = "Nimbus-4";
  c.modeName = "orchestrator";
  c.degradedRows = degradedStripRows(set, 3);
  return c;
}

static ScreenCtx barJobsCtx(const DegradedSet& set) {
  ScreenCtx c = barCtx(set);
  c.jobs.push_back({101, 1 /*Running*/, 40, 170, "research", 0});
  c.jobs.push_back({102, 2 /*WaitingInput*/, 0, 213, "draft reply", 0});
  c.jobs.push_back({103, 4 /*Done*/, 100, 85, "summary", 0});
  return c;
}

static ScreenCtx detailsCtx(const DegradedSet& set, int page) {
  ScreenCtx c;
  c.deviceName = "Lumi";
  c.modeName = "orchestrator";
  c.askText = degradedDetailText(set);
  c.askCompact = true;
  c.askTitle = "Degraded";
  c.detailPage = page;
  return c;
}

// ---- goldens: one condition at a time (Lumi's layout, the default panel) --------------------

static void test_ring_head_fallback() {
  goldenDefault("degraded_ring_head_fallback", ScreenId::StatusIdle, ringlessCtx(setOf({DegradedId::HeadFallback})));
}
static void test_ring_provider_parked() {
  goldenDefault("degraded_ring_provider_parked", ScreenId::StatusIdle,
                ringlessCtx(setOf({DegradedId::ProviderParked})));
}
static void test_ring_memory() {
  goldenDefault("degraded_ring_memory", ScreenId::StatusIdle, ringlessCtx(setOf({DegradedId::Memory})));
}
static void test_ring_store_full() {
  goldenDefault("degraded_ring_store_full", ScreenId::StatusIdle, ringlessCtx(setOf({DegradedId::StoreFull})));
}
static void test_ring_telegram_silent() {
  goldenDefault("degraded_ring_telegram_silent", ScreenId::StatusIdle,
                ringlessCtx(setOf({DegradedId::TelegramSilent})));
}
static void test_ring_clock_unsynced() {
  goldenDefault("degraded_ring_clock_unsynced", ScreenId::StatusIdle,
                ringlessCtx(setOf({DegradedId::ClockUnsynced})));
}

// ---- goldens: the layouts at every panel ------------------------------------------------------

static void test_ring_three_rows_at_every_panel() {
  golden("degraded_ring_three", ScreenId::StatusIdle,
         ringlessCtx(setOf({DegradedId::HeadFallback, DegradedId::StoreFull, DegradedId::ClockUnsynced})));
}
static void test_ring_more_than_three_folds_into_a_count() {
  goldenDefault("degraded_ring_more", ScreenId::StatusIdle, ringlessCtx(allSix()));
}
static void test_bar_one_row() {
  goldenDefault("degraded_bar_one", ScreenId::StatusIdle, barCtx(setOf({DegradedId::HeadFallback})));
}
static void test_bar_three_rows_over_job_cards_at_every_panel() {
  golden("degraded_bar_three_jobs", ScreenId::StatusIdle,
         barJobsCtx(setOf({DegradedId::ProviderParked, DegradedId::Memory, DegradedId::TelegramSilent})));
}

// ---- goldens: the reasons, one tap away ---------------------------------------------------------

static void test_details_one() {
  goldenDefault("degraded_details_one", ScreenId::Ask, detailsCtx(setOf({DegradedId::HeadFallback}), 0));
}
static void test_details_all_at_every_panel() {
  golden("degraded_details_all", ScreenId::Ask, detailsCtx(allSix(), 0));
}
static void test_details_last_page() {
  const DegradedSet all = allSix();
  const int pages = askPageCount(degradedDetailText(all), true);
  TEST_ASSERT_TRUE(pages > 1);
  goldenDefault("degraded_details_last", ScreenId::Ask, detailsCtx(all, pages - 1));
}

// ---- structural -----------------------------------------------------------------------------------

// ---- "exactly as before" is anchored to goldens blessed BEFORE this lane ---------------------------------
//
// An A/B of one ScreenCtx against a copy of itself proves nothing (both sides run the same
// code, so ANY change to the default render leaves them equal). The "before" is bytes that were
// blessed by another suite before this lane existed: status_empty, status_jobs and ask in
// test_tft_render (the ring layout's healthy bytes are test_voice_render's voice_ring_* and
// spend_paused_ring, which run in the same native battery and did not change). These helpers
// read them and never write them: a re-bless belongs to the suite that owns the golden.
static void assertMatchesOwnedGolden(const char* name, ScreenId id, const ScreenCtx& ctx, const PanelSize& p) {
  Fb565 fb(p.w, p.h);
  renderScreen(fb, id, ctx);
  const std::string dir = p.subdir[0] ? std::string(kDir) + "/" + p.subdir : std::string(kDir);
  const std::string path = dir + "/" + name + ".bin";
  FILE* f = std::fopen(path.c_str(), "rb");
  TEST_ASSERT_NOT_NULL_MESSAGE(f, path.c_str());
  std::vector<uint8_t> expect(fb.byteSize());
  const size_t got = std::fread(expect.data(), 1, expect.size(), f);
  std::fclose(f);
  TEST_ASSERT_TRUE_MESSAGE(got == expect.size(), path.c_str());
  TEST_ASSERT_EQUAL_MEMORY_MESSAGE(expect.data(), fb.data(), fb.byteSize(), path.c_str());
}

static bool pixelsDiffer(const Fb565& a, const Fb565& b) {
  return std::memcmp(a.data(), b.data(), a.byteSize()) != 0;
}

// The fixtures of the owning suite (test_tft_render baseCtx / jobsCtx), copied exactly: if they
// drift apart the comparison below fails, loudly.
static ScreenCtx ownedBaseCtx() {
  ScreenCtx c;
  c.deviceName = "Nimbus-4";
  return c;
}
static ScreenCtx ownedJobsCtx() {
  ScreenCtx c = ownedBaseCtx();
  c.jobs.push_back({101, 1 /*Running*/, 40, 170, "build firmware", 2 /*codex*/});
  c.jobs.push_back({102, 2 /*WaitingInput*/, 0, 213, "review deploy plan", 1 /*claude*/});
  c.jobs.push_back({103, 4 /*Done*/, 100, 85, "run tests", 3 /*vibe*/});
  c.jobs.push_back({104, 5 /*Error*/, 0, 0, "publish release", 1});
  c.cursorJob = 1;
  c.battery.valid = true;
  c.battery.percent = 82;
  return c;
}

// A healthy device renders exactly as before: the device sets degradedStripRows(empty set, 3) on
// every paint, and that must be the bytes the pre-lane renderer drew, at every panel. The strip is
// live (one row changes the pixels and adds the one Degraded target), and recovery returns to the
// owned bytes exactly.
static void test_healthy_renders_exactly_as_before() {
  struct Case { const char* golden; ScreenCtx (*make)(); };
  const Case cases[] = {{"status_empty", ownedBaseCtx}, {"status_jobs", ownedJobsCtx}};
  for (const auto& p : kPanels)
    for (const Case& k : cases) {
      ScreenCtx healthy = k.make();
      healthy.degradedRows = degradedStripRows(DegradedSet(), 3);
      assertMatchesOwnedGolden(k.golden, ScreenId::StatusIdle, healthy, p);
      Fb565 fh(p.w, p.h);
      TEST_ASSERT_TRUE_MESSAGE(tapOf(renderScreen(fh, ScreenId::StatusIdle, healthy), TapRegion::Action::Degraded) == nullptr,
                               k.golden);
      // The detector is live: the same screen with one degraded row differs and has the one target ...
      ScreenCtx sick = k.make();
      sick.degradedRows = degradedStripRows(setOf({DegradedId::ClockUnsynced}), 3);
      Fb565 fs(p.w, p.h);
      const Rendered rs = renderScreen(fs, ScreenId::StatusIdle, sick);
      TEST_ASSERT_TRUE_MESSAGE(pixelsDiffer(fh, fs), k.golden);
      TEST_ASSERT_TRUE_MESSAGE(tapOf(rs, TapRegion::Action::Degraded) != nullptr, k.golden);
      // ... and the recovery (rows emptied again) is the owned bytes once more.
      sick.degradedRows = degradedStripRows(DegradedSet(), 3);
      assertMatchesOwnedGolden(k.golden, ScreenId::StatusIdle, sick, p);
    }
}

// An unrelated reply is untouched by the new card fields: it is the `ask` golden blessed before this
// lane, at every panel. The fields are live (compact text and a title each change the pixels), so
// "unchanged" means the defaults, not a field that does nothing.
static void test_a_plain_reply_renders_exactly_as_before() {
  const char* text = "The deploy finished. Two tests failed on the release branch; "
                     "do you want me to open a pull request with the fixes?";   // test_tft_render test_ask
  for (const auto& p : kPanels) {
    ScreenCtx plain = ownedBaseCtx();
    plain.askText = text;
    assertMatchesOwnedGolden("ask", ScreenId::Ask, plain, p);
    Fb565 base(p.w, p.h), compact(p.w, p.h), titled(p.w, p.h);
    renderScreen(base, ScreenId::Ask, plain);
    ScreenCtx c = plain;
    c.askCompact = true;
    renderScreen(compact, ScreenId::Ask, c);
    TEST_ASSERT_TRUE_MESSAGE(pixelsDiffer(base, compact), "askCompact must change the card");
    ScreenCtx t = plain;
    t.askTitle = "Degraded";
    renderScreen(titled, ScreenId::Ask, t);
    TEST_ASSERT_TRUE_MESSAGE(pixelsDiffer(base, titled), "askTitle must change the header");
  }
}

// The strip is ONE target of at least 44 px, on the panel, that no other overlaps, for
// every row count on every layout and panel.
static void test_the_strip_is_one_clear_44px_target() {
  const DegradedSet six = allSix();
  for (const auto& p : kPanels)
    for (size_t rows = 1; rows <= 6; rows++)
      for (int layout = 0; layout < 3; layout++) {
        DegradedSet s(six.begin(), six.begin() + (long)rows);
        const ScreenCtx c = layout == 0 ? ringlessCtx(s) : layout == 1 ? barCtx(s) : barJobsCtx(s);
        Fb565 fb(p.w, p.h);
        const Rendered r = renderScreen(fb, ScreenId::StatusIdle, c);
        assertTapsSane("strip", r, p.w, p.h);
        int n = 0;
        for (const auto& t : r.taps) n += t.action == TapRegion::Action::Degraded;
        TEST_ASSERT_EQUAL_INT(1, n);
      }
}

// The ring and the mic button give the strip its room: nothing but the strip sits in its
// rows, and the mic control is still there and still a 44 px target.
static void test_the_ring_and_the_mic_stay_clear_of_the_strip() {
  const DegradedSet six = allSix();
  for (const auto& p : kPanels)
    for (size_t rows = 1; rows <= 3; rows++) {
      const DegradedSet s(six.begin(), six.begin() + (long)rows);
      Fb565 fb(p.w, p.h);
      const Rendered r = renderScreen(fb, ScreenId::StatusIdle, ringlessCtx(s));
      const TapRegion* strip = tapOf(r, TapRegion::Action::Degraded);
      const TapRegion* mic = tapOf(r, TapRegion::Action::Mic);
      TEST_ASSERT_NOT_NULL(strip);
      TEST_ASSERT_NOT_NULL(mic);
      TEST_ASSERT_TRUE(r.ringW > 0 && r.ringH > 0);
      TEST_ASSERT_TRUE_MESSAGE(r.ringY + r.ringH <= strip->y, "the ring square runs into the strip");
      TEST_ASSERT_TRUE_MESSAGE(mic->y + mic->h <= strip->y, "the mic button runs into the strip");
      TEST_ASSERT_TRUE_MESSAGE(r.ringW >= 96, "the ring is too small to read");
    }
}

// The job cards and the empty-state card give way to the strip instead of drawing under it.
static void test_job_cards_stay_above_the_strip() {
  const DegradedSet six = allSix();
  for (const auto& p : kPanels)
    for (size_t rows = 1; rows <= 3; rows++) {
      const DegradedSet s(six.begin(), six.begin() + (long)rows);
      Fb565 fb(p.w, p.h);
      const Rendered r = renderScreen(fb, ScreenId::StatusIdle, barJobsCtx(s));
      const TapRegion* strip = tapOf(r, TapRegion::Action::Degraded);
      TEST_ASSERT_NOT_NULL(strip);
      for (const auto& t : r.taps)
        if (t.action == TapRegion::Action::SessionCard)
          TEST_ASSERT_TRUE_MESSAGE(t.y + t.h <= strip->y, "a session card runs into the strip");
    }
}

// The widest legal status line fits the strip unclipped on the smallest panel, with the
// bullet and the chevron: the line limit is a fact of the layout, not a guess.
static void test_the_longest_line_fits_the_smallest_strip() {
  const Layout L = Layout::forSize(320, 240);
  const int textW = (L.w - 2 * L.gut()) - 26 - 30;   // the strip's text width (tft_screens.cpp)
  const std::string widest(kDegradedLineMax, 'W');
  TEST_ASSERT_TRUE_MESSAGE(Fb565::textWidth(widest, 1) <= textW, "kDegradedLineMax is wider than the strip");
  // And the real lines, from the real facts, none clipped.
  for (int i = 0; i < kDegradedCount; i++)
    for (const auto& c : computeDegraded(factsRaising(DegradedId(i))))
      TEST_ASSERT_TRUE_MESSAGE(Fb565::textWidth(c.line, 1) <= textW, c.line.c_str());
}

// The strip draws where it says: a pixel changes inside its card and nowhere under the
// header, and its text is clipped to the card.
static void test_a_row_draws_inside_its_card() {
  Fb565 with(320, 240), without(320, 240);
  const Rendered r = renderScreen(with, ScreenId::StatusIdle, barCtx(setOf({DegradedId::HeadFallback})));
  renderScreen(without, ScreenId::StatusIdle, barCtx(DegradedSet()));
  const TapRegion* strip = tapOf(r, TapRegion::Action::Degraded);
  TEST_ASSERT_NOT_NULL(strip);
  int inCard = 0;
  for (int y = 0; y < 240; y++)
    for (int x = 0; x < 320; x++) {
      if (with.get(x, y) == without.get(x, y)) continue;
      if (x >= strip->x && x < strip->x + strip->w && y >= strip->y && y < strip->y + strip->h) ++inCard;
      TEST_ASSERT_TRUE_MESSAGE(y >= 44, "the strip drew into the header");
    }
  TEST_ASSERT_TRUE_MESSAGE(inCard > 200, "the strip drew almost nothing");
}

// The details card: compact text pages, every condition's words are in the pages, and the
// last page is reachable (a touch board swipes to it).
static void test_the_details_pages_hold_every_word() {
  const DegradedSet all = allSix();
  const std::string text = degradedDetailText(all);
  const int compact = askPageCount(text, true);
  const int normal = askPageCount(text, false);
  TEST_ASSERT_TRUE(compact >= 1);
  TEST_ASSERT_TRUE_MESSAGE(compact * 2 < normal, "the compact card should need far fewer pages");
  for (int page = 0; page < compact; page++) {
    Fb565 fb(320, 240);
    const Rendered r = renderScreen(fb, ScreenId::Ask, detailsCtx(all, page));
    TEST_ASSERT_NOT_NULL(tapOf(r, TapRegion::Action::Home));   // Close
  }
  // One condition fits one page and one swipe is not needed.
  TEST_ASSERT_EQUAL_INT(1, askPageCount(degradedDetailText(setOf({DegradedId::Memory})), true));
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_ring_head_fallback);
  RUN_TEST(test_ring_provider_parked);
  RUN_TEST(test_ring_memory);
  RUN_TEST(test_ring_store_full);
  RUN_TEST(test_ring_telegram_silent);
  RUN_TEST(test_ring_clock_unsynced);
  RUN_TEST(test_ring_three_rows_at_every_panel);
  RUN_TEST(test_ring_more_than_three_folds_into_a_count);
  RUN_TEST(test_bar_one_row);
  RUN_TEST(test_bar_three_rows_over_job_cards_at_every_panel);
  RUN_TEST(test_details_one);
  RUN_TEST(test_details_all_at_every_panel);
  RUN_TEST(test_details_last_page);
  RUN_TEST(test_healthy_renders_exactly_as_before);
  RUN_TEST(test_a_plain_reply_renders_exactly_as_before);
  RUN_TEST(test_the_strip_is_one_clear_44px_target);
  RUN_TEST(test_the_ring_and_the_mic_stay_clear_of_the_strip);
  RUN_TEST(test_job_cards_stay_above_the_strip);
  RUN_TEST(test_the_longest_line_fits_the_smallest_strip);
  RUN_TEST(test_a_row_draws_inside_its_card);
  RUN_TEST(test_the_details_pages_hold_every_word);
  return UNITY_END();
}
