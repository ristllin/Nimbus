// The old engine's files.read and files.edit on the shared aids (file_aids_e1.h).
//
// Class rules proven here:
//   - the shared fixtures (test/support/file_aids_e1_cases.h), run the way the device's
//     files_subsystem.cpp runs them, give test/golden/file_aids_e1_results.txt byte for byte;
//     nimbusd's PosixFiles tools must give the same bytes (test_hosted_file_aids);
//   - files.read totals nothing (ruling 95) and a selection is a plain read of its rows (ruling
//     97): a day's read of owner-shaped files in three equal domains (a food log, an expense
//     log, a training log: comment lines, rows with more fields than the header, empty cells, a
//     stray blank line, a free-text key) is the plain read's head, the header and every row of
//     that day as stored, and no other, and never a figure;
//   - files.edit's default stays the first match; count, all and line pick as documented;
//     every result names the current lines.
#include <unity.h>

#include <cstdio>
#include <cstdlib>
#include <map>
#include <string>

#include "../support/fake_aid_card.h"
#include "../support/file_aids_e1_cases.h"

using namespace nimbus::orch;
using aidcases::e1::MemCard;
using aidfake::Rng;

void setUp() {}
void tearDown() {}

namespace {

std::string readFile(const char* path) {
  std::string out;
  FILE* f = std::fopen(path, "rb");
  if (!f) return out;
  char buf[4096];
  for (size_t n; (n = std::fread(buf, 1, sizeof buf, f)) > 0;) out.append(buf, n);
  std::fclose(f);
  return out;
}

struct Shape {
  const char* header;
  const char* col;
  std::string (*row)(Rng&, const std::string& date);
};
std::string dateOf(int day) {
  char b[16];
  std::snprintf(b, sizeof b, "2026-11-%02d", 1 + day % 28);
  return b;
}
const Shape kShapes[] = {
    {"date,occasion,item,grams,kcal", "kcal",
     [](Rng& g, const std::string& d) {
       static const char* items[] = {"oats", "\"rice, beans\"", "toast, butter", "apple"};
       const std::string kcal = g.chance(3) ? "" : std::to_string(g.below(900));
       return d + ",lunch " + std::to_string(g.below(3)) + "," + items[g.below(4)] + "," + std::to_string(g.below(400)) + "," + kcal;
     }},
    {"date,when,note,amount,tip", "amount",
     [](Rng& g, const std::string& d) {
       static const char* notes[] = {"grocer", "\"cafe, two cups\"", "cafe, cake", "rail"};
       const std::string amt = g.chance(3) ? "" : std::to_string(g.below(90)) + "." + std::to_string(g.below(100));
       return d + ",errand " + std::to_string(g.below(3)) + "," + notes[g.below(4)] + "," + amt + ",0";
     }},
    {"date,session,exercise,reps,kg", "kg",
     [](Rng& g, const std::string& d) {
       static const char* ex[] = {"squat", "\"rows, cable\"", "rows, cable, slow", "bench"};
       const std::string kg = g.chance(3) ? "" : std::to_string(40 + g.below(140)) + ".5";
       return d + ",set " + std::to_string(g.below(3)) + "," + ex[g.below(4)] + "," + std::to_string(g.below(12)) + "," + kg;
     }},
};

}  // namespace

static void test_shared_fixtures_match_the_golden_transcript() {
  std::map<std::string, MemCard> cards;
  aids::e1::RemovedRows memory;
  const std::string got = aidcases::e1::transcript(
      [&](const aidcases::e1::Call& c, const std::string& path) {
        MemCard& card = cards[path];
        if (c.seed) card.file = c.seed;
        return aidcases::e1::deviceCall(card, path, c, memory);
      },
      [&](const std::string& path) { return cards[path].file; });
  const char* golden = "test/golden/file_aids_e1_results.txt";
  if (std::getenv("GOLDEN_UPDATE")) {
    FILE* f = std::fopen(golden, "wb");
    std::fwrite(got.data(), 1, got.size(), f);
    std::fclose(f);
  }
  TEST_ASSERT_EQUAL_STRING_MESSAGE(readFile(golden).c_str(), got.c_str(),
                                   "GOLDEN_UPDATE=1 pio test -e native -f test_file_aids_e1, then read the diff");
}

