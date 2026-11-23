// test_tx_search - reading one chat's stored conversation back (lane E2-SRCH, tx/search.h):
//   - a fact stored three weeks and 600 turns back is found with a date range and without one;
//   - the remainder is true: following the cursors from the newest line reaches the start, and every
//     matching line is found exactly once across the pages (whatever the page's budget);
//   - the earlier engine's rows below the import's verbatim floor are searched after the transcript,
//     the rows the import brought over are not searched twice, engine 2's own rows never;
//   - an outside result keeps its class (the hit is marked outside), the search and view tools' own
//     results and the open turn are never searched;
//   - view's lookups: an item by q (across segments), its whole text paged exactly (a bulk payload
//     included), a photo of this conversation only;
//   - nothing is written by a search (the card is byte-identical after it), a malformed cursor is
//     refused, a refused read is Io (never "nothing found").
#include <unity.h>

#include <algorithm>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "../support/tx_battery/mem_files.h"
#include "nimbus/orch/tx/log.h"
#include "nimbus/orch/tx/old_engine.h"
#include "nimbus/orch/tx/search.h"

namespace tx = nimbus::orch::tx;

void setUp() {}
void tearDown() {}

namespace {

constexpr uint64_t kNow = 1794920520ULL;    // Tue 17 Nov 2026 13:02 UTC
constexpr uint64_t kDay = 86400;
const char* kChat = "1001";

// A chat written through the real log.
struct Chat {
  txb::MemFiles files;
  tx::TxLog log{files, kChat};
  uint64_t q = 1;
  uint32_t turn = 0;
  Chat() {
    tx::HeadState h;
    TEST_ASSERT_TRUE(log.open(h).ok());
  }
  uint64_t put(tx::Item it) {
    it.q = q++;
    if (!it.turn) it.turn = turn;
    tx::StoreState s;
    const tx::Status st = log.append(it, s);
    TEST_ASSERT_TRUE_MESSAGE(st.ok() && s == tx::StoreState::Stored, st.where);
    return it.q;
  }
  uint64_t owner(const std::string& text, uint64_t t, uint64_t m = 0) {
    ++turn;
    tx::Item o;
    o.kind = tx::ItemKind::TurnOpen;
    o.t = t;
    put(o);
    tx::Item u;
    u.kind = tx::ItemKind::User;
    u.x = text;
    u.t = t;
    u.m = m;
    return put(u);
  }
  uint64_t reply(const std::string& text, uint64_t t) {
    tx::Item a;
    a.kind = tx::ItemKind::Assistant;
    a.x = text;
    a.t = t;
    a.fin = true;
    const uint64_t at = put(a);
    tx::Item e;
    e.kind = tx::ItemKind::TurnEnd;
    e.t = t;
    e.end.cause = "reply";
    put(e);
    return at;
  }
  uint64_t result(const std::string& tool, const std::string& text, uint64_t t, bool outside) {
    tx::Item c;
    c.kind = tx::ItemKind::Call;
    c.id = "c" + std::to_string(q);
    c.n = tool;
    c.args = "{}";
    c.t = t;
    put(c);
    tx::Item r;
    r.kind = tx::ItemKind::Result;
    r.id = c.id;
    r.n = tool;
    r.t = t;
    r.bytes = (uint32_t)text.size();
    r.cls = outside ? tx::ResultClass::Outside : tx::ResultClass::Receipt;
    if (outside || text.size() > tx::kTier1Receipt) {
      if (!outside) r.cls = tx::ResultClass::Bulk;
      TEST_ASSERT_TRUE(log.appendBulk(turn, text, r.handle).ok());
    } else {
      r.x = text;
    }
    return put(r);
  }
  std::string snapshot() const {
    std::string s;
    for (const auto& kv : files.files) s += kv.first + "\n" + kv.second + "\n";
    return s;
  }
};

// The earlier engine's rows of the chat, newest first by row id (the store's `before` cursor).
class FakeOld : public tx::OldRows {
 public:
  std::vector<tx::DayRow> rows;   // any order
  uint32_t pages = 0;
  bool refuse = false;
  tx::Status page(const tx::OldPageAsk& a, tx::OldPage& out, tx::ReadStats& st) override {
    if (refuse) return tx::Status::fail(tx::Err::Io, "fake.old");
    ++pages;
    std::vector<tx::DayRow> sorted = rows;
    std::sort(sorted.begin(), sorted.end(), [](const tx::DayRow& x, const tx::DayRow& y) { return x.id > y.id; });
    const uint64_t below = a.before.empty() ? UINT64_MAX : tx::rowIdNumber(a.before);
    out = tx::OldPage{};
    for (const tx::DayRow& r : sorted) {
      if (r.id >= below) continue;
      if (out.rows.size() == a.max) {
        out.older = true;
        break;
      }
      out.rows.push_back(r);
    }
    if (!out.rows.empty()) out.next = tx::rowIdString(out.rows.back().id);
    st.add(1, 64 * out.rows.size());
    return tx::Status::success();
  }
  tx::Status row(uint64_t id, tx::DayRow& out, tx::ReadStats&) override {
    for (const tx::DayRow& r : rows)
      if (r.id == id) return out = r, tx::Status::success();
    return tx::Status::fail(tx::Err::NotFound, "fake.row");
  }
};

tx::DayRow oldRow(uint64_t id, uint64_t t, tx::RowRole role, const std::string& text, bool ours = false) {
  tx::DayRow r;
  r.id = id;
  r.t = t;
  r.role = role;
  r.text = text;
  r.ours = ours;
  return r;
}

// Three weeks of chat: 600 turns, one fact planted at the start, ordinary turns after it.
const char* kFact = "Mira's flight EZ8123 lands at 07:40 on 14 December";
uint64_t kFactQ = 0;
const uint64_t kStart = kNow - 21 * kDay;

void threeWeeks(Chat& c, uint32_t turns = 600) {
  const std::string pad(1900, 'x');   // about 2 KB a turn: the log rolls into a second segment
  for (uint32_t i = 0; i < turns; ++i) {
    const uint64_t t = kStart + i * (21 * kDay / turns);
    if (i == 3) kFactQ = c.owner(std::string("note this: ") + kFact, t);
    else c.owner("ordinary message " + std::to_string(i) + " about the garden " + pad, t);
    c.reply("ok " + std::to_string(i), t + 5);
  }
}

// Every page from the newest line to the start.
std::vector<tx::SearchOutcome> allPages(const tx::SearchDeps& d, tx::SearchAsk a, size_t maxPages = 100000) {
  std::vector<tx::SearchOutcome> pages;
  while (pages.size() < maxPages) {
    tx::SearchOutcome o;
    const tx::Status st = tx::search(d, a, o);
    TEST_ASSERT_TRUE_MESSAGE(st.ok(), st.where);
    pages.push_back(o);
    if (o.next.empty()) break;
    TEST_ASSERT_FALSE(o.next.empty());
    TEST_ASSERT_TRUE(tx::cursorOk(o.next));
    a.before = o.next;
  }
  return pages;
}

}  // namespace

