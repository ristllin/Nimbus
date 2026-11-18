// test_tx_stub_rule - the pure rules of what a request shows (PRD section 2, "The stub
// rule"; lane RB-B0b writes them, lane RB-T3 makes them pass). Stub texts in the PRD's
// shape and bounded, outside stubs that hold no outside byte, the one monotonic pointer
// and its moves, superseded reads and day views, photo visibility, the overflow ladder,
// the omission line. Each rule is tested over its class (every input of a grid), not one
// instance. The shape checkers below are counter-tested on wrong strings, so they bite.
//
// Owner: the battery (code-owner review to edit). Lanes may add tests.
#include <unity.h>

#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

#include "../support/tx_battery/items.h"
#include "../support/tx_battery/pending.h"
#include "nimbus/orch/tx/rules.h"

namespace tx = nimbus::orch::tx;

void setUp() {}
void tearDown() {}

namespace {

bool validUtf8(const std::string& s) {
  size_t i = 0;
  while (i < s.size()) {
    const unsigned char c = (unsigned char)s[i];
    const size_t n = c < 0x80 ? 1 : (c >> 5) == 0x6 ? 2 : (c >> 4) == 0xE ? 3 : (c >> 3) == 0x1E ? 4 : 0;
    if (n == 0 || i + n > s.size()) return false;
    for (size_t k = 1; k < n; ++k)
      if (((unsigned char)s[i + k] >> 6) != 0x2) return false;
    i += n;
  }
  return true;
}

// The internal stub's frame: '[stub <tool> r<q> <bytes>B <lines>L <HH:MM>] ... (full: view id=r<q>)',
// one line, at most 400 B, valid UTF-8.
bool internalStubOk(const tx::StubFacts& f, const std::string& s, std::string& why) {
  const std::string head = "[stub " + f.tool + " r" + std::to_string(f.q) + " " + std::to_string(f.bytes) + "B " +
                           std::to_string(f.lines) + "L " + f.hhmm + "] ";
  const std::string tail = " ... (full: view id=r" + std::to_string(f.q) + ")";
  if (s.rfind(head, 0) != 0) why = "does not start with '" + head + "': " + s.substr(0, 80);
  else if (s.size() < tail.size() || s.compare(s.size() - tail.size(), tail.size(), tail) != 0)
    why = "does not end with the view hint '" + tail + "'";
  else if (s.size() > tx::kStubMaxBytes) why = "is " + std::to_string(s.size()) + " B, over 400";
  else if (s.find('\n') != std::string::npos) why = "is not one line";
  else if (!validUtf8(s)) why = "cuts a UTF-8 sequence";
  return why.empty();
}

// The outside stub: the call and its size only, never a byte of the result.
bool outsideStubOk(const tx::StubFacts& f, const std::string& marker, const std::string& s, std::string& why) {
  const std::string want = "[stub " + f.tool + "(" + f.args.substr(0, tx::kStubArgsPreviewBytes) + ") r" +
                           std::to_string(f.q) + " " + std::to_string(f.bytes) +
                           "B outside content, not shown: view id=r" + std::to_string(f.q) + "]";
  if (s.find(marker) != std::string::npos) why = "outside stub carries outside bytes ('" + marker + "')";
  else if (s != want) why = "outside stub is '" + s + "', want '" + want + "'";
  return why.empty();
}

tx::StubFacts internalFacts(const std::string& preview) {
  tx::StubFacts f;
  f.tool = "read_file";
  f.q = 42;
  f.bytes = 15204;
  f.lines = 310;
  f.hhmm = "13:02";
  f.preview = preview;
  f.cls = tx::ResultClass::Bulk;
  return f;
}

tx::StubFacts outsideFacts(const std::string& args, const std::string& preview) {
  tx::StubFacts f;
  f.tool = "web_search";
  f.q = 7;
  f.bytes = 3000;
  f.lines = 40;
  f.hhmm = "13:02";
  f.args = args;
  f.preview = preview;
  f.cls = tx::ResultClass::Outside;
  return f;
}

// Previews over the class: empty, short, long, multi-line, multibyte at the 200 B cut.
std::vector<std::string> previews() {
  std::vector<std::string> v = {"", "x", "line one\nline two\nline three", std::string(5000, 'a')};
  for (size_t pad = 195; pad <= 201; ++pad) v.push_back(std::string(pad, 'b') + "\xC3\xA9\xF0\x9F\x8D\xB2 more");
  return v;
}

// ---- counter-tests: the shape checkers reject wrong stubs ------------------------
void test_checker_rejects_wrong_internal_stubs() {
  const tx::StubFacts f = internalFacts("hello");
  std::string why;
  TEST_ASSERT_TRUE(internalStubOk(f, "[stub read_file r42 15204B 310L 13:02] hello ... (full: view id=r42)", why));
  const char* bad[] = {
      "[stub read_file r42 15204B 310L 13:02] hello",                                  // no view hint
      "[stub read_file r43 15204B 310L 13:02] hello ... (full: view id=r42)",          // wrong q
      "[stub read_file r42 15204B 310L 13:02] hel\nlo ... (full: view id=r42)",        // two lines
      "[stub read_file r42 15204B 310L 13:02] \xC3 ... (full: view id=r42)",           // cut UTF-8
  };
  for (const char* s : bad) {
    why.clear();
    TEST_ASSERT_FALSE_MESSAGE(internalStubOk(f, s, why), s);
  }
  why.clear();
  TEST_ASSERT_FALSE(internalStubOk(f, "[stub read_file r42 15204B 310L 13:02] " + std::string(400, 'z') +
                                         " ... (full: view id=r42)", why));
}

void test_checker_rejects_leaking_outside_stubs() {
  const tx::StubFacts f = outsideFacts(R"({"q":"train times"})", "OUTSIDE-SECRET page text");
  std::string why;
  TEST_ASSERT_TRUE(outsideStubOk(
      f, "OUTSIDE-SECRET",
      R"([stub web_search({"q":"train times"}) r7 3000B outside content, not shown: view id=r7])", why));
  why.clear();
  TEST_ASSERT_FALSE(outsideStubOk(
      f, "OUTSIDE-SECRET",
      R"([stub web_search({"q":"train times"}) r7 3000B OUTSIDE-SECRET page: view id=r7])", why));
  why.clear();
  TEST_ASSERT_FALSE(outsideStubOk(f, "OUTSIDE-SECRET", "[stub web_search r7 3000B]", why));
}

// ---- stub texts ------------------------------------------------------------------
void test_internal_stub_has_the_prd_shape() {
  TXB_WAITS_FOR("PENDING RB-T3");
  for (const std::string& p : previews()) {
    const tx::StubFacts f = internalFacts(p);
    std::string s, why;
    TXB_OK(tx::stubText(f, s));
    TEST_ASSERT_TRUE_MESSAGE(internalStubOk(f, s, why), why.c_str());
    // The preview is the result's first bytes on one line (at most 200 B of it).
    std::string first = p.substr(0, 40);
    for (char& c : first)
      if (c == '\n') c = ' ';
    TEST_ASSERT_TRUE_MESSAGE(s.find(first) != std::string::npos, "the preview's first bytes are not in the stub");
  }
}

void test_stub_text_is_bounded_for_every_tool_and_size() {
  TXB_WAITS_FOR("PENDING RB-T3");
  const std::string tools[] = {"read_file", "ledger_day", std::string(60, 't')};
  const uint64_t qs[] = {1, 999999, 18446744073709551615ULL};
  for (const std::string& tool : tools)
    for (uint64_t q : qs)
      for (const std::string& p : previews()) {
        tx::StubFacts f = internalFacts(p);
        f.tool = tool;
        f.q = q;
        f.bytes = 4294967295u;
        std::string s;
        TXB_OK(tx::stubText(f, s));
        TEST_ASSERT_TRUE_MESSAGE(s.size() <= tx::kStubMaxBytes, s.c_str());
        TEST_ASSERT_TRUE_MESSAGE(validUtf8(s), "a stub cuts a UTF-8 sequence");
        TEST_ASSERT_TRUE_MESSAGE(s.find("view id=r" + std::to_string(q)) != std::string::npos, "no view hint");
      }
}

void test_outside_stub_holds_no_outside_bytes() {
  TXB_WAITS_FOR("PENDING RB-T3");
  const std::string args[] = {R"({"q":"train times"})", "{\"url\":\"https://example.test/" + std::string(300, 'u') + "\"}"};
  for (const std::string& a : args)
    for (const std::string& p : previews()) {
      const tx::StubFacts f = outsideFacts(a, "OUTSIDE-SECRET " + p);
      std::string s, why;
      TXB_OK(tx::stubText(f, s));
      TEST_ASSERT_TRUE_MESSAGE(outsideStubOk(f, "OUTSIDE-SECRET", s, why), why.c_str());
      TEST_ASSERT_TRUE(s.size() <= tx::kStubMaxBytes);
    }
}

void test_file_tool_stub_names_path_and_sha() {
  TXB_WAITS_FOR("PENDING RB-T3");
  tx::StubFacts f = internalFacts("# Boat\nNereid II");
  f.path = "notes/boat.md";
  f.sha8 = "ab12cd34";
  std::string s;
  TXB_OK(tx::stubText(f, s));
  TEST_ASSERT_TRUE_MESSAGE(s.find("path=notes/boat.md") != std::string::npos, s.c_str());
  TEST_ASSERT_TRUE_MESSAGE(s.find("sha8=ab12cd34") != std::string::npos, s.c_str());
  TEST_ASSERT_TRUE_MESSAGE(s.find("view id=r42") != std::string::npos, s.c_str());
  TEST_ASSERT_TRUE(s.size() <= tx::kStubMaxBytes);
}

void test_long_argument_and_photo_stubs_have_the_prd_shape() {
  TXB_WAITS_FOR("PENDING RB-T3");
  std::string s;
  TXB_OK(tx::longArgStub(15204, "ab12cd34", 9, s));
  TEST_ASSERT_EQUAL_STRING("[stub 15204B sha:ab12cd34: view id=r9]", s.c_str());
  TXB_OK(tx::photoStub("ab12cd34", "Tue 17 Nov 13:02", s));
  TEST_ASSERT_EQUAL_STRING("[photo pab12cd34 Tue 17 Nov 13:02 not shown: view id=pab12cd34]", s.c_str());
  TXB_OK(tx::omissionLine(3, "15-16 Nov", s));
  TEST_ASSERT_EQUAL_STRING("[3 earlier turns, 15-16 Nov, not shown: search finds them]", s.c_str());
}

// A stub is computed once at capture: the same facts give the same bytes, every time.
void test_stub_text_is_a_pure_function() {
  TXB_WAITS_FOR("PENDING RB-T3");
  for (const std::string& p : previews()) {
    std::string a, b;
    TXB_OK(tx::stubText(internalFacts(p), a));
    TXB_OK(tx::stubText(internalFacts(p), b));
    TEST_ASSERT_EQUAL_STRING(a.c_str(), b.c_str());
    TXB_OK(tx::stubText(outsideFacts("{}", p), a));
    TXB_OK(tx::stubText(outsideFacts("{}", p), b));
    TEST_ASSERT_EQUAL_STRING(a.c_str(), b.c_str());
  }
}

// ---- the pointer -------------------------------------------------------------------
tx::OpenFacts openFacts(uint64_t gapS) {
  tx::OpenFacts f;
  f.lastReqAt = txb::kT0;
  f.now = txb::kT0 + gapS;
  f.pointer = 10;
  f.lastClosedTurnQ = 40;
  return f;
}

// Rule (1): over 270 s since the last request, no clock, or after a restart, the pointer
// moves to the last closed turn; rule (2): otherwise it stays.
void test_cold_open_moves_to_the_last_closed_turn() {
  TXB_WAITS_FOR("PENDING RB-T3");
  const uint64_t gaps[] = {0, 1, 60, 269, 270, 271, 600, 86400};
  for (uint64_t gap : gaps)
    for (int variant = 0; variant < 3; ++variant) {
      tx::OpenFacts f = openFacts(gap);
      if (variant == 1) f.now = 0;              // clock not set
      if (variant == 2) f.afterRestart = true;
      tx::PointerMove m;
      TXB_OK(tx::decideTurnOpen(f, m));
      const bool cold = gap > tx::kColdGapS || variant > 0;
      char msg[96];
      std::snprintf(msg, sizeof msg, "gap %llu s variant %d", (unsigned long long)gap, variant);
      TEST_ASSERT_EQUAL_MESSAGE(cold, m.move, msg);
      if (cold) {
        TEST_ASSERT_EQUAL_UINT64_MESSAGE(40, m.to, msg);
        TEST_ASSERT_TRUE_MESSAGE(m.why == tx::MoveWhy::ColdOpen, msg);
      }
    }
}

void test_warm_open_moves_only_past_the_bulk_threshold() {
  TXB_WAITS_FOR("PENDING RB-T3");
  struct Row {
    bool floor;
    uint32_t bulk;
    bool move;
  } rows[] = {{false, 0, false},    {false, 24576, false}, {false, 24577, true},
              {true, 8192, false},  {true, 8193, true},    {true, 24577, true}};
  for (const Row& r : rows) {
    tx::OpenFacts f = openFacts(60);
    f.floorHead = r.floor;
    f.wholeBulkAbove = r.bulk;
    tx::PointerMove m;
    TXB_OK(tx::decideTurnOpen(f, m));
    char msg[64];
    std::snprintf(msg, sizeof msg, "floor %d bulk %u", r.floor, r.bulk);
    TEST_ASSERT_EQUAL_MESSAGE(r.move, m.move, msg);
    if (r.move) TEST_ASSERT_TRUE_MESSAGE(m.why == tx::MoveWhy::WarmBulk && m.to == 40, msg);
  }
}

// One monotonic pointer: whatever the facts, a move never goes backwards.
void test_pointer_never_moves_backwards() {
  TXB_WAITS_FOR("PENDING RB-T3");
  const uint64_t gaps[] = {10, 600};
  const uint64_t closed[] = {0, 5, 10, 11, 1000};
  for (uint64_t gap : gaps)
    for (uint64_t c : closed)
      for (uint32_t bulk : {0u, 100000u}) {
        tx::OpenFacts f = openFacts(gap);
        f.lastClosedTurnQ = c;
        f.wholeBulkAbove = bulk;
        tx::PointerMove m;
        TXB_OK(tx::decideTurnOpen(f, m));
        if (m.move) TEST_ASSERT_TRUE_MESSAGE(m.to > f.pointer, "the pointer moved backwards or nowhere");
      }
}

// ---- superseded reads ----------------------------------------------------------------
void test_a_write_supersedes_earlier_reads_of_that_path_only() {
  TXB_WAITS_FOR("PENDING RB-T3");
  txb::Convo c;
  c.openTurn();
  const std::string a = c.call("read_file", R"({"path":"notes/a.md"})");
  const uint64_t ra = c.receipt(a, "read_file", "alpha");
  const std::string b = c.call("read_file", R"({"path":"notes/b.md"})");
  c.receipt(b, "read_file", "beta");
  const std::string l = c.call("list_files", R"({"path":"notes"})");
  c.receipt(l, "list_files", "a.md b.md");
  const std::string a2 = c.call("read_file", R"({"path":"notes/a.md","offset":4})");
  const uint64_t ra2 = c.receipt(a2, "read_file", "ha");
  const std::vector<tx::Item> earlier = c.items;
  c.call("write_file", R"({"path":"notes/a.md","text":"new"})", true);
  std::vector<uint64_t> sup;
  TXB_OK(tx::supersededBy(c.items.back(), earlier, sup));
  std::sort(sup.begin(), sup.end());
  TEST_ASSERT_EQUAL(2, (int)sup.size());
  TEST_ASSERT_EQUAL_UINT64(ra, sup[0]);
  TEST_ASSERT_EQUAL_UINT64(ra2, sup[1]);
}

void test_a_ledger_write_supersedes_views_of_that_day_only() {
  TXB_WAITS_FOR("PENDING RB-T3");
  txb::Convo c;
  c.openTurn();
  const std::string d17 = c.call("ledger_day", R"({"day":"2026-11-17"})");
  const uint64_t r17 = c.receipt(d17, "ledger_day", "L00A3 skyr 150 g");
  const std::string d16 = c.call("ledger_day", R"({"day":"2026-11-16"})");
  c.receipt(d16, "ledger_day", "L0099 oats 40 g");
  const std::vector<tx::Item> earlier = c.items;
  c.call("ledger_write", R"({"day":"2026-11-17","add":[{"food":"apple","g":120}]})", true);
  std::vector<uint64_t> sup;
  TXB_OK(tx::supersededBy(c.items.back(), earlier, sup));
  TEST_ASSERT_EQUAL(1, (int)sup.size());
  TEST_ASSERT_EQUAL_UINT64(r17, sup[0]);
}

// ---- photos ----------------------------------------------------------------------------
tx::PhotoFacts photoFacts(tx::PhotoMode mode, size_t n, uint32_t since, uint64_t idleS) {
  tx::PhotoFacts f;
  f.mode = mode;
  f.lastOwnerTurnAt = txb::kT0;
  f.now = txb::kT0 + idleS;
  for (size_t i = 0; i < n; ++i) {
    f.candidates.push_back("p0000000" + std::to_string(i));
    f.ownerTurnsSince.push_back(since);
  }
  return f;
}

void expectVisible(const tx::PhotoFacts& f, size_t want, const char* msg) {
  std::vector<std::string> v;
  TXB_OK(tx::visiblePhotos(f, v));
  TEST_ASSERT_EQUAL_MESSAGE((int)want, (int)v.size(), msg);
  for (size_t i = 0; i < v.size(); ++i) TEST_ASSERT_EQUAL_STRING_MESSAGE(f.candidates[i].c_str(), v[i].c_str(), msg);
}

// File-reference heads: the photos stay for the next two owner turns, at most 4 (newest
// first), until 60 minutes idle or a restart.
void test_file_reference_photos_follow_the_table() {
  TXB_WAITS_FOR("PENDING RB-T3");
  expectVisible(photoFacts(tx::PhotoMode::FileRef, 3, 0, 0), 3, "the arrival turn");
  expectVisible(photoFacts(tx::PhotoMode::FileRef, 3, 1, 60), 3, "next owner turn");
  expectVisible(photoFacts(tx::PhotoMode::FileRef, 3, 2, 600), 3, "second owner turn");
  expectVisible(photoFacts(tx::PhotoMode::FileRef, 3, 3, 60), 0, "third owner turn");
  expectVisible(photoFacts(tx::PhotoMode::FileRef, 6, 1, 60), 4, "at most 4");
  expectVisible(photoFacts(tx::PhotoMode::FileRef, 3, 1, 3601), 0, "60 minutes idle");
  tx::PhotoFacts r = photoFacts(tx::PhotoMode::FileRef, 3, 1, 60);
  r.afterRestart = true;
  expectVisible(r, 0, "after a restart");
}

// Inline heads (routed included): the next owner turn only, inside 270 s, newest 2.
void test_inline_photos_follow_the_table() {
  TXB_WAITS_FOR("PENDING RB-T3");
  expectVisible(photoFacts(tx::PhotoMode::Inline, 2, 0, 0), 2, "the arrival turn");
  expectVisible(photoFacts(tx::PhotoMode::Inline, 1, 1, 270), 1, "next turn inside 270 s");
  expectVisible(photoFacts(tx::PhotoMode::Inline, 1, 1, 271), 0, "next turn after 270 s");
  expectVisible(photoFacts(tx::PhotoMode::Inline, 1, 2, 30), 0, "second owner turn");
  expectVisible(photoFacts(tx::PhotoMode::Inline, 3, 1, 30), 2, "newest 2");
  expectVisible(photoFacts(tx::PhotoMode::None, 2, 1, 30), 0, "a head without vision");
}

// ---- the ladder --------------------------------------------------------------------------
void test_ladder_climbs_one_rung_at_a_time() {
  TXB_WAITS_FOR("PENDING RB-T3");
  tx::LadderFacts f;
  f.bodyCap = 85000;
  f.bodyBytes = 85000;
  tx::LadderRung r;
  TXB_OK(tx::nextRung(f, r));
  TEST_ASSERT_TRUE_MESSAGE(r == tx::LadderRung::None, "a body at the cap needs no rung");
  f.bodyBytes = 85001;
  const tx::LadderRung order[] = {tx::LadderRung::StubClosedTurns, tx::LadderRung::StubOldBulk,
                                  tx::LadderRung::OmitOldTurns, tx::LadderRung::SizeGuard, tx::LadderRung::SizeGuard};
  f.applied = tx::LadderRung::None;
  for (tx::LadderRung want : order) {
    TXB_OK(tx::nextRung(f, r));
    TEST_ASSERT_EQUAL((int)want, (int)r);
    f.applied = r;
  }
}

}  // namespace

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_checker_rejects_wrong_internal_stubs);
  RUN_TEST(test_checker_rejects_leaking_outside_stubs);
  RUN_TEST(test_internal_stub_has_the_prd_shape);
  RUN_TEST(test_stub_text_is_bounded_for_every_tool_and_size);
  RUN_TEST(test_outside_stub_holds_no_outside_bytes);
  RUN_TEST(test_file_tool_stub_names_path_and_sha);
  RUN_TEST(test_long_argument_and_photo_stubs_have_the_prd_shape);
  RUN_TEST(test_stub_text_is_a_pure_function);
  RUN_TEST(test_cold_open_moves_to_the_last_closed_turn);
  RUN_TEST(test_warm_open_moves_only_past_the_bulk_threshold);
  RUN_TEST(test_pointer_never_moves_backwards);
  RUN_TEST(test_a_write_supersedes_earlier_reads_of_that_path_only);
  RUN_TEST(test_a_ledger_write_supersedes_views_of_that_day_only);
  RUN_TEST(test_file_reference_photos_follow_the_table);
  RUN_TEST(test_inline_photos_follow_the_table);
  RUN_TEST(test_ladder_climbs_one_rung_at_a_time);
  return UNITY_END();
}
