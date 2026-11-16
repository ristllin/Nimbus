#include <unity.h>

#include <cstdio>
#include <cstring>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "nimbus/harness/conv_cache.h"
#include "nimbus/harness/conversation.h"
#include "nimbus/harness/engine.h"   // HistoryQuery
#include "nimbus/orch/episodic_log.h"
#include "nimbus/orch/row_overlay.h"
#include "../support/fake_config.h"
#include "../support/fake_platform.h"

// The conversation cache (lane H of the history fix, conv_cache.h): a chat's rows after
// its fold anchor kept between turns, so a steady-state turn reads no card row, the first
// turn after a boot carries the NEWEST exchanges (RULINGS-v3 item 4), the idle pump
// fills the rest a page at a time, and every row after thru the cache does not hold is
// named as a span instead of vanishing.

using namespace nimbus::orch;
using agent::ConvCache;
using agent::HistoryQuery;

void setUp() {}
void tearDown() {}

namespace {

struct FakeFs : EpiFs {
  std::map<std::string, std::string> files;
  mutable int readRanges = 0;
  long size(const std::string& p) const override {
    auto it = files.find(p);
    return it == files.end() ? 0 : (long)it->second.size();
  }
  long append(const std::string& p, const std::string& b) override {
    std::string& f = files[p];
    const long off = (long)f.size();
    f += b;
    return off;
  }
  std::string readAll(const std::string& p) const override {
    auto it = files.find(p);
    return it == files.end() ? std::string() : it->second;
  }
  std::string readRange(const std::string& p, long off, long len) const override {
    readRanges++;
    auto it = files.find(p);
    if (it == files.end() || off < 0 || len < 0 || (size_t)(off + len) > it->second.size()) return std::string();
    return it->second.substr((size_t)off, (size_t)len);
  }
  void forEachName(const std::string& dir, const std::function<void(const char*)>& fn) const override {
    const std::string prefix = dir + "/";
    for (auto it = files.lower_bound(prefix); it != files.end() && it->first.compare(0, prefix.size(), prefix) == 0;
         ++it)
      if (std::strchr(it->first.c_str() + prefix.size(), '/') == nullptr) fn(it->first.c_str() + prefix.size());
  }
  std::vector<std::string> list(const std::string& dir) const override {
    std::vector<std::string> out;
    forEachName(dir, [&](const char* n) { out.push_back(n); });
    return out;
  }
  bool remove(const std::string& p) override { return files.erase(p) > 0; }
  bool exists(const std::string& p) const override { return files.count(p) > 0; }
};

const uint32_t kHour0 = 490000;

// The device's shape: the SD log under the photo-record overlay, a 64-row RAM ring shared
// with trace rows (the device's is 512, with far more trace rows per turn).
struct Store {
  FakeFs fs;
  AppendLogEpisodicStore log{fs, "/mem/episodic", 64};
  RowTextOverlay overlay;
  std::vector<EpisodicMessage> mirror;   // every row added, oldest first (the truth)
  uint32_t seq = 0;
  Store() { overlay.bind(&log); }