static void test_words_match_all_case_insensitive() {
  const auto w = tx::queryWords("Mira  FLIGHT, ez8123?");
  TEST_ASSERT_EQUAL(3, (int)w.size());
  TEST_ASSERT_EQUAL_STRING("mira", w[0].c_str());
  TEST_ASSERT_TRUE(tx::matchesAll("Mira's flight EZ8123 lands", w));
  TEST_ASSERT_FALSE(tx::matchesAll("Mira's train EZ8123", w));
  TEST_ASSERT_FALSE(tx::matchesAll("anything", tx::queryWords("  ,.; ")));   // no word: nothing
  TEST_ASSERT_TRUE(tx::matchesAll("Café Zürich", tx::queryWords("café")));    // bytes over 0x7f are letters
  // A hit needs the words near each other: one line, or two lines in a row.
  const auto near = tx::queryWords("kettle hedge 18");
  TEST_ASSERT_TRUE(tx::matchesNear("a\nchecked the kettle and the hedge, 18 minutes\nb", near));
  TEST_ASSERT_TRUE(tx::matchesNear("Mira's flight:\nthe kettle hedge 18", near));
  TEST_ASSERT_TRUE(tx::matchesNear("kettle\nhedge 18", near));
  TEST_ASSERT_FALSE(tx::matchesNear("kettle\nx\nhedge 18", near));   // three lines apart
  TEST_ASSERT_TRUE(tx::matchesAll("kettle\nx\nhedge 18", near));
  TEST_ASSERT_TRUE(tx::matchesNear("one long line with kettle then hedge then 18", near));
}