static void test_a_days_read_shows_every_row_of_the_day_and_no_figure() {
  size_t days = 0;
  for (const Shape& s : kShapes) {
    Rng g(31 + s.col[0]);
    MemCard card;
    card.file = std::string(s.header) + "\n";
    for (int i = 0; i < 300; ++i) {
      if (g.chance(13)) card.file += "#was: " + s.row(g, dateOf(i / 11)) + "\n";
      else if (g.chance(1)) card.file += "\n";
      else card.file += s.row(g, dateOf(i / 11)) + "\n";
    }
    for (int d = 0; d < 28; ++d) {
      const std::string day = dateOf(d);
      aids::CsvQuery q;
      q.column = "date";
      q.from = day;
      std::string now;
      TEST_ASSERT_TRUE(aids::e1::tableRead(card, card.file.size(), "log/x.csv", q, now));
      TEST_ASSERT_TRUE_MESSAGE(now.find("sum") == std::string::npos && now.find("total") == std::string::npos, now.c_str());
      // Ruling 97: after the plain read's head, exactly the header and every stored row of the
      // day as stored, in file order (a row of another field count among them); nothing else.
      std::string want = std::string(s.header) + "\n";
      for (size_t at = card.file.find('\n') + 1, nl; (nl = card.file.find('\n', at)) != std::string::npos; at = nl + 1)
        if (card.file.compare(at, day.size(), day) == 0) want += card.file.substr(at, nl + 1 - at);
      const std::string head = "bytes 0-" + std::to_string(card.file.size()) + " of " + std::to_string(card.file.size()) +
                               ", lines 1-301 of 301; end of file\n";
      TEST_ASSERT_EQUAL_STRING_MESSAGE((head + want).c_str(), now.c_str(), day.c_str());
      ++days;
    }
  }
  TEST_ASSERT_EQUAL(84, days);
}

static void test_files_edit_keeps_its_default_and_picks_by_line() {
  MemCard card;
  aids::e1::RemovedRows memory;
  card.file = "date,amount\n2026-11-19,5\n2026-11-19,5\n2026-11-20,6\n";
  aids::e1::EditCall c;
  c.project = "log";
  c.name = "x.txt";   // not a CSV name: no keyed report, no history rows
  c.find = "2026-11-19,5";
  c.replace = "2026-11-19,7";
  std::string reply;
  TEST_ASSERT_TRUE(aids::e1::edit(card, card, card.file.size(), c, memory, reply));
  TEST_ASSERT_EQUAL_STRING("date,amount\n2026-11-19,7\n2026-11-19,5\n2026-11-20,6\n", card.file.c_str());
  TEST_ASSERT_EQUAL_STRING(
      "edited log/x.txt: replaced 1 of 2 matches (now 51 bytes); 1 more remain (all: true replaces them); at line 2; 4 lines; "
      "find is still at line 3",
      reply.c_str());
  c.replace = "";
  c.line = 3;
  TEST_ASSERT_TRUE(aids::e1::edit(card, card, card.file.size(), c, memory, reply));
  TEST_ASSERT_EQUAL_STRING("date,amount\n2026-11-19,7\n2026-11-20,6\n", card.file.c_str());
  c.line = 3;
  TEST_ASSERT_FALSE(aids::e1::edit(card, card, card.file.size(), c, memory, reply));
  TEST_ASSERT_EQUAL_STRING("find is not in log/x.txt (38 B)", reply.c_str());
}

// Review cases: a source that ends short of the size it was given is a card fault, never a
// shorter file; history rows ride after the last row on a line of their own (a file with no
// last newline gains one) and never count as the find text still in the file.
static void test_short_reads_and_trailers() {
  MemCard card;
  card.file = "date,amount\n2026-11-19,7\n";
  std::string out;
  aids::CsvQuery q;
  q.column = "date";
  q.from = "2026-11-19";
  TEST_ASSERT_FALSE(aids::e1::tableRead(card, card.file.size() + 100, "log/x.csv", q, out));
  TEST_ASSERT_TRUE_MESSAGE(aids::e1::tableRead(card, card.file.size(), "log/x.csv", q, out), out.c_str());

  card.file = "a\nb";
  aids::EditRequest e;
  e.oldText = "b";
  e.newText = "c";
  e.trailer = "#was: b\n";
  const aids::EditOutcome o = aids::editFile(card, card, card.file.size(), 0, "log/x.txt", e);
  TEST_ASSERT_TRUE_MESSAGE(o.ok, o.error.c_str());
  TEST_ASSERT_EQUAL_STRING("a\nc\n#was: b\n", card.file.c_str());
  TEST_ASSERT_EQUAL_UINT64(card.file.size(), o.newSize);
  TEST_ASSERT_EQUAL_size_t(0, o.stillAt.count);
}

