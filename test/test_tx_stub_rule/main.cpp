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
#include <map>
#include <string>
#include <vector>

#include "../support/tx_battery/items.h"
#include "../support/tx_battery/pending.h"
#include "nimbus/orch/tx/rules.h"

namespace tx = nimbus::orch::tx;

void setUp() {}
void tearDown() {}

namespace {
const tx::BoundTools kAllBound{true, true};   // E2-FIX5: search and view registered

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
  f.bound.view = true;   // the forms below are the view-bound ones (E2-FIX5: unbound in its own test)
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
  f.bound.view = true;
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
  const std::string tools[] = {"read_file", "list_files", std::string(60, 't')};
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
  // E2-VERB: a file read's stub names the read that brings it back, path and range.
  f.again = tx::wayBack("read_file", R"({"path":"notes/boat.md","offset":32761,"limit":4000})");
  TEST_ASSERT_EQUAL_STRING("read_file path=notes/boat.md offset=32761 limit=4000", f.again.c_str());
  TXB_OK(tx::stubText(f, s));
  TEST_ASSERT_TRUE_MESSAGE(s.find("(not shown in full to fit the request; read_file path=notes/boat.md offset=32761 limit=4000 reads it again)") != std::string::npos, s.c_str());
  TEST_ASSERT_TRUE(s.size() <= tx::kStubMaxBytes);
  TEST_ASSERT_TRUE(tx::looksLikeStub(s));
  TEST_ASSERT_EQUAL_STRING("", tx::wayBack("calc", R"({"path":"notes/boat.md"})").c_str());
  TEST_ASSERT_EQUAL_STRING("", tx::wayBack("read_file", "[stub 15204B sha:ab12cd34: view id=r9]").c_str());
}