static void test_a_fact_three_weeks_and_600_turns_back_is_found_without_and_with_a_range() {
  Chat c;
  threeWeeks(c);
  TEST_ASSERT_TRUE_MESSAGE(c.files.segments("/mem/tx/1001").size() >= 2, "the log rolled");
  tx::SearchDeps d;
  d.files = &c.files;
  d.chatKey = kChat;
  // No range: newest first, in pages; following the remainder reaches it.
  tx::SearchAsk a;
  a.query = "mira flight";
  const auto pages = allPages(d, a);
  size_t found = 0;
  for (const auto& p : pages)
    for (const auto& h : p.hits) {
      TEST_ASSERT_EQUAL_STRING(("r" + std::to_string(kFactQ)).c_str(), h.id.c_str());
      TEST_ASSERT_TRUE(h.source == tx::HitSource::Person);
      ++found;
    }
  TEST_ASSERT_EQUAL(1, (int)found);
  TEST_ASSERT_TRUE(pages.back().next.empty());
  TEST_ASSERT_EQUAL_MESSAGE(1, (int)pages.size(), "one call reads the whole conversation (1.3 MB)");
  TEST_ASSERT_TRUE(pages[0].complete);
  // A range that holds the day: found in one call.
  tx::SearchAsk r = a;
  r.fromT = kStart - kDay;
  r.toT = kStart + 2 * kDay;
  tx::SearchOutcome o;
  TEST_ASSERT_TRUE(tx::search(d, r, o).ok());
  TEST_ASSERT_EQUAL(1, (int)o.hits.size());
  TEST_ASSERT_TRUE(o.next.empty());
  // A range that does not hold it: nothing, and the whole conversation was read.
  r.fromT = kNow - 3 * kDay;
  r.toT = kNow;
  TEST_ASSERT_TRUE(tx::search(d, r, o).ok());
  TEST_ASSERT_EQUAL(0, (int)o.hits.size());
  TEST_ASSERT_TRUE(o.next.empty() && o.complete);
}

// The remainder is true at any window size: the pages together hold every match exactly once.
static void test_the_remainder_is_true_and_every_match_is_found_once() {
  Chat c;
  threeWeeks(c, 300);
  for (size_t budget : {size_t(8192), size_t(20000), size_t(70000), tx::kSearchWindowBytes}) {
    tx::SearchDeps d;
    d.files = &c.files;
    d.chatKey = kChat;
    d.limits.windowBytes = budget;
    d.limits.maxHits = 5;
    tx::SearchAsk a;
    a.query = "garden";
    const auto pages = allPages(d, a);
    std::vector<std::string> ids;
    for (const auto& p : pages)
      for (const auto& h : p.hits) ids.push_back(h.id);
    TEST_ASSERT_EQUAL_MESSAGE(299, (int)ids.size(), ("budget " + std::to_string(budget)).c_str());
    std::vector<std::string> uniq = ids;
    std::sort(uniq.begin(), uniq.end());
    TEST_ASSERT_TRUE(std::unique(uniq.begin(), uniq.end()) == uniq.end());
    TEST_ASSERT_TRUE(pages.size() >= 60);   // five hits a page at most
  }
}

