// test_fold_slice_memory - the fold's slice holds its rows in PSRAM, not in internal SRAM.
//
// Why this exists (v5.1.2 review, lane CTXR1): the fold's forward read pushed every row of
// its slice into a vector with no PsramFirstScope and no spill. The fold runs from the pump,
// outside a turn, where the device keeps every block of 128 B or less in internal SRAM, so a
// slice of short messages (a 400-row backlog) put 1,400 to 2,100 small blocks, 76 to 94 KB,
// in internal SRAM for the length of its card reads: the S2 L24 condition (turn_memory.h),
// where a Wi-Fi internal allocation fails and IDF aborts. v5.1.1's read held about 3 KB.
//
// The allocation seam: every block of 128 B or less allocated while no PsramFirstScope is
// open is one the device would place in internal SRAM. The test counts the live ones through
// whole slices (manual and automatic, short and mixed rows) and fails when the peak grows
// with the backlog.
#include <unity.h>

#include <cstdio>
#include <cstdlib>
#include <map>
#include <memory>
#include <new>
#include <string>
#include <vector>

#include "../support/fake_config.h"
#include "../support/fake_platform.h"
#include "nimbus/harness/engine.h"
#include "nimbus/harness/turn_memory.h"
#include "nimbus/orch/episodic.h"

namespace {
constexpr size_t kInternalLimit = 128;   // nimbus::heapwatch::kSmallAllocInternalLimit
size_t g_liveBlocks = 0, g_peakBlocks = 0, g_live = 0, g_peak = 0;
bool g_on = false;

struct Head {
  size_t n;
  size_t internal;   // counted: small and allocated outside every PsramFirstScope
};

void* counted(size_t n) {
  void* raw = std::malloc(n + sizeof(Head));
  if (!raw) return nullptr;
  Head* h = static_cast<Head*>(raw);
  h->n = n;
  h->internal = g_on && n <= kInternalLimit && agent::PsramFirstScope::depth() == 0;
  if (h->internal) {
    g_live += n;
    if (g_live > g_peak) g_peak = g_live;
    if (++g_liveBlocks > g_peakBlocks) g_peakBlocks = g_liveBlocks;
  }
  return h + 1;
}
void uncounted(void* p) {
  if (!p) return;
  Head* h = static_cast<Head*>(p) - 1;
  if (h->internal) {
    g_live -= h->n;
    g_liveBlocks--;
  }
  std::free(h);
}
}  // namespace

void* operator new(size_t n) {
  void* p = counted(n);
  if (!p) throw std::bad_alloc();
  return p;
}
void* operator new[](size_t n) {
  void* p = counted(n);
  if (!p) throw std::bad_alloc();
  return p;
}
void operator delete(void* p) noexcept { uncounted(p); }
void operator delete[](void* p) noexcept { uncounted(p); }
void operator delete(void* p, size_t) noexcept { uncounted(p); }
void operator delete[](void* p, size_t) noexcept { uncounted(p); }

void setUp() {}
void tearDown() {}

namespace orch = nimbus::orch;

namespace {
struct Rig {
  harness_test::FakePlatform plat;
  harness_test::FakeConfig cfg;
  std::vector<orch::EpisodicMessage> rows;
  agent::HistoryMarks marks;
  std::unique_ptr<agent::TurnEngine> eng;

  Rig() {
    cfg.orchModels["anthropic"] = "claude-sonnet-5-5";
    agent::TurnEngine::Deps d;
    d.cfg = cfg.contract();
    d.platform = plat.contract();
    d.histV2 = [] { return true; };
    d.captureUserRow = [](const std::string&, const std::string&, const std::string&) { return std::string(); };
    d.historyRows = [this](const std::string&, const agent::HistoryQuery& q) {
      std::vector<orch::EpisodicMessage> out;
      if (!q.after.empty()) {
        for (const auto& m : rows)
          if (agent::rowIdAfter(m.id, q.after) && out.size() < q.limit) out.push_back(m);
        return out;
      }
      for (auto it = rows.rbegin(); it != rows.rend() && out.size() < q.limit; ++it)
        if (q.before.empty() || agent::rowIdAfter(q.before, it->id)) out.push_back(*it);
      return out;
    };
    d.historyMarks = [this](const std::string&) { return marks; };
    eng.reset(new agent::TurnEngine(std::move(d)));
  }
  // `n` exchanges of short (or mixed) rows, each with the tags a device row carries.
  void addRows(int n, size_t userBytes, size_t replyBytes) {
    for (int i = 0; i < 2 * n; i++) {
      orch::EpisodicMessage m;
      char id[16];
      std::snprintf(id, sizeof(id), "m%08x", (unsigned)rows.size() + 1);
      m.id = id;
      m.sessionId = "1001";
      m.role = i % 2 ? "assistant" : "user";
      m.kind = orch::MsgKind::Message;
      m.text = std::to_string(i) + " " + std::string(i % 2 ? replyBytes : userBytes, i % 2 ? 'a' : 'u');
      m.tags = i % 2 ? "fx:none,out:reply,turn:m00000001,at:1794787200" : "from:Roy,src:tg:12345678,at:1794787200";
      m.tsHours = 498552 + (uint32_t)i / 20;
      rows.push_back(m);
    }
  }
};

// The peak of small blocks outside any PsramFirstScope while one slice runs.
size_t slicePeak(Rig& r, bool manual) {
  g_peakBlocks = g_liveBlocks;
  g_peak = g_live;
  const size_t base = g_liveBlocks;
  g_on = true;
  const agent::FoldSlice s = r.eng->historyFoldSlice("1001", 53900, manual);
  g_on = false;
  TEST_ASSERT_FALSE_MESSAGE(s.digest.empty(), "the slice took rows");
  return g_peakBlocks - base;
}
}  // namespace

// A chat of short messages (60/90 B rows), the 400-row backlog the review measured, and a
// mixed chat: the small blocks the slice leaves in internal SRAM stay a small constant (the
// slice's own bookkeeping), whatever the rows.
static void test_the_fold_slice_keeps_its_rows_out_of_internal_sram() {
  const struct {
    int exchanges;
    size_t user, reply;
  } shapes[] = {{200, 60, 90}, {400, 60, 90}, {150, 80, 600}};
  for (const auto& sh : shapes) {
    for (int manual = 1; manual >= 0; manual--) {
      Rig r;
      r.addRows(sh.exchanges, sh.user, sh.reply);
      r.marks.thru = "m00000001";
      r.marks.softStart = r.rows[r.rows.size() / 2].id;   // a shift hid half of it
      const size_t peak = slicePeak(r, manual == 1);
      std::printf("%d exchanges of %u/%u B, %s: %u small blocks outside PSRAM at the peak (%u B)\n", sh.exchanges,
                  (unsigned)sh.user, (unsigned)sh.reply, manual ? "manual" : "automatic", (unsigned)peak,
                  (unsigned)g_peak);
      TEST_ASSERT_TRUE_MESSAGE(peak <= 16, "the slice's rows ride PSRAM");
    }
  }
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_the_fold_slice_keeps_its_rows_out_of_internal_sram);
  return UNITY_END();
}