// RB-TR1: files.read's plain form can be continued to the whole file. Every file of the class
// (test/support/file_aids_e1_cases.h partFiles), at four result caps: following the next part
// each head names gives the file byte for byte, each result fits the cap, the line numbers are
// true. Counter-tests: the read nimbusd gave on 19 Nov (the start of the file whatever the
// offset, cut at 4 KB) and a head that names no next part both fail the same check.
static void test_a_plain_read_can_be_continued_to_the_whole_file() {
  for (const auto& f : aidcases::e1::partFiles()) {
    MemCard card;
    card.file = f.second;
    for (size_t cap : {size_t(512), size_t(4096), size_t(8192), size_t(16384)}) {
      auto read = [&](uint64_t off) {
        std::string out;
        TEST_ASSERT_TRUE(aids::e1::readPart(card, card.file.size(), off, cap, out));
        return out;
      };
      const std::string why = aidcases::e1::followParts(read, f.second, cap);
      TEST_ASSERT_TRUE_MESSAGE(why.empty(), (f.first + " at cap " + std::to_string(cap) + ": " + why).c_str());
    }
  }
  const std::string text = aidcases::e1::partFiles()[0].second;
  auto startOnly = [&](uint64_t) { return "bytes 0-4096 of " + std::to_string(text.size()) + "; next part: \"offset\":4096\n" + text.substr(0, 4096); };
  TEST_ASSERT_FALSE(aidcases::e1::followParts(startOnly, text, 8192).empty());
  auto noNext = [&](uint64_t off) { return "bytes " + std::to_string(off) + "-" + std::to_string(off + 8000) + " of " + std::to_string(text.size()) + "\n" + text.substr(off, 8000); };
  TEST_ASSERT_FALSE(aidcases::e1::followParts(noNext, text, 8192).empty());
}

// RB-TR1: an offset inside a character starts at the next one; past the end is said; a read
// short of the stored size is the card fault, never a shorter file.
static void test_a_part_starts_on_a_character_and_says_the_end() {
  MemCard card;
  card.file = "ab\xe2\x82\xac" "cd\n";
  std::string out;
  TEST_ASSERT_TRUE(aids::e1::readPart(card, card.file.size(), 3, 8192, out));
  TEST_ASSERT_EQUAL_STRING("bytes 5-8 of 8 (\"offset\":3 was inside a character), lines 1-1 of 1; end of file\ncd\n",
                           out.c_str());
  TEST_ASSERT_TRUE(aids::e1::readPart(card, card.file.size(), 99, 8192, out));
  TEST_ASSERT_EQUAL_STRING("bytes 8-8 of 8; end of file\n", out.c_str());
  // A run of bytes that are no character: every part still moves on, and the read ends.
  MemCard junk;
  junk.file = std::string(20000, '\x80') + "end\n";
  uint64_t at = 0;
  int calls = 0;
  for (; calls < 100; ++calls) {
    TEST_ASSERT_TRUE(aids::e1::readPart(junk, junk.file.size(), at, 4096, out));
    const size_t next = out.find("next part: \"offset\":");
    if (next == std::string::npos) break;
    const uint64_t to = std::stoull(out.substr(next + 20));
    TEST_ASSERT_TRUE(to > at);
    at = to;
  }
  TEST_ASSERT_TRUE(calls < 10);   // 20 KB at 4 KB a part
  // The offset argument never wraps: negative, huge and missing are 0, 2^53 and 0.
  ArduinoJson::JsonDocument d;
  d["offset"] = -5;
  TEST_ASSERT_EQUAL_UINT64(0, aids::e1::offsetArg(d.as<ArduinoJson::JsonObjectConst>()));
  d["offset"] = 1e30;
  TEST_ASSERT_EQUAL_UINT64(9007199254740992ull, aids::e1::offsetArg(d.as<ArduinoJson::JsonObjectConst>()));
  d["offset"] = 12345;
  TEST_ASSERT_EQUAL_UINT64(12345, aids::e1::offsetArg(d.as<ArduinoJson::JsonObjectConst>()));
  TEST_ASSERT_FALSE(aids::e1::readPart(card, card.file.size() + 10, 0, 8192, out));
  TEST_ASSERT_EQUAL_STRING("the card read failed; nothing was shown", out.c_str());
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_shared_fixtures_match_the_golden_transcript);
  RUN_TEST(test_a_days_read_shows_every_row_of_the_day_and_no_figure);
  RUN_TEST(test_files_edit_keeps_its_default_and_picks_by_line);
  RUN_TEST(test_short_reads_and_trailers);
  RUN_TEST(test_a_plain_read_can_be_continued_to_the_whole_file);
  RUN_TEST(test_a_part_starts_on_a_character_and_says_the_end);
  return UNITY_END();
}