static void test_old_rows_below_the_import_floor_are_searched_after_the_transcript() {
  Chat c;
  FakeOld old;
  // The earlier engine's rows 1..40: 1..20 were digested or named by the import, 21..40 imported.
  for (uint64_t id = 1; id <= 40; ++id)
    old.rows.push_back(oldRow(id, kStart + id * 3600, id % 2 ? tx::RowRole::Owner : tx::RowRole::Assistant,
                              id == 7 ? "the boiler pressure is 1.3 bar" : "old row " + std::to_string(id)));
  old.rows.push_back(oldRow(41, kStart + 41 * 3600, tx::RowRole::Tool, "boiler tool output 1.3 bar"));   // tool rows never
  old.rows.push_back(oldRow(50, kNow - 100, tx::RowRole::Owner, "boiler 1.3 bar again", /*ours=*/true));   // engine 2's own
  for (uint64_t id = 21; id <= 40; id += 2) c.owner("old row " + std::to_string(id), kStart + id * 3600, id);
  c.owner("boiler 1.3 bar, said now", kNow - 60);
  tx::SearchDeps d;
  d.files = &c.files;
  d.chatKey = kChat;
  d.old = &old;
  tx::SearchAsk a;
  a.query = "boiler 1.3";
  const auto pages = allPages(d, a);
  std::vector<std::string> ids;
  for (const auto& p : pages)
    for (const auto& h : p.hits) ids.push_back(h.id);
  TEST_ASSERT_EQUAL(2, (int)ids.size());
  TEST_ASSERT_EQUAL_STRING("m00000007", ids[1].c_str());
  TEST_ASSERT_TRUE(ids[0][0] == 'r');
  uint64_t floor = 0;
  TEST_ASSERT_TRUE(tx::verbatimFloor(c.files, kChat, floor).ok());
  TEST_ASSERT_EQUAL(21, (int)floor);
  // The rows the import brought over (21..40) are not read twice: one row id below the floor each.
  tx::SearchAsk any;
  any.query = "old row";
  std::vector<std::string> oldIds;
  for (const auto& p : allPages(d, any))
    for (const auto& h : p.hits)
      if (h.id[0] == 'm') oldIds.push_back(h.id);
  TEST_ASSERT_EQUAL(19, (int)oldIds.size());   // 1..20 without row 7
  for (const std::string& id : oldIds) TEST_ASSERT_TRUE(tx::rowIdNumber(id) < 21);
  // A small budget pages through both parts and still finds each once.
  d.limits.maxHits = 2;
  std::vector<std::string> paged;
  for (const auto& p : allPages(d, any))
    for (const auto& h : p.hits) paged.push_back(h.id);
  TEST_ASSERT_EQUAL(29, (int)paged.size());   // 10 imported in the transcript + 19 old
  std::sort(paged.begin(), paged.end());
  TEST_ASSERT_TRUE(std::unique(paged.begin(), paged.end()) == paged.end());
}

static void test_outside_content_keeps_its_class_and_own_results_and_the_open_turn_are_skipped() {
  Chat c;
  c.owner("look up the opening hours", kNow - 600);
  c.result("web_search", "The museum opens at 09:30 on weekdays (page text).", kNow - 590, /*outside=*/true);
  c.result("calc", "museum budget 42", kNow - 580, false);
  c.result("search", "r3 museum opens 09:30", kNow - 570, false);   // the search tool's own result
  c.reply("It opens at 09:30.", kNow - 560);
  c.owner("when does the museum open?", kNow - 10);   // the open turn: the question itself
  const uint32_t open = c.turn;
  tx::SearchDeps d;
  d.files = &c.files;
  d.chatKey = kChat;
  tx::SearchAsk a;
  a.query = "museum";
  a.openTurn = open;
  tx::SearchOutcome o;
  TEST_ASSERT_TRUE(tx::search(d, a, o).ok());
  TEST_ASSERT_EQUAL(2, (int)o.hits.size());   // the page and calc; not search's own result, not the question
  int outside = 0;
  for (const auto& h : o.hits) {
    if (h.outside) {
      ++outside;
      TEST_ASSERT_EQUAL_STRING("web_search", h.tool.c_str());
      TEST_ASSERT_TRUE(h.excerpt.find("09:30") != std::string::npos);
    }
    TEST_ASSERT_TRUE(h.tool != "search");
  }
  TEST_ASSERT_EQUAL(1, outside);
}