  std::string add(const std::string& chat, const std::string& role, MsgKind kind, const std::string& text,
                  const std::string& tags = "") {
    EpisodicMessage m;
    char id[16];
    snprintf(id, sizeof(id), "m%08x", (unsigned)(++seq));
    m.id = id;
    m.sessionId = chat;
    m.tsHours = kHour0 + seq / 8;
    m.role = role;
    m.kind = kind;
    m.text = text;
    m.tags = tags;
    overlay.addMessage(m);
    mirror.push_back(m);
    return m.id;
  }
  // One exchange in `chat` with the trace rows a turn writes around it; its last row's id.
  std::string turn(const std::string& chat, int n, size_t bytes = 300) {
    const std::string u = add(chat, "user", MsgKind::Message, "question " + std::to_string(n));
    for (int t = 0; t < 4; t++) add(chat, "tool", MsgKind::ToolOutput, "trace " + std::string(200, 't'));
    return add(chat, "assistant", MsgKind::Message, "answer " + std::to_string(n) + " " + std::string(bytes, 'a'),
               "turn:" + u);   // the turn's last row: what a fold anchors on
  }
  ConvCache::ReadFn reader(const std::string& chat) {
    return [this, chat](const HistoryQuery& hq) {
      MsgQuery q;
      q.sessionId = chat;
      q.haveKind = true;
      q.kind = MsgKind::Message;
      q.alsoKinds = {MsgKind::Image, MsgKind::File, MsgKind::Audio};
      q.limit = (int)hq.limit;
      q.before = hq.before;
      q.after = hq.after;
      return overlay.query(q);
    };
  }
  // The chat's conversation rows after `thru`, oldest first, from what was written.
  std::vector<std::string> truth(const std::string& chat, const std::string& thru) {
    std::vector<std::string> out;
    for (const auto& m : mirror)
      if (m.sessionId == chat && m.kind == MsgKind::Message &&
          (thru.empty() || epiIdSuffix(m.id) > epiIdSuffix(thru)))
        out.push_back(m.id);
    return out;
  }
};

std::vector<std::string> ids(const ConvCache::View& v) {
  std::vector<std::string> out;
  for (const auto& m : v.rows) out.push_back(m.id);
  return out;
}

void assertIds(const std::vector<std::string>& want, const std::vector<std::string>& got) {
  TEST_ASSERT_EQUAL((int)want.size(), (int)got.size());
  for (size_t i = 0; i < want.size(); i++) TEST_ASSERT_EQUAL_STRING(want[i].c_str(), got[i].c_str());
}

const ConvCache::Bound kWide{1 << 22, 0};

// Accesses until the chat is filled (bounded: a cache that never completes fails the test).
void fillAll(ConvCache& cache, Store& s, const std::string& chat) {
  bool done = false;
  for (int i = 0; i < 100 && !done; i++) done = cache.access(chat, "", kWide, 1, s.reader(chat)).complete;
  TEST_ASSERT_TRUE_MESSAGE(done, "the fill completes");
}

}  // namespace

// Steady state: every access returns exactly the store's rows after thru, oldest first,
// and reads no card row (the newer rows come from the store's RAM ring).
static void test_appends_match_store_rows_with_no_card_read() {
  Store s;
  for (int i = 0; i < 30; i++) s.turn("c1", i);   // 180 rows: the index outgrows the 64-row ring
  ConvCache cache;
  fillAll(cache, s, "c1");
  int maxReads = 0;
  for (int i = 30; i < 50; i++) {
    s.turn("c1", i);
    s.turn("c2", i);   // another chat's turn in between
    s.fs.readRanges = 0;
    const uint32_t before = epiCardReads();
    const ConvCache::View v = cache.access("c1", "", kWide, 1, s.reader("c1"));
    maxReads = std::max(maxReads, s.fs.readRanges);
    TEST_ASSERT_EQUAL_UINT32(before, epiCardReads());   // the counter the context line reports agrees
    TEST_ASSERT_TRUE(v.complete);
    TEST_ASSERT_TRUE(v.span.empty());
    assertIds(s.truth("c1", ""), ids(v));
    TEST_ASSERT_EQUAL_MESSAGE(1, v.pages, "one read: the rows newer than the newest held");
  }
  TEST_ASSERT_EQUAL_MESSAGE(0, maxReads, "a steady-state turn reads no card row");
}