void test_long_argument_and_photo_stubs_have_the_prd_shape() {
  TXB_WAITS_FOR("PENDING RB-T3");
  std::string s;
  TXB_OK(tx::longArgStub(15204, "ab12cd34", 9, s, std::string(), kAllBound));
  TEST_ASSERT_EQUAL_STRING("[stub 15204B sha:ab12cd34: an argument sent earlier, not content; view id=r9]", s.c_str());
  // E2-VERB: a file write's argument names the file it wrote, and every stub is refused as content.
  TXB_OK(tx::longArgStub(15204, "ab12cd34", 9, s, tx::wayBack("write_file", R"({"path":"notes/a.md","content":"x"})")));
  TEST_ASSERT_EQUAL_STRING("[stub 15204B sha:ab12cd34: an argument sent earlier, not content; read_file path=notes/a.md shows the file]", s.c_str());
  TEST_ASSERT_TRUE(tx::looksLikeStub(s));
  TEST_ASSERT_TRUE(tx::looksLikeStub("  [stub 1720B sha:c4091c8e: view id=r4]\n"));
  TEST_ASSERT_FALSE(tx::looksLikeStub("[stub] is how the parser marks a placeholder.\nSecond line."));
  TEST_ASSERT_FALSE(tx::looksLikeStub("Journal, day 1 [stub 1720B sha:c4091c8e: view id=r4]"));
  TXB_OK(tx::photoStub("ab12cd34", "Tue 17 Nov 13:02", s));
  TEST_ASSERT_EQUAL_STRING("[photo pab12cd34 Tue 17 Nov 13:02: shown earlier in this conversation; not attached to this request]", s.c_str());
  TXB_OK(tx::omissionLine(3, "15-16 Nov", s, kAllBound));
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
// Lane E2-VERB (rulings 81, 82 and 106): the pointer has no default move any more (the cold and warm
// opens are gone); test_tx_verbatim pins that a request under the due mark replays every earlier
// call and result whole. What is left here: which x items' lists make stubs.
void test_only_the_tool_fold_lists_make_stubs() {
  tx::Item x;
  x.kind = tx::ItemKind::PointerMove;
  x.why = tx::moveWhySlug(tx::MoveWhy::Superseded);
  TEST_ASSERT_FALSE_MESSAGE(tx::stubList(x), "a superseded-read list stubbed the stale read");
  x.why = tx::moveWhySlug(tx::MoveWhy::ToolFold);
  TEST_ASSERT_TRUE(tx::stubList(x));
  x.kind = tx::ItemKind::Result;
  TEST_ASSERT_FALSE(tx::stubList(x));
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

// ---- photos (lane E2-FIX4: in view until compaction's pointer or a cap, never by the clock) ----
// n candidates, newest first: `arrivals` of them this turn's, then one per earlier person turn, each
// of `size` stored bytes.
tx::PhotoFacts photoFacts(tx::PhotoMode mode, size_t n, size_t arrivals = 0, uint32_t size = 60000) {
  tx::PhotoFacts f;
  f.mode = mode;
  for (size_t i = 0; i < n; ++i) {
    f.candidates.push_back("p0000000" + std::to_string(i));
    f.ownerTurnsSince.push_back(i < arrivals ? 0 : (uint32_t)(i - arrivals + 1));
    f.bytes.push_back(size);
  }
  return f;
}

void expectVisible(const tx::PhotoFacts& f, size_t want, const char* msg) {
  std::vector<std::string> v;
  TXB_OK(tx::visiblePhotos(f, v));
  TEST_ASSERT_EQUAL_MESSAGE((int)want, (int)v.size(), msg);
  for (size_t i = 0; i < v.size(); ++i) TEST_ASSERT_EQUAL_STRING_MESSAGE(f.candidates[i].c_str(), v[i].c_str(), msg);
}

// D1 over the class: for every head mode with vision, every number of earlier photos, every photo
// size and every person-turn distance, the visible set is exactly the newest prefix that fits both
// caps: a photo stays on every later turn until a cap pushes it out, the oldest falls out first, and
// nothing else (no distance in turns) takes part. None shows nothing.
void test_photos_stay_in_view_until_a_cap_pushes_the_oldest_out() {
  size_t cases = 0;
  for (tx::PhotoMode mode : {tx::PhotoMode::Inline, tx::PhotoMode::FileRef, tx::PhotoMode::None})
    for (size_t n = 0; n <= tx::kFileRefPhotosInViewMax + 3; ++n)
      for (uint32_t size : {1000u, 60000u, 120000u, 200000u, 400000u, 700000u})
        for (uint32_t gap : {1u, 2u, 13u, 400u}) {   // person turns between two photos
          tx::PhotoFacts f = photoFacts(mode, n, 0, size);
          for (size_t i = 0; i < n; ++i) f.ownerTurnsSince[i] = 1 + (uint32_t)i * gap;
          size_t want = 0, bytes = 0;
          while (want < n && want < tx::photosInViewMax(mode) && bytes + size <= tx::photoBytesInViewMax(mode)) {
            bytes += size;
            ++want;
          }
          char msg[96];
          std::snprintf(msg, sizeof msg, "mode %d n %u size %u gap %u", (int)mode, (unsigned)n, (unsigned)size, (unsigned)gap);
          expectVisible(f, want, msg);
          ++cases;
        }
  TEST_ASSERT_TRUE(cases > 500);
  // The caps hold: never more than the count, never more than the bytes, on any head.
  TEST_ASSERT_EQUAL(0, (int)tx::photosInViewMax(tx::PhotoMode::None));
  expectVisible(photoFacts(tx::PhotoMode::Inline, 9), tx::kInlinePhotosInViewMax, "inline count cap");
  expectVisible(photoFacts(tx::PhotoMode::FileRef, 12), tx::kFileRefPhotosInViewMax, "file-reference count cap");
  // An older, smaller photo never takes the place of a newer one that does not fit.
  tx::PhotoFacts mixed = photoFacts(tx::PhotoMode::Inline, 3);
  mixed.bytes = {200000, (uint32_t)tx::kInlinePhotoBytesInViewMax, 1000};
  expectVisible(mixed, 1, "the second does not fit: the third stays out too");
}

// Photo after photo, turn after turn: a photo once shown stays shown until a cap pushes it out, and
// once out it never comes back (no oscillation: the line, then pixels, then the line again).
void test_a_photo_leaves_view_once_and_in_arrival_order() {
  for (tx::PhotoMode mode : {tx::PhotoMode::Inline, tx::PhotoMode::FileRef}) {
    std::vector<std::string> refs;   // newest first
    std::vector<uint32_t> since;
    std::map<std::string, int> changes;
    std::map<std::string, bool> was;
    for (int turn = 0; turn < 30; ++turn) {
      for (uint32_t& k : since) ++k;
      if (turn % 3 == 0) {   // a photo every third turn
        refs.insert(refs.begin(), "p" + std::to_string(10000000 + turn));
        since.insert(since.begin(), 0);
      }
      tx::PhotoFacts f;
      f.mode = mode;
      f.candidates = refs;
      f.ownerTurnsSince = since;
      f.bytes.assign(refs.size(), 120000);
      std::vector<std::string> v;
      TXB_OK(tx::visiblePhotos(f, v));
      for (const std::string& r : refs) {
        const bool shown = std::find(v.begin(), v.end(), r) != v.end();
        if (was.count(r) && was[r] != shown) ++changes[r];
        was[r] = shown;
      }
      for (size_t i = 1; i < v.size(); ++i) TEST_ASSERT_TRUE(v[i - 1] > v[i]);   // newest first, a prefix
    }
    for (const auto& c : changes) TEST_ASSERT_TRUE_MESSAGE(c.second <= 1, c.first.c_str());
  }
}

// The arrival turn and the device facts the rule still reads: the arrival shows at most 4 and
// counts first against the caps; no card carries nothing earlier; a head without vision sees nothing.
void test_rules_photo_table_edges() {
  expectVisible(photoFacts(tx::PhotoMode::FileRef, 6, 6), 4, "arrival at most 4");
  expectVisible(photoFacts(tx::PhotoMode::Inline, 6, 6), 4, "inline arrival at most 4");
  expectVisible(photoFacts(tx::PhotoMode::Inline, 6, 2), tx::kInlinePhotosInViewMax, "two arrivals, then the newest earlier");
  expectVisible(photoFacts(tx::PhotoMode::Inline, 3, 1, (uint32_t)(tx::kInlinePhotoBytesInViewMax / 2 + 1)), 1,
                "the arrival's bytes count first");
  expectVisible(photoFacts(tx::PhotoMode::None, 2, 2), 0, "no vision: not even on arrival");
  expectVisible(photoFacts(tx::PhotoMode::None, 2), 0, "no vision: nothing earlier");
  tx::PhotoFacts noCard = photoFacts(tx::PhotoMode::FileRef, 3, 1);
  noCard.noCard = true;
  expectVisible(noCard, 1, "no card: the arrival turn only");
  tx::PhotoFacts unknown = photoFacts(tx::PhotoMode::Inline, 3);
  unknown.bytes.clear();
  expectVisible(unknown, 3, "sizes unknown: the count cap alone");
  tx::PhotoFacts bad = photoFacts(tx::PhotoMode::FileRef, 2);
  bad.ownerTurnsSince.pop_back();
  std::vector<std::string> v;
  TEST_ASSERT_TRUE(tx::visiblePhotos(bad, v).err == tx::Err::InvalidArgument);
  bad = photoFacts(tx::PhotoMode::FileRef, 2);
  bad.bytes.pop_back();
  TEST_ASSERT_TRUE(tx::visiblePhotos(bad, v).err == tx::Err::InvalidArgument);
}

// Counter-test, rule side: the rule's facts are the mode, the card and the per-photo lists, nothing
// else (no clock, idle time or restart flag; written as an aggregate so a field added in front of
// these fails to compile here). The engine side (test_plain_conformance, photo_tests) renders one
// conversation at every gap around the old 270 s and 3,600 s marks and across restarts.
void test_the_photo_rule_reads_no_clock() {
  const tx::PhotoFacts f{tx::PhotoMode::Inline, false, {"p00000001"}, {5}, {60000}};
  std::vector<std::string> v;
  TXB_OK(tx::visiblePhotos(f, v));
  TEST_ASSERT_EQUAL(1, (int)v.size());
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


// ---- lane RB-T3 additions: the rest of each rule's class -----------------------------------

// A stub is computed once at capture, stored on the r line and read back byte for byte after
// a restart (encode, decode); recomputing from the same facts gives the same bytes.
void test_rules_stub_survives_a_restart() {
  for (const std::string& p : previews())
    for (int outside = 0; outside < 2; ++outside) {
      const tx::StubFacts f = outside ? outsideFacts(R"({"q":"tide"})", p) : internalFacts(p);
      tx::Item r;
      r.q = f.q;
      r.kind = tx::ItemKind::Result;
      r.id = "toolu_0001";
      r.n = f.tool;
      r.cls = f.cls;
      TXB_OK(tx::stubText(f, r.st));
      std::string line;
      TXB_OK(tx::encodeLine(r, line));
      tx::Item back;
      TXB_OK(tx::decodeLine(line.data(), line.size(), back));
      TEST_ASSERT_EQUAL_STRING(r.st.c_str(), back.st.c_str());
      std::string again;
      TXB_OK(tx::stubText(f, again));
      TEST_ASSERT_EQUAL_STRING(r.st.c_str(), again.c_str());
    }
}

// Outside content never rides a stub: over generated payloads, no 8-byte window of the
// result appears in its stub (with and without a card).
void test_rules_outside_stub_never_carries_result_bytes() {
  uint32_t seed = 12345;
  auto next = [&seed] { return seed = seed * 1103515245u + 12345u; };
  const char alpha[] = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
  for (int k = 0; k < 200; ++k) {
    std::string body;
    const size_t len = 8 + next() % 600;
    for (size_t i = 0; i < len; ++i) body += alpha[(next() >> 8) % 62];
    tx::StubFacts f = outsideFacts(R"({"q":"x"})", body);
    f.noCard = (k % 2) == 1;
    f.bytes = (uint32_t)body.size();
    std::string s;
    TXB_OK(tx::stubText(f, s));
    for (size_t i = 0; i + 8 <= body.size(); ++i)
      TEST_ASSERT_TRUE_MESSAGE(s.find(body.substr(i, 8)) == std::string::npos, s.c_str());
  }
}

// Every byte value in a preview, a tool name, a path or the outside arguments gives one line
// of valid UTF-8 with no control byte, within 400 B.
void test_rules_every_byte_gives_one_clean_line() {
  for (int b = 0; b < 256; ++b) {
    const std::string odd = std::string("a") + (char)b + "b" + (char)b;
    for (int field = 0; field < 4; ++field) {
      tx::StubFacts f = field == 3 ? outsideFacts(odd, odd) : internalFacts(odd);
      if (field == 1) f.tool = "t" + odd;
      if (field == 2) f.path = odd;
      std::string s;
      TXB_OK(tx::stubText(f, s));
      TEST_ASSERT_TRUE(validUtf8(s));
      TEST_ASSERT_TRUE(s.size() <= tx::kStubMaxBytes);
      for (char c : s) TEST_ASSERT_FALSE_MESSAGE((unsigned char)c < 0x20 || c == 0x7F, "a control byte in a stub");
    }
  }
}

// No card: no view hint (nothing to view), and still the PRD's frame.
void test_rules_no_card_stub_says_not_kept() {
  tx::StubFacts f = internalFacts("abc");
  f.noCard = true;
  std::string s;
  TXB_OK(tx::stubText(f, s));
  TEST_ASSERT_EQUAL_STRING("[stub read_file r42 15204B 310L 13:02] abc ... (full result not kept: no storage card)",
                           s.c_str());
  f.hhmm = "";
  f.noCard = false;
  TXB_OK(tx::stubText(f, s));
  TEST_ASSERT_TRUE_MESSAGE(s.rfind("[stub read_file r42 15204B 310L --:--] abc", 0) == 0, s.c_str());
}

// Every path write supersedes reads of its path; nothing else does.
void test_rules_every_path_write_supersedes_its_reads() {
  const char* writes[] = {R"(write_file|{"path":"n/a.md","content":"x"})", R"(append_file|{"path":"n/a.md","text":"x"})",
                          R"(edit_file|{"path":"n/a.md","old":"a","new":"b"})", R"(delete_file|{"path":"n/a.md"})",
                          R"(move_file|{"from":"n/a.md","to":"n/c.md"})"};
  for (const char* w : writes) {
    const std::string spec = w;
    const std::string name = spec.substr(0, spec.find('|')), args = spec.substr(spec.find('|') + 1);
    txb::Convo c;
    c.openTurn();
    const uint64_t r1 = c.receipt(c.call("read_file", R"({"path":"n/a.md"})"), "read_file", "alpha");
    const uint64_t r2 = c.receipt(c.call("file_stat", R"({"path":"n/a.md"})"), "file_stat", "size=5");
    const uint64_t r3 = c.receipt(c.call("read_pdf", R"({"path":"n/a.md"})"), "read_pdf", "alpha");
    c.receipt(c.call("read_file", R"({"path":"n/b.md"})"), "read_file", "beta");
    c.receipt(c.call("list_files", R"({"path":"n/a.md"})"), "list_files", "a.md");
    c.receipt(c.call("search", R"({"query":"n/a.md"})"), "search", "n/a.md");
    const std::vector<tx::Item> earlier = c.items;
    c.call(name, args, true);
    std::vector<uint64_t> sup;
    TXB_OK(tx::supersededBy(c.items.back(), earlier, sup));
    TEST_ASSERT_EQUAL_MESSAGE(3, (int)sup.size(), w);
    TEST_ASSERT_EQUAL_UINT64(r1, sup[0]);
    TEST_ASSERT_EQUAL_UINT64(r2, sup[1]);
    TEST_ASSERT_EQUAL_UINT64(r3, sup[2]);
  }
  // A read is not a write; a long-argument stub supersedes nothing; a non-call is refused.
  txb::Convo c;
  c.openTurn();
  c.receipt(c.call("read_file", R"({"path":"n/a.md"})"), "read_file", "alpha");
  const std::vector<tx::Item> earlier = c.items;
  std::vector<uint64_t> sup;
  c.call("read_file", R"({"path":"n/a.md"})");
  TXB_OK(tx::supersededBy(c.items.back(), earlier, sup));
  TEST_ASSERT_EQUAL(0, (int)sup.size());
  c.call("write_file", "[stub 15204B sha:ab12cd34: view id=r9]", true);
  TXB_OK(tx::supersededBy(c.items.back(), earlier, sup));
  TEST_ASSERT_EQUAL(0, (int)sup.size());
  TEST_ASSERT_TRUE(tx::supersededBy(earlier.back(), earlier, sup).err == tx::Err::InvalidArgument);
}

// One stale read costs one change point: a read already superseded, or tier-2 already a stub
// at or below the pointer, is not superseded again. A receipt at or below the pointer is
// still verbatim (Ruling 16), so it is.
void test_rules_supersede_counts_each_read_once() {
  txb::Convo c;
  c.openTurn();
  const uint64_t r1 = c.receipt(c.call("read_file", R"({"path":"n/a.md"})"), "read_file", "v1");
  const std::string big = c.call("read_file", R"({"path":"n/a.md","offset":0})");
  c.bulkResult(big, "read_file", txb::payload("BIG", 30000));
  const uint64_t r3 = c.receipt(c.call("read_file", R"({"path":"n/a.md"})"), "read_file", "v1");
  c.move(c.q, "cold", {r1});
  const std::vector<tx::Item> earlier = c.items;
  c.call("write_file", R"({"path":"n/a.md","content":"x"})", true);
  std::vector<uint64_t> sup;
  TXB_OK(tx::supersededBy(c.items.back(), earlier, sup));
  TEST_ASSERT_EQUAL(1, (int)sup.size());
  TEST_ASSERT_EQUAL_UINT64(r3, sup[0]);
}

// No tool is special for what it stores (ruling 41): only a file write supersedes, and only
// reads of the paths it names. A write by any other tool, whatever days, row ids or paths
// its arguments or its receipt name (a tracker's shapes included), supersedes nothing; the
// same reads are superseded by a file write of their path.
void test_rules_no_tool_is_special_for_what_it_stores() {
  struct Row {
    const char* tool;
    const char* args;
    const char* receipt;
  } rows[] = {
      {"ledger_write", R"({"ops":[{"op":"add","row":{"item":"apple"},"day":"2026-11-17"}]})", nullptr},
      {"ledger_write", R"({"ops":[{"op":"move","ids":["L0077"],"day":"2026-11-16"}]})",
       "L0077 moved\nday 2026-11-16 (04:00 to 04:00): 2 rows"},
      {"records_write", R"({"table":"expenses","ops":[{"op":"remove","id":"R0001"}],"path":"expenses/2026.csv"})", nullptr},
      {"note", R"({"op":"set","key":"training","value":"day 2026-11-17","path":"training/log.csv"})", nullptr},
      {"schedule", R"({"op":"create","when":"2026-11-17 18:00","text":"food/food-log.csv"})", nullptr},
  };
  for (const Row& row : rows) {
    txb::Convo c;
    c.openTurn();
    const uint64_t day = c.receipt(c.call("records_day", R"({"day":"2026-11-17"})"), "records_day", "R0001 taxi 14.80");
    // The old engine-2 day view: a write naming its day or row once superseded it.
    const uint64_t old = c.receipt(c.call("ledger_day", R"({"day":"2026-11-17"})"), "ledger_day",
                                   "L0077 rice 80 g\nday 2026-11-16 (04:00 to 04:00): 1 rows");
    const uint64_t view = c.receipt(c.call("list_files", R"({"prefix":"expenses"})"), "list_files", "expenses/2026.csv");
    uint64_t reads[3];
    const char* paths[] = {"food/food-log.csv", "expenses/2026.csv", "training/log.csv"};
    for (int i = 0; i < 3; ++i)
      reads[i] = c.receipt(c.call("read_file", std::string(R"({"path":")") + paths[i] + "\"}"), "read_file", "date,item");
    const std::vector<tx::Item> earlier = c.items;
    const std::string id = c.call(row.tool, row.args, true);
    const tx::Item call = c.items.back();
    std::vector<uint64_t> sup;
    if (row.receipt) {
      c.receipt(id, row.tool, row.receipt);
      TXB_OK(tx::supersededBy(call, c.items.back(), earlier, sup));
    } else {
      TXB_OK(tx::supersededBy(call, earlier, sup));
    }
    TEST_ASSERT_EQUAL_MESSAGE(0, (int)sup.size(), row.tool);
    // Counter: a file write of each path supersedes exactly that path's read.
    for (int i = 0; i < 3; ++i) {
      c.call("edit_file", std::string(R"({"path":")") + paths[i] + R"(","old":"a","new":"b"})", true);
      TXB_OK(tx::supersededBy(c.items.back(), earlier, sup));
      TEST_ASSERT_EQUAL_MESSAGE(1, (int)sup.size(), paths[i]);
      TEST_ASSERT_EQUAL_UINT64_MESSAGE(reads[i], sup[0], paths[i]);
    }
    (void)day;
    (void)view;
    (void)old;
  }
}

// Whole or stub, over every result class, pointer side, turn and supersession.
void test_rules_show_result_table() {
  const tx::ResultClass classes[] = {tx::ResultClass::Receipt, tx::ResultClass::Bulk, tx::ResultClass::Outside};
  for (tx::ResultClass cls : classes)
    for (uint64_t q : {5ull, 10ull, 11ull})
      for (uint32_t turn : {1u, 2u})
        for (int sup = 0; sup < 2; ++sup)
          for (uint64_t ladder : {0ull, 11ull}) {
            tx::Item r;
            r.kind = tx::ItemKind::Result;
            r.q = q;
            r.turn = turn;
            r.cls = cls;
            const std::vector<uint64_t> s = sup ? std::vector<uint64_t>{q} : std::vector<uint64_t>{};
            tx::ShowFacts f;
            f.pointer = 10;
            f.openTurn = 2;
            f.superseded = &s;
            f.ladderStubThrough = ladder;
            tx::ShowAs got;
            TXB_OK(tx::showResult(r, f, got));
            bool stub = false;
            if (sup) stub = true;
            else if (cls == tx::ResultClass::Receipt) stub = false;
            else if (q <= 10) stub = true;
            else if (cls == tx::ResultClass::Outside && turn != 2) stub = true;
            else if (turn == 2 && q <= ladder) stub = true;
            char msg[96];
            std::snprintf(msg, sizeof msg, "cls %d q %llu turn %u sup %d ladder %llu", (int)cls, (unsigned long long)q,
                          turn, sup, (unsigned long long)ladder);
            TEST_ASSERT_EQUAL_MESSAGE((int)stub, (int)(got == tx::ShowAs::Stub), msg);
          }
  tx::ShowAs got;
  TEST_ASSERT_TRUE(tx::showResult(tx::Item{}, tx::ShowFacts{}, got).err == tx::Err::InvalidArgument);
}

// Rung 3 over its class: the omitted prefix never reaches the newest 6 owner turns, frees
// enough when it can, and omits no more turns than it needs.
void test_rules_omission_span_keeps_the_newest_six_owner_turns() {
  for (uint32_t n = 0; n <= 14; ++n)
    for (uint32_t pattern = 0; pattern < 4; ++pattern)
      for (size_t need : {(size_t)0, (size_t)1, (size_t)2500, (size_t)9000, (size_t)1000000}) {
        std::vector<tx::TurnSpan> turns;
        for (uint32_t i = 0; i < n; ++i) {
          tx::TurnSpan t;
          t.turn = i + 1;
          t.owner = pattern == 0 || (i + pattern) % 3 != 0;
          t.bytes = 1000 + 100 * i;
          turns.push_back(t);
        }
        uint32_t omit = 99;
        TXB_OK(tx::omissionSpan(turns, need, omit));
        // The protected boundary: the oldest of the newest 6 owner turns (none with fewer).
        size_t bound = 0, owners = 0;
        for (size_t i = turns.size(); i-- > 0;)
          if (turns[i].owner && ++owners == tx::kNewestOwnerTurnsKept) {
            bound = i;
            break;
          }
        TEST_ASSERT_TRUE_MESSAGE(omit <= bound, "the omission reached into the newest 6 owner turns");
        size_t freed = 0;
        for (uint32_t i = 0; i < omit; ++i) freed += turns[i].bytes;
        if (omit > 0) TEST_ASSERT_TRUE_MESSAGE(freed - turns[omit - 1].bytes < need, "omitted more than needed");
        if (freed < need) TEST_ASSERT_EQUAL_MESSAGE((int)bound, (int)omit, "stopped short of the protected turns");
      }
  std::vector<tx::TurnSpan> backwards(2);
  backwards[0].turn = 5;
  backwards[1].turn = 4;
  uint32_t omit;
  TEST_ASSERT_TRUE(tx::omissionSpan(backwards, 1, omit).err == tx::Err::InvalidArgument);
}

// Ruling 11: the year in every stamp the rules write.
void test_rules_stamps_carry_the_year() {
  std::string s;
  TXB_OK(tx::photoWhen(txb::kT0, 0, s));
  TEST_ASSERT_EQUAL_STRING("Tue 17 Nov 2026 13:02", s.c_str());
  TXB_OK(tx::photoWhen(txb::kT0, 60, s));
  TEST_ASSERT_EQUAL_STRING("Tue 17 Nov 2026 14:02", s.c_str());
  TXB_OK(tx::photoWhen(0, 0, s));
  TEST_ASSERT_EQUAL_STRING("", s.c_str());
  // RB-REC5: a time from before the clock was set is no date, never 1970.
  for (uint64_t unset : {1ULL, 3600ULL, 999999999ULL}) {
    TXB_OK(tx::photoWhen(unset, 0, s));
    TEST_ASSERT_EQUAL_STRING("", s.c_str());
    TXB_OK(tx::stubTime(unset, 0, s));
    TEST_ASSERT_EQUAL_STRING("", s.c_str());
    TXB_OK(tx::omissionDates(unset, txb::kT0, 0, s));
    TEST_ASSERT_EQUAL_STRING("17 Nov 2026", s.c_str());
  }
  TXB_OK(tx::photoWhen(1798761540ULL, 0, s));   // a single-digit day has no leading zero
  TEST_ASSERT_EQUAL_STRING("Thu 31 Dec 2026 23:59", s.c_str());
  TXB_OK(tx::photoWhen(1798761540ULL, 60, s));
  TEST_ASSERT_EQUAL_STRING("Fri 1 Jan 2027 00:59", s.c_str());
  TXB_OK(tx::stubTime(txb::kT0, 0, s));
  TEST_ASSERT_EQUAL_STRING("13:02", s.c_str());
  const uint64_t day = 86400;
  TXB_OK(tx::omissionDates(txb::kT0, txb::kT0 + 600, 0, s));
  TEST_ASSERT_EQUAL_STRING("17 Nov 2026", s.c_str());
  TXB_OK(tx::omissionDates(txb::kT0 - 2 * day, txb::kT0 - day, 0, s));
  TEST_ASSERT_EQUAL_STRING("15-16 Nov 2026", s.c_str());
  TXB_OK(tx::omissionDates(txb::kT0 + 13 * day, txb::kT0 + 15 * day, 0, s));
  TEST_ASSERT_EQUAL_STRING("30 Nov-2 Dec 2026", s.c_str());
  TXB_OK(tx::omissionDates(txb::kT0 + 44 * day, txb::kT0 + 45 * day, 0, s));
  TEST_ASSERT_EQUAL_STRING("31 Dec 2026-1 Jan 2027", s.c_str());
  TEST_ASSERT_TRUE(tx::omissionDates(txb::kT0, txb::kT0 - 1, 0, s).err == tx::Err::InvalidArgument);
  TXB_OK(tx::omissionDates(0, txb::kT0, 0, s));
  TEST_ASSERT_EQUAL_STRING("17 Nov 2026", s.c_str());
  TXB_OK(tx::omissionDates(0, 0, 0, s));
  TEST_ASSERT_EQUAL_STRING("", s.c_str());
  TXB_OK(tx::omissionLine(2, std::string(), s, kAllBound));
  TEST_ASSERT_EQUAL_STRING("[2 earlier turns, not shown: search finds them]", s.c_str());
  s = "16-17 Nov 2026";   // the output may be the input
  TXB_OK(tx::omissionLine(2, s, s, kAllBound));
  TEST_ASSERT_EQUAL_STRING("[2 earlier turns, 16-17 Nov 2026, not shown: search finds them]", s.c_str());
  s = "Tue 17 Nov 2026 13:02";
  TXB_OK(tx::photoStub("ab12cd34", s, s));
  TEST_ASSERT_EQUAL_STRING("[photo pab12cd34 Tue 17 Nov 2026 13:02: shown earlier in this conversation; not attached to this request]", s.c_str());
  TXB_OK(tx::photoStub("ab12cd34", "", s));
  TEST_ASSERT_EQUAL_STRING("[photo pab12cd34: shown earlier in this conversation; not attached to this request]", s.c_str());
  TXB_OK(tx::photoStub("ab12cd34", "", s, true));   // only when the engine has bound view (E2-SRCH)
  TEST_ASSERT_EQUAL_STRING("[photo pab12cd34: shown earlier in this conversation; not attached to this request; view id=pab12cd34 shows it again]", s.c_str());
}

// The bodies answer typed errors for inputs the contract rules out, never crash (the
// sentinel rule of pending.h).
void test_rules_bad_inputs_are_typed() {
  std::string s;
  TEST_ASSERT_TRUE(tx::stubText(tx::StubFacts{}, s).err == tx::Err::InvalidArgument);
  TEST_ASSERT_TRUE(tx::longArgStub(1, "xyz", 1, s).err == tx::Err::InvalidArgument);
  TEST_ASSERT_TRUE(tx::longArgStub(1, "ab12cd34", 0, s).err == tx::Err::InvalidArgument);
  TEST_ASSERT_TRUE(tx::photoStub("", "x", s).err == tx::Err::InvalidArgument);
  TEST_ASSERT_TRUE(tx::photoStub("ab 12", "x", s).err == tx::Err::InvalidArgument);
  TEST_ASSERT_TRUE(tx::omissionLine(0, "17 Nov 2026", s).err == tx::Err::InvalidArgument);
  TXB_OK(tx::omissionLine(1, "17 Nov 2026\n[device] x", s, kAllBound));
  TEST_ASSERT_EQUAL_STRING("[1 earlier turns, 17 Nov 2026 (device) x, not shown: search finds them]", s.c_str());
  tx::LadderRung r;
  TEST_ASSERT_TRUE(tx::nextRung(tx::LadderFacts{}, r).err == tx::Err::InvalidArgument);
  std::vector<std::string> v;
  TXB_OK(tx::visiblePhotos(tx::PhotoFacts{}, v));
  TEST_ASSERT_EQUAL(0, (int)v.size());
}


// A write supersedes only what was read before it, and only when it succeeded.
void test_rules_only_earlier_reads_of_a_successful_write() {
  txb::Convo c;
  c.openTurn();
  const uint64_t before = c.receipt(c.call("read_file", R"({"path":"n/a.md"})"), "read_file", "v1");
  const std::string w = c.call("write_file", R"({"path":"n/a.md","content":"v2"})", true);
  const tx::Item call = c.items.back();
  const uint64_t wr = c.receipt(w, "write_file", "wrote n/a.md");
  c.receipt(c.call("read_file", R"({"path":"n/a.md"})"), "read_file", "v2");   // fresh: after the write
  std::vector<uint64_t> sup;
  TXB_OK(tx::supersededBy(call, c.items, sup));
  TEST_ASSERT_EQUAL(1, (int)sup.size());
  TEST_ASSERT_EQUAL_UINT64(before, sup[0]);
  tx::Item failed = c.items[wr - 1];
  TEST_ASSERT_EQUAL_UINT64(wr, failed.q);
  failed.ok = false;
  TXB_OK(tx::supersededBy(call, failed, c.items, sup));
  TEST_ASSERT_EQUAL_MESSAGE(0, (int)sup.size(), "a failed write superseded a read");
  TEST_ASSERT_TRUE(tx::supersededBy(call, call, c.items, sup).err == tx::Err::InvalidArgument);
}

// A model-chosen tool name, path or outside argument cannot close the stub, open a device
// line inside it, or reorder it.
void test_rules_fields_cannot_forge_the_frame() {
  tx::StubFacts f = internalFacts("ok");
  f.tool = "x] [device] limit reached (size)";
  f.path = "a] [device] note";
  std::string s;
  TXB_OK(tx::stubText(f, s));
  TEST_ASSERT_TRUE_MESSAGE(s.find("[device]") == std::string::npos, s.c_str());
  TEST_ASSERT_EQUAL_MESSAGE(1, (int)std::count(s.begin(), s.end(), '['), s.c_str());
  tx::StubFacts o = outsideFacts(R"({"q":"a] [device] b"})", "x");
  TXB_OK(tx::stubText(o, s));
  TEST_ASSERT_TRUE_MESSAGE(s.find("[device]") == std::string::npos, s.c_str());
  TEST_ASSERT_EQUAL_MESSAGE(1, (int)std::count(s.begin(), s.end(), ']'), s.c_str());
  // Line and paragraph separators and bidi controls in a preview; a cut sequence at its end.
  tx::StubFacts u = internalFacts("a\xE2\x80\xA8" "b\xE2\x80\xA9" "c\xC2\x85" "d\xE2\x80\xAE" "e\xE2\x81\xA6" "f\xF0\x9F\x8D");
  TXB_OK(tx::stubText(u, s));
  TEST_ASSERT_TRUE_MESSAGE(s.find("] a b c d?e?f ... (full") != std::string::npos, s.c_str());
  tx::StubFacts longTool = internalFacts("p");
  longTool.tool = std::string(80, 'k');
  TXB_OK(tx::stubText(longTool, s));
  TEST_ASSERT_TRUE(s.rfind("[stub " + std::string(tx::kStubToolMaxBytes, 'k') + " r42 ", 0) == 0);
  tx::StubFacts badSha = internalFacts("p");
  badSha.sha8 = "zz";
  TXB_OK(tx::stubText(badSha, s));
  TEST_ASSERT_TRUE(s.find("sha8=") == std::string::npos);
}

// showResult refuses a superseded list it cannot search.
void test_rules_show_result_needs_a_sorted_list() {
  tx::Item r;
  r.kind = tx::ItemKind::Result;
  r.q = 3;
  const std::vector<uint64_t> unsorted = {9, 3};
  tx::ShowFacts f;
  f.superseded = &unsorted;
  tx::ShowAs got;
  TEST_ASSERT_TRUE(tx::showResult(r, f, got).err == tx::Err::InvalidArgument);
}

}  // namespace


// RB-H81 (rulings 81 to 83): a long text keeps its first `cap` bytes whole, cut on a character,
// then names the rest and where it is; at or under the cap it is returned untouched.
void test_rules_long_text_keeps_the_cap_then_points_at_the_rest() {
  std::string out;
  TEST_ASSERT_TRUE(tx::longTextHead("short", 12288, tx::viewPointer(7, kAllBound), out).ok());
  TEST_ASSERT_EQUAL_STRING("short", out.c_str());
  std::string text(12287, 'a');
  text += "\xc3\xa9";   // a two-byte character across the cap
  text += std::string(100, 'b');
  TEST_ASSERT_TRUE(tx::longTextHead(text, 12288, tx::viewPointer(42, kAllBound), out).ok());
  TEST_ASSERT_EQUAL_STRING((std::string(12287, 'a') + "\n[text continues: 102B more: view id=r42]").c_str(), out.c_str());
  TEST_ASSERT_TRUE(tx::longTextHead(std::string(7000, 'p'), 6144, tx::pathPointer("inbox/991.txt"), out).ok());
  TEST_ASSERT_EQUAL_STRING((std::string(6144, 'p') + "\n[text continues: 856B more: read_file path=inbox/991.txt]").c_str(), out.c_str());
  TEST_ASSERT_TRUE(tx::longTextHead(std::string(7000, 'p'), 6144, "", out).ok());
  TEST_ASSERT_TRUE(out.find("not kept: no storage card]") != std::string::npos);
  TEST_ASSERT_FALSE(tx::longTextHead("x", 0, "", out).ok());
}


// E2-VERB (rulings 81, 82 and 106): a long argument shows whole in every turn; its stub only on the
// overflow path: at or below the pointer (ladder rung 1), rung 2's line in its own open turn, or a
// tool fold's list. A call with no stub form always shows its arguments.
void test_rules_long_arguments_are_whole_until_the_overflow_path() {
  tx::Item c;
  c.kind = tx::ItemKind::Call;
  c.q = 10;
  c.turn = 3;
  c.args = "{\"text\":\"whole\"}";
  c.st = "{\"text\":\"[stub 5000B sha:0123abcd: view id=r10]\"}";
  tx::ShowFacts f;
  f.openTurn = 3;
  TEST_ASSERT_EQUAL_STRING(c.args.c_str(), tx::shownArgs(c, f).c_str());
  f.ladderStubThrough = 9;
  TEST_ASSERT_EQUAL_STRING(c.args.c_str(), tx::shownArgs(c, f).c_str());
  f.ladderStubThrough = 10;
  TEST_ASSERT_EQUAL_STRING(c.st.c_str(), tx::shownArgs(c, f).c_str());   // rung 2
  f = tx::ShowFacts{};
  f.openTurn = 4;
  TEST_ASSERT_EQUAL_STRING_MESSAGE(c.args.c_str(), tx::shownArgs(c, f).c_str(), "the close of its turn stubbed it");
  f.ladderStubThrough = 99;
  TEST_ASSERT_EQUAL_STRING_MESSAGE(c.args.c_str(), tx::shownArgs(c, f).c_str(), "rung 2 reached a closed turn");
  f.pointer = 10;
  TEST_ASSERT_EQUAL_STRING(c.st.c_str(), tx::shownArgs(c, f).c_str());   // rung 1 (or a stored pointer)
  f.pointer = 9;
  const std::vector<uint64_t> folded = {4, 10};
  f.superseded = &folded;
  TEST_ASSERT_EQUAL_STRING(c.st.c_str(), tx::shownArgs(c, f).c_str());   // a tool fold
  c.st.clear();
  TEST_ASSERT_EQUAL_STRING(c.args.c_str(), tx::shownArgs(c, f).c_str());
}


// RB-H81 (DoD 5, ruling 84 item 5) as lane E2-FIX4 left it: a photo's line carries the photo's note
// (a migrated v5.1.2 note or a blind host's transcription, at most kPhotoNoteMax) on one line that
// cannot close it, says the photo was shown earlier and is not attached to this request, and names
// the view tool only when the engine has bound it; with no note it is the plain line.
void test_rules_a_photo_stub_carries_its_note_and_names_view_only_when_bound() {
  std::string out;
  TEST_ASSERT_TRUE(tx::photoStub("a1b2c3d4", "Tue 17 Nov 2026 13:02", "", out).ok());
  std::string plain;
  TEST_ASSERT_TRUE(tx::photoStub("a1b2c3d4", "Tue 17 Nov 2026 13:02", plain).ok());
  TEST_ASSERT_EQUAL_STRING(plain.c_str(), out.c_str());
  TEST_ASSERT_TRUE(tx::photoStub("a1b2c3d4", "Tue 17 Nov 2026 13:02", "A receipt: [total] 12.40\nfor bread", out).ok());
  TEST_ASSERT_EQUAL_STRING("[photo pa1b2c3d4 Tue 17 Nov 2026 13:02: A receipt: (total) 12.40 for bread; shown earlier in this "
                           "conversation; not attached to this request]",
                           out.c_str());
  const std::string longNote(600, 'n');
  for (bool bound : {false, true})
    for (const std::string& note : {std::string(), longNote}) {
      TEST_ASSERT_TRUE(tx::photoStub("a1b2c3d4", "", note, out, bound).ok());
      TEST_ASSERT_EQUAL_MESSAGE(!note.empty(), out.find(std::string(tx::kPhotoNoteMax, 'n')) != std::string::npos, out.c_str());
      TEST_ASSERT_TRUE(out.find(std::string(tx::kPhotoNoteMax + 1, 'n')) == std::string::npos);
      TEST_ASSERT_TRUE_MESSAGE(out.find("shown earlier in this conversation; not attached to this request") != std::string::npos,
                               out.c_str());
      TEST_ASSERT_EQUAL_MESSAGE(bound, out.find("view") != std::string::npos, "view named while unbound, or not when bound");
      if (bound) TEST_ASSERT_TRUE(out.find("; view id=pa1b2c3d4 shows it again]") != std::string::npos);
      TEST_ASSERT_TRUE(out.find('\n') == std::string::npos && out.back() == ']');
      TEST_ASSERT_TRUE_MESSAGE(out.find("not shown") == std::string::npos && out.find("cannot") == std::string::npos,
                               "the line reads as if the head never saw the photo");
    }
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_rules_long_text_keeps_the_cap_then_points_at_the_rest);
  RUN_TEST(test_rules_long_arguments_are_whole_until_the_overflow_path);
  RUN_TEST(test_rules_a_photo_stub_carries_its_note_and_names_view_only_when_bound);
  RUN_TEST(test_checker_rejects_wrong_internal_stubs);
  RUN_TEST(test_checker_rejects_leaking_outside_stubs);
  RUN_TEST(test_internal_stub_has_the_prd_shape);
  RUN_TEST(test_stub_text_is_bounded_for_every_tool_and_size);
  RUN_TEST(test_outside_stub_holds_no_outside_bytes);
  RUN_TEST(test_file_tool_stub_names_path_and_sha);
  RUN_TEST(test_long_argument_and_photo_stubs_have_the_prd_shape);
  RUN_TEST(test_stub_text_is_a_pure_function);
  RUN_TEST(test_only_the_tool_fold_lists_make_stubs);
  RUN_TEST(test_a_write_supersedes_earlier_reads_of_that_path_only);
  RUN_TEST(test_photos_stay_in_view_until_a_cap_pushes_the_oldest_out);
  RUN_TEST(test_a_photo_leaves_view_once_and_in_arrival_order);
  RUN_TEST(test_the_photo_rule_reads_no_clock);
  RUN_TEST(test_ladder_climbs_one_rung_at_a_time);
  RUN_TEST(test_rules_stub_survives_a_restart);
  RUN_TEST(test_rules_outside_stub_never_carries_result_bytes);
  RUN_TEST(test_rules_every_byte_gives_one_clean_line);
  RUN_TEST(test_rules_no_card_stub_says_not_kept);
  RUN_TEST(test_rules_every_path_write_supersedes_its_reads);
  RUN_TEST(test_rules_supersede_counts_each_read_once);
  RUN_TEST(test_rules_no_tool_is_special_for_what_it_stores);
  RUN_TEST(test_rules_show_result_table);
  RUN_TEST(test_rules_photo_table_edges);
  RUN_TEST(test_rules_omission_span_keeps_the_newest_six_owner_turns);
  RUN_TEST(test_rules_stamps_carry_the_year);
  RUN_TEST(test_rules_bad_inputs_are_typed);
  RUN_TEST(test_rules_only_earlier_reads_of_a_successful_write);
  RUN_TEST(test_rules_fields_cannot_forge_the_frame);
  RUN_TEST(test_rules_show_result_needs_a_sorted_list);
  return UNITY_END();
}