static void test_a_search_writes_nothing_and_a_refused_read_is_io() {
  Chat c;
  threeWeeks(c, 50);
  const std::string before = c.snapshot();
  tx::SearchDeps d;
  d.files = &c.files;
  d.chatKey = kChat;
  tx::SearchAsk a;
  a.query = "garden";
  tx::SearchOutcome o;
  TEST_ASSERT_TRUE(tx::search(d, a, o).ok());
  TEST_ASSERT_EQUAL_STRING(before.c_str(), c.snapshot().c_str());
  TEST_ASSERT_TRUE(o.card.reads > 0 && o.card.bytes > 0);
  a.before = "t1.x";
  TEST_ASSERT_TRUE(tx::search(d, a, o).err == tx::Err::InvalidArgument);
  a.before = "../1002";
  TEST_ASSERT_TRUE(tx::search(d, a, o).err == tx::Err::InvalidArgument);
  // Old rows that do not answer: Io, and the cursor to try again.
  FakeOld old;
  old.refuse = true;
  d.old = &old;
  a.before.clear();
  a.query = "message 12";
  const tx::Status st = tx::search(d, a, o);
  TEST_ASSERT_TRUE(st.err == tx::Err::Io);
  TEST_ASSERT_EQUAL(0, (int)o.hits.size());   // a window is never returned half-ranked
  TEST_ASSERT_EQUAL_STRING("", o.next.c_str());   // the same call again
}

static void test_view_finds_an_item_and_pages_its_whole_text() {
  Chat c;
  threeWeeks(c, 600);
  tx::Item it;
  TEST_ASSERT_TRUE(tx::findItem(c.files, kChat, kFactQ, it).ok());
  TEST_ASSERT_TRUE(it.x.find(kFact) != std::string::npos);
  const uint64_t last = c.q - 1;
  TEST_ASSERT_TRUE(tx::findItem(c.files, kChat, last, it).ok());   // in the second segment
  TEST_ASSERT_EQUAL((int)last, (int)it.q);
  TEST_ASSERT_TRUE(tx::findItem(c.files, kChat, c.q + 5, it).err == tx::Err::NotFound);
  // A bulk payload, paged exactly.
  std::string big;
  for (int i = 0; i < 2000; ++i) big += "line " + std::to_string(i) + "\n";
  const uint64_t rq = c.result("read_file", big, kNow, false);
  TEST_ASSERT_TRUE(tx::findItem(c.files, kChat, rq, it).ok());
  std::string all, page;
  size_t total = 0;
  for (size_t off = 0; off < big.size(); off += page.size()) {
    TEST_ASSERT_TRUE(tx::itemText(c.files, kChat, it, tx::TextPage{off, 4096}, page, total).ok());
    TEST_ASSERT_EQUAL((int)big.size(), (int)total);
    TEST_ASSERT_TRUE(!page.empty());
    all += page;
  }
  TEST_ASSERT_EQUAL_STRING(big.c_str(), all.c_str());
}

static void test_a_photo_is_found_only_in_its_own_conversation() {
  Chat c, other;
  tx::Item u;
  u.kind = tx::ItemKind::User;
  u.x = "a photo";
  u.t = kNow - 100;
  tx::Part p;
  p.ref = "p0123abcd";
  p.mime = "image/jpeg";
  u.parts.push_back(p);
  c.put(u);
  c.owner("later", kNow);
  other.owner("no photos here", kNow);
  tx::Item owner;
  bool found = false;
  TEST_ASSERT_TRUE(tx::findPhoto(c.files, kChat, "p0123abcd", owner, found).ok());
  TEST_ASSERT_TRUE(found);
  TEST_ASSERT_EQUAL((int)(kNow - 100), (int)owner.t);
  TEST_ASSERT_TRUE(tx::findPhoto(other.files, kChat, "p0123abcd", owner, found).ok());
  TEST_ASSERT_FALSE(found);
  TEST_ASSERT_TRUE(tx::findPhoto(c.files, kChat, "p0123", owner, found).ok());   // a prefix is not the photo
  TEST_ASSERT_FALSE(found);
}