// RULINGS-v3 item 4: after a boot the cache is empty and the backlog after thru is 400
// rows. The first turn's one page is the NEWEST rows; the rest is named as unread. The
// idle pump then fills toward thru a page at a time, and the result is the whole chat.
static void test_the_first_turn_after_boot_carries_the_newest_rows() {
  Store s;
  const std::string thru = s.turn("c1", 0);
  for (int i = 1; i <= 200; i++) s.turn("c1", i);   // 400 conversation rows after thru
  ConvCache cache;
  const ConvCache::View first = cache.access("c1", thru, kWide, 1, s.reader("c1"));
  const auto all = s.truth("c1", thru);
  TEST_ASSERT_EQUAL(400, (int)all.size());
  TEST_ASSERT_EQUAL(1, first.pages);
  TEST_ASSERT_EQUAL((int)ConvCache::kPageRows, (int)first.rows.size());
  TEST_ASSERT_EQUAL_STRING(all.back().c_str(), first.rows.back().id.c_str());   // the newest exchange
  TEST_ASSERT_EQUAL_STRING(all[all.size() - ConvCache::kPageRows].c_str(), first.rows.front().id.c_str());
  TEST_ASSERT_FALSE(first.complete);
  TEST_ASSERT_TRUE(first.span.uncounted);
  TEST_ASSERT_EQUAL_STRING(thru.c_str(), first.span.afterId.c_str());
  TEST_ASSERT_EQUAL_STRING(first.rows.front().id.c_str(), first.span.beforeId.c_str());
  // The idle pump: one page a pass until the anchor.
  int passes = 0;
  while (passes < 100 && cache.fillStep([&](const std::string& c) { return s.reader(c); })) passes++;
  TEST_ASSERT_TRUE(passes >= 400 / (int)ConvCache::kPageRows - 1);
  TEST_ASSERT_TRUE(passes <= 400 / (int)ConvCache::kPageRows + 1);
  const ConvCache::View full = cache.access("c1", thru, kWide, 1, s.reader("c1"));
  TEST_ASSERT_TRUE(full.complete);
  TEST_ASSERT_TRUE(full.span.empty());
  assertIds(all, ids(full));
}

// A turn never reads more than one fill page, however far the fill is from done.
static void test_a_turn_reads_at_most_one_fill_page() {
  Store s;
  for (int i = 0; i < 100; i++) s.turn("c1", i);
  ConvCache cache;
  for (int t = 0; t < 3; t++) {
    const ConvCache::View v = cache.access("c1", "", kWide, 1, s.reader("c1"));
    TEST_ASSERT_TRUE_MESSAGE(v.pages <= 2, "the delta read and one fill page");
    TEST_ASSERT_EQUAL((int)ConvCache::kPageRows * (t + 1), (int)v.rows.size());
  }
}

// A fold moves thru: the rows it covered leave, nothing is read for it.
static void test_a_fold_trims() {
  Store s;
  std::vector<std::string> users;
  for (int i = 0; i < 12; i++) users.push_back(s.turn("c1", i));
  ConvCache cache;
  fillAll(cache, s, "c1");
  const std::string thru = users[6];
  const ConvCache::View v = cache.access("c1", thru, kWide, 0, s.reader("c1"));
  TEST_ASSERT_TRUE(v.complete);
  assertIds(s.truth("c1", thru), ids(v));
  // An anchor that moves back (the chat's fold state was reset): read again.
  const ConvCache::View back = cache.access("c1", "", kWide, 0, s.reader("c1"));
  TEST_ASSERT_FALSE(back.complete);
}

// /clear: the boundary row ends the conversation; the rows before it leave the cache.
static void test_a_clear_clears() {
  Store s;
  for (int i = 0; i < 8; i++) s.turn("c1", i);
  ConvCache cache;
  fillAll(cache, s, "c1");
  const std::string boundary = s.add("c1", "system", MsgKind::Message, agent::kClearBoundaryText, agent::kClearBoundaryTag);
  s.turn("c1", 99);
  const ConvCache::View v = cache.access("c1", "", kWide, 1, s.reader("c1"));
  TEST_ASSERT_EQUAL_STRING(boundary.c_str(), v.rows.front().id.c_str());
  TEST_ASSERT_EQUAL(3, (int)v.rows.size());   // the boundary, then the new exchange
  // A cache filled after the /clear stops at the boundary too.
  ConvCache fresh;
  const ConvCache::View f = fresh.access("c1", "", kWide, 1, s.reader("c1"));
  TEST_ASSERT_TRUE(f.complete);
  TEST_ASSERT_EQUAL_STRING(boundary.c_str(), f.rows.front().id.c_str());
}

