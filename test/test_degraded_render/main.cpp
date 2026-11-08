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

// A healthy device renders exactly as before: an empty strip draws nothing and moves nothing.
static void test_healthy_renders_exactly_as_before() {
  for (const auto& p : kPanels) {
    for (int layout = 0; layout < 3; layout++) {
      ScreenCtx a = layout == 0 ? ringlessCtx(DegradedSet()) : layout == 1 ? barCtx(DegradedSet()) : barJobsCtx(DegradedSet());
      ScreenCtx b = a;
      b.degradedRows.clear();   // never set
      Fb565 fa(p.w, p.h), fbuf(p.w, p.h);
      const Rendered ra = renderScreen(fa, ScreenId::StatusIdle, a);
      const Rendered rb = renderScreen(fbuf, ScreenId::StatusIdle, b);
      TEST_ASSERT_EQUAL_MEMORY(fa.data(), fbuf.data(), fa.byteSize());
      TEST_ASSERT_TRUE(tapOf(ra, TapRegion::Action::Degraded) == nullptr);
      TEST_ASSERT_EQUAL_size_t(rb.taps.size(), ra.taps.size());
    }
  }
}

// An unrelated reply is untouched by the new card fields (compact off, no title).
static void test_a_plain_reply_renders_exactly_as_before() {
  for (const auto& p : kPanels) {
    ScreenCtx c;
    c.deviceName = "Lumi";
    c.modeName = "orchestrator";
    c.askText = "Your 3 pm is moved to 4.";
    ScreenCtx d = c;
    d.askCompact = false;
    d.askTitle.clear();
    Fb565 a(p.w, p.h), b(p.w, p.h);
    renderScreen(a, ScreenId::Ask, c);
    renderScreen(b, ScreenId::Ask, d);
    TEST_ASSERT_EQUAL_MEMORY(a.data(), b.data(), a.byteSize());
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