// A long assistant text keeps its rest in the bulk file: a word only there is found. A bulk read the
// card refuses stops the call with Io (never "searched to the start"), and the next call reads that
// line again.
static void test_a_long_reply_is_searched_whole_and_a_refused_bulk_read_is_io() {
  Chat c;
  c.owner("write me a long story", kNow - 100);
  const std::string whole = std::string(13000, 'a') + " the hidden word zanzibar at the end";
  tx::Item a;
  a.kind = tx::ItemKind::Assistant;
  a.t = kNow - 90;
  a.x = whole.substr(0, 12288);
  TEST_ASSERT_TRUE(c.log.appendBulk(c.turn, whole, a.handle).ok());
  c.put(a);
  tx::SearchDeps d;
  d.files = &c.files;
  d.chatKey = kChat;
  tx::SearchAsk q;
  q.query = "zanzibar";
  tx::SearchOutcome o;
  TEST_ASSERT_TRUE(tx::search(d, q, o).ok());
  TEST_ASSERT_EQUAL(1, (int)o.hits.size());
  TEST_ASSERT_TRUE(o.hits[0].source == tx::HitSource::Assistant);
  // The bulk file is cut short (pruned): counted as unreadable, the search goes on.
  txb::MemFiles& f = c.files;
  std::string saved;
  for (auto& kv : f.files)
    if (kv.first.find("/b/") != std::string::npos) saved = kv.first;
  TEST_ASSERT_FALSE(saved.empty());
  const std::string bytes = f.files[saved];
  f.files[saved] = bytes.substr(0, 10);
  TEST_ASSERT_TRUE(tx::search(d, q, o).ok());
  TEST_ASSERT_EQUAL(1, (int)o.unreadBulk);
  TEST_ASSERT_TRUE(o.complete && o.next.empty());
  f.files[saved] = bytes;
  // The card refuses the bulk read: Io, no "to the start", and the cursor reads that line again.
  struct Refusing : tx::TxFiles {
    tx::TxFiles& in;
    explicit Refusing(tx::TxFiles& i) : in(i) {}
    bool append(const std::string&, const char*, size_t) override { return false; }
    bool readAt(const std::string& p, size_t off, char* b, size_t w, size_t& g) override {
      if (p.find("/b/") != std::string::npos) return false;
      return in.readAt(p, off, b, w, g);
    }
    bool size(const std::string& p, size_t& n) override { return in.size(p, n); }
    bool writeWhole(const std::string&, const std::string&) override { return false; }
    bool rename(const std::string&, const std::string&) override { return false; }
    bool remove(const std::string&) override { return false; }
    bool list(const std::string& dir, std::vector<std::string>& n) override { return in.list(dir, n); }
  } refusing(c.files);
  d.files = &refusing;
  const tx::Status st = tx::search(d, q, o);
  TEST_ASSERT_TRUE(st.err == tx::Err::Io);
  TEST_ASSERT_EQUAL(0, (int)o.hits.size());
  TEST_ASSERT_TRUE(o.next.empty());   // the same (fresh) call again
  d.files = &c.files;
  q.before = o.next;
  TEST_ASSERT_TRUE(tx::search(d, q, o).ok());
  TEST_ASSERT_EQUAL_MESSAGE(1, (int)o.hits.size(), "the refused line is read again from the cursor");
}