// A photo's record arrives after the cache read its row: the next access shows it.
static void test_an_overlay_edit_patches() {
  Store s;
  for (int i = 0; i < 3; i++) s.turn("c1", i);
  const std::string photo = s.add("c1", "user", MsgKind::Image, "[image 1/1 ref=abc] (pending)");
  ConvCache cache;
  cache.access("c1", "", kWide, 1, s.reader("c1"));
  s.overlay.put(photo, "[image 1/1 ref=abc] a bowl of yogurt with blueberries");
  const ConvCache::View v = cache.access("c1", "", kWide, 1, s.reader("c1"));
  bool patched = false;
  for (const auto& m : v.rows)
    if (m.id == photo) patched = m.text == "[image 1/1 ref=abc] a bowl of yogurt with blueberries";
  TEST_ASSERT_TRUE(patched);
  // More records than the journal holds while the cache was not looking: it reads again.
  for (size_t i = 0; i < kPatchJournal + 2; i++) s.overlay.put("m00000001", "x" + std::to_string(i));
  const ConvCache::View again = cache.access("c1", "", kWide, 1, s.reader("c1"));
  TEST_ASSERT_TRUE(again.pages >= 1);
  for (const auto& m : again.rows)
    if (m.id == photo) TEST_ASSERT_EQUAL_STRING("[image 1/1 ref=abc] a bowl of yogurt with blueberries", m.text.c_str());
}

// The bound holds: the oldest rows leave and are counted in the span with their hours, so
// the request can name them. A fill that meets the bound stops (the rest stays unread).
static void test_the_bound_holds_and_names_what_left() {
  Store s;
  for (int i = 0; i < 10; i++) s.turn("c1", i, 1000);
  ConvCache cache;
  const ConvCache::Bound tight{6000, 0};
  fillAll(cache, s, "c1");
  for (int i = 10; i < 14; i++) s.turn("c1", i, 1000);
  const ConvCache::View v = cache.access("c1", "", tight, 1, s.reader("c1"));
  TEST_ASSERT_TRUE(cache.bytesOf("c1") <= 6000);
  TEST_ASSERT_FALSE(v.span.empty());
  TEST_ASSERT_FALSE(v.span.uncounted);
  const auto all = s.truth("c1", "");
  TEST_ASSERT_EQUAL((int)all.size(), (int)(v.span.rows + v.rows.size()));
  TEST_ASSERT_EQUAL_STRING(v.rows.front().id.c_str(), v.span.beforeId.c_str());
  TEST_ASSERT_TRUE(v.span.fromHours > 0 && v.span.toHours >= v.span.fromHours);
  // A fresh cache under the same bound: the fill stops at it and the rest is unread.
  ConvCache fresh;
  ConvCache::View f;
  for (int i = 0; i < 10 && !f.complete; i++) f = fresh.access("c1", "", tight, 1, s.reader("c1"));
  TEST_ASSERT_TRUE(f.complete);
  TEST_ASSERT_TRUE(f.span.uncounted);
  TEST_ASSERT_TRUE(fresh.bytesOf("c1") <= 6000);
}

// More than a page of rows arrived while the chat was away (another task wrote them): the
// entry is filled again newest first, never forward from the old newest (which would put
// the oldest of them first and leave the newest unread).
static void test_a_full_delta_refills_newest_first() {
  Store s;
  for (int i = 0; i < 4; i++) s.turn("c1", i);
  ConvCache cache;
  cache.access("c1", "", kWide, 1, s.reader("c1"));
  for (int i = 4; i < 40; i++) s.turn("c1", i);   // 72 new rows
  const ConvCache::View v = cache.access("c1", "", kWide, 1, s.reader("c1"));
  const auto all = s.truth("c1", "");
  TEST_ASSERT_EQUAL_STRING(all.back().c_str(), v.rows.back().id.c_str());
  TEST_ASSERT_TRUE(v.span.uncounted);
}

// At most kChats chats: the least recently used goes.
static void test_three_chats_lru() {
  Store s;
  ConvCache cache;
  for (const char* c : {"a", "b", "c"}) {
    s.turn(c, 1);
    cache.access(c, "", kWide, 1, s.reader(c));
  }
  cache.access("a", "", kWide, 1, s.reader("a"));   // a is used again
  s.turn("d", 1);
  cache.access("d", "", kWide, 1, s.reader("d"));
  TEST_ASSERT_EQUAL(3, (int)cache.chats());
  TEST_ASSERT_TRUE(cache.holds("a"));
  TEST_ASSERT_FALSE(cache.holds("b"));
  cache.drop("a");
  TEST_ASSERT_FALSE(cache.holds("a"));
}

// A store under its per-call read budget answers short pages: a short page is not the start
// of the chat, so the cache keeps filling until a page comes back empty, and holds every row.
static void test_a_short_page_is_not_the_start() {
  Store s;
  for (int i = 0; i < 20; i++) s.turn("c1", i);
  const ConvCache::ReadFn full = s.reader("c1");
  const ConvCache::ReadFn stingy = [&full](const HistoryQuery& q) {
    std::vector<EpisodicMessage> rows = full(q);
    if (q.after.empty() && rows.size() > 5) rows.resize(5);   // the budget stopped the call
    return rows;
  };
  ConvCache cache;
  ConvCache::View v;
  for (int i = 0; i < 40 && !v.complete; i++) v = cache.access("c1", "", kWide, 1, stingy);
  TEST_ASSERT_TRUE(v.complete);
  assertIds(s.truth("c1", ""), ids(v));
}

// A record that grew a row's text keeps the byte count true: a fold that then takes every
// row leaves the cache at zero, never wrapped around (which would evict every later row).
static void test_a_patch_keeps_the_byte_count() {
  Store s;
  std::string last;
  for (int i = 0; i < 4; i++) last = s.turn("c1", i);
  const std::string photo = s.add("c1", "user", MsgKind::Image, "[image 1/1 ref=abc] (pending)");
  const std::string after = s.turn("c1", 9);
  ConvCache cache;
  fillAll(cache, s, "c1");
  s.overlay.put(photo, "[image 1/1 ref=abc] " + std::string(3000, 'p'));
  cache.access("c1", "", kWide, 1, s.reader("c1"));
  cache.access("c1", after, kWide, 1, s.reader("c1"));   // a fold took every row
  TEST_ASSERT_TRUE_MESSAGE(cache.bytesOf("c1") < 100, std::to_string(cache.bytesOf("c1")).c_str());
  s.turn("c1", 10);
  const ConvCache::View v = cache.access("c1", after, kWide, 1, s.reader("c1"));
  TEST_ASSERT_EQUAL(2, (int)v.rows.size());
  TEST_ASSERT_TRUE(v.span.empty());
}

// ---- through the engine (RULINGS-v3 item 4) ------------------------------------------

namespace {
// A device's turn engine over the SD log after a restart: the boot scan indexed only the
// newest rows, the ring holds 64.
struct EngineRig {
  harness_test::FakePlatform plat;
  harness_test::FakeConfig cfg;
  FakeFs* fs;
  AppendLogEpisodicStore log;
  RowTextOverlay overlay;
  agent::HistoryMarks marks;
  std::vector<std::string> instructions, inputs;
  uint32_t seq;
  std::unique_ptr<agent::TurnEngine> eng;

  EngineRig(FakeFs& f, uint32_t nextId, const std::string& thru) : fs(&f), log(f, "/mem/episodic", 64), seq(nextId) {
    log.hydrate(300, 0);   // a truncated boot scan
    overlay.bind(&log);
    marks.thru = thru;
    cfg.toolLoop = false;
    agent::TurnEngine::Deps d;
    d.cfg = cfg.contract();
    d.platform = plat.contract();
    d.deliver = [](const std::string&, const std::string&) {};
    d.composeInputs = [](const std::string&) {
      agent::ComposeInputs in;
      in.devName = "Nimbus";
      return in;
    };
    d.apply.deliver = d.deliver;
    d.apply.principalFor = [](const std::string& chat) { return principalForRole(chat, Role::Admin); };
    d.hosts.add("anthropic", [this](std::string&, const std::string& ins, const std::string& in, std::string& out,
                                    std::string&, const agent::HeadTools*, TokenUsage*) {
      instructions.push_back(ins);
      inputs.push_back(in);
      out = "{\"reply\":\"ok\",\"memory\":\"\",\"ask\":\"\"}";
      return true;
    });
    d.histV2 = [] { return true; };
    d.historyRows = [this](const std::string& c, const HistoryQuery& hq) {
      MsgQuery q;
      q.sessionId = c;
      q.haveKind = true;
      q.kind = MsgKind::Message;
      q.alsoKinds = {MsgKind::Image, MsgKind::File, MsgKind::Audio};
      q.limit = (int)hq.limit;
      q.before = hq.before;
      q.after = hq.after;
      return overlay.query(q);
    };
    d.captureUserRow = [this](const std::string& c, const std::string& t, const std::string& tags) {
      EpisodicMessage m;
      char id[16];
      snprintf(id, sizeof(id), "m%08x", (unsigned)(++seq));
      m.id = id;
      m.sessionId = c;
      m.tsHours = kHour0 + seq / 8;
      m.role = "user";
      m.text = t;
      m.tags = tags;
      overlay.addMessage(m);
      return m.id;
    };
    d.historyMarks = [this](const std::string&) { return marks; };
    eng.reset(new agent::TurnEngine(std::move(d)));
  }
};
}  // namespace