// Lane E2-SRCH2 (red on 4f776c621, where the five newest matches came first): a fact stated once 600
// turns back, whose words are also in ten newer rows as scattered words, is the first hit of the first
// call; with the exact phrase it is hit one whatever the number of newer partial matches.
static void test_the_best_match_is_first_whatever_its_age() {
  Chat c;
  const uint64_t start = kNow - 21 * kDay;
  uint64_t factQ = 0;
  for (uint32_t i = 0; i < 600; ++i) {
    const uint64_t t = start + i * (20 * kDay / 600);
    if (i == 0) factQ = c.owner("the spare key hangs behind the blue watering can in the shed", t);
    else if (i % 50 == 0)   // ten newer rows with every word, over two lines (a weaker match)
      c.owner("the spare blue key ring\nby the watering can", t);
    else if (i % 25 == 0)   // and more, scattered over lines (not a match)
      c.owner("the blue paint\nis in the shed\nnext to the watering can\nwith the spare\nbrush and the key ring", t);
    else if (i % 7 == 0) c.owner("the spare blue key", t);   // two of the words in one line
    else c.owner("ordinary message " + std::to_string(i), t);
    c.reply("ok", t + 5);
  }
  tx::SearchDeps d;
  d.files = &c.files;
  d.chatKey = kChat;
  tx::SearchAsk a;
  a.query = "spare key blue watering can";
  tx::SearchOutcome o;
  TEST_ASSERT_TRUE(tx::search(d, a, o).ok());
  TEST_ASSERT_TRUE(o.complete);
  TEST_ASSERT_FALSE(o.hits.empty());
  TEST_ASSERT_EQUAL_STRING(("r" + std::to_string(factQ)).c_str(), o.hits[0].id.c_str());
  TEST_ASSERT_TRUE(o.hits[0].rank == tx::MatchRank::Line);
  // The exact phrase: hit one over every newer partial match.
  for (uint32_t i = 0; i < 40; ++i) c.owner("behind the shed: the blue can, the watering hose", kNow - 1000 + i);
  a.query = "behind the blue watering can";
  TEST_ASSERT_TRUE(tx::search(d, a, o).ok());
  TEST_ASSERT_EQUAL_STRING(("r" + std::to_string(factQ)).c_str(), o.hits[0].id.c_str());
  TEST_ASSERT_TRUE(o.hits[0].rank == tx::MatchRank::Phrase);
  TEST_ASSERT_TRUE(o.hits[0].excerpt.find("behind the blue watering can") != std::string::npos);
  // The count of the rest is true, and the cursor reaches each of them once.
  std::set<std::string> seen;
  uint32_t total = o.matches;
  tx::SearchOutcome cur = o;
  for (int guard = 0; guard < 100; ++guard) {
    for (const auto& h : cur.hits) TEST_ASSERT_TRUE(seen.insert(h.id).second);
    TEST_ASSERT_EQUAL(total, cur.matches);
    TEST_ASSERT_EQUAL((int)total - (int)seen.size(), (int)cur.further);
    if (cur.next.empty()) break;
    a.before = cur.next;
    TEST_ASSERT_TRUE(tx::search(d, a, cur).ok());
  }
  TEST_ASSERT_EQUAL((int)total, (int)seen.size());
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_words_match_all_case_insensitive);
  RUN_TEST(test_the_best_match_is_first_whatever_its_age);
  RUN_TEST(test_a_fact_three_weeks_and_600_turns_back_is_found_without_and_with_a_range);
  RUN_TEST(test_the_remainder_is_true_and_every_match_is_found_once);
  RUN_TEST(test_old_rows_below_the_import_floor_are_searched_after_the_transcript);
  RUN_TEST(test_outside_content_keeps_its_class_and_own_results_and_the_open_turn_are_skipped);
  RUN_TEST(test_a_search_writes_nothing_and_a_refused_read_is_io);
  RUN_TEST(test_view_finds_an_item_and_pages_its_whole_text);
  RUN_TEST(test_a_photo_is_found_only_in_its_own_conversation);
  RUN_TEST(test_a_long_reply_is_searched_whole_and_a_refused_bulk_read_is_io);
  return UNITY_END();
}