// The first turn after a restart with a 400-row backlog after thru carries the NEWEST
// exchanges before the restart (the one card page a turn may read), names the rest as not
// read yet, and the idle pump fills the rest a page a pass.
static void test_the_first_turn_after_a_restart_carries_the_newest_exchanges() {
  Store s;
  const std::string thru = s.turn("c1", 0);
  for (int i = 1; i <= 200; i++) s.turn("c1", i, 500);   // 400 conversation rows, 1,200 rows in all
  EngineRig r(s.fs, s.seq, thru);
  TEST_ASSERT_TRUE(r.log.hydrateTruncated());
  r.eng->handleMessage("what did you just say?", "Roy", "c1");
  TEST_ASSERT_EQUAL(1, (int)r.instructions.size());
  const std::string& ins = r.instructions.back();
  TEST_ASSERT_TRUE_MESSAGE(ins.find("answer 200 ") != std::string::npos, "the newest exchange before the restart");
  TEST_ASSERT_TRUE(ins.find("answer 199 ") != std::string::npos);
  TEST_ASSERT_TRUE(ins.find("answer 1 ") == std::string::npos);
  const std::string& in = r.inputs.back();
  TEST_ASSERT_TRUE_MESSAGE(in.find("[CONVERSATION COVERAGE]") != std::string::npos, in.c_str());
  TEST_ASSERT_TRUE(in.find("still reading them") != std::string::npos);
  // At most one page of card reads on the turn.
  TEST_ASSERT_TRUE(r.eng->historyRead().cardReads > 0);
  TEST_ASSERT_TRUE_MESSAGE(r.eng->historyRead().cardReads <= 24, std::to_string(r.eng->historyRead().cardReads).c_str());
  // The idle pump fills toward thru, a page a pass, and the next turn reads no card row.
  int passes = 0;
  while (passes < 100 && r.eng->historyIdleStep()) passes++;
  TEST_ASSERT_TRUE(passes > 1 && passes < 100);
  r.eng->handleMessage("and the first thing I asked today?", "Roy", "c1");
  TEST_ASSERT_EQUAL_UINT32(0, r.eng->historyRead().cardReads);   // steady state
  TEST_ASSERT_TRUE(r.inputs.back().find("still reading them") == std::string::npos);
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_appends_match_store_rows_with_no_card_read);
  RUN_TEST(test_the_first_turn_after_boot_carries_the_newest_rows);
  RUN_TEST(test_a_turn_reads_at_most_one_fill_page);
  RUN_TEST(test_a_fold_trims);
  RUN_TEST(test_a_clear_clears);
  RUN_TEST(test_an_overlay_edit_patches);
  RUN_TEST(test_the_bound_holds_and_names_what_left);
  RUN_TEST(test_a_full_delta_refills_newest_first);
  RUN_TEST(test_three_chats_lru);
  RUN_TEST(test_a_short_page_is_not_the_start);
  RUN_TEST(test_a_patch_keeps_the_byte_count);
  RUN_TEST(test_the_first_turn_after_a_restart_carries_the_newest_exchanges);
  return UNITY_END();
}
