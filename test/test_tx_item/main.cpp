#include <unity.h>

#include <cstring>
#include <string>
#include <vector>

#include "../support/tx_items/gen.h"
#include "nimbus/orch/tx/item.h"

// test_tx_item - the transcript line codec (lane RB-T1, PRD section 2): every kind round
// trips with every field set; a line is one line with a real crc32 last; every cut, flip
// and glue of a line is dropped as torn; no line passes 60,000 B; the message stamp
// carries the year (Ruling 11).
namespace tx = nimbus::orch::tx;
using txgen::Rng;

void setUp() {}
void tearDown() {}

namespace {

std::string enc(const tx::Item& it) {
  std::string line;
  const tx::Status st = tx::encodeLine(it, line);
  TEST_ASSERT_TRUE_MESSAGE(st.ok(), tx::errSlug(st.err));
  return line;
}

// An independent bitwise crc32 (IEEE), so the codec's table is checked, not trusted.
uint32_t refCrc(const std::string& s) {
  uint32_t c = 0xFFFFFFFFu;
  for (unsigned char b : s) {
    c ^= b;
    for (int k = 0; k < 8; ++k) c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1u)));
  }
  return ~c;
}

// A hand-built line with a correct tail, for decoder cases the encoder never writes.
std::string sealed(const std::string& body) {
  char tail[32];
  std::snprintf(tail, sizeof tail, ",\"z\":\"%08x\"}\n", refCrc(body));
  return body + tail;
}

std::vector<std::string> oneLinePerKind(uint64_t seed) {
  Rng r(seed);
  std::vector<std::string> lines;
  uint64_t q = 1;
  for (tx::ItemKind k : tx::kAllItemKinds) lines.push_back(enc(txgen::fullItem(k, q++, r, 60)));
  return lines;
}

}  // namespace

// Every kind, 300 random items each, every field set: the kind's fields come back exactly,
// the others are not written, and decoding then encoding gives the same bytes.
void test_every_kind_round_trips_with_every_field_set() {
  Rng r(17);
  for (tx::ItemKind k : tx::kAllItemKinds) {
    for (int i = 0; i < 300; ++i) {
      const tx::Item it = txgen::fullItem(k, r.nonzero(UINT64_MAX - 1), r);
      const std::string line = enc(it);
      tx::Item back;
      const tx::Status st = tx::decodeLine(line.data(), line.size(), back);
      TEST_ASSERT_TRUE_MESSAGE(st.ok(), (std::string(1, tx::kindLetter(k)) + ": " + tx::errSlug(st.err)).c_str());
      const std::string diff = txgen::firstDifference(txgen::project(it), back);
      TEST_ASSERT_TRUE_MESSAGE(diff.empty(), (std::string(1, tx::kindLetter(k)) + " lost field " + diff).c_str());
      TEST_ASSERT_EQUAL_STRING(line.c_str(), enc(back).c_str());
    }
  }
}

// Defaults are not written: an item with only k and q set is the shortest line, for
// every kind, and decodes to the default item.
void test_default_fields_are_not_written() {
  for (tx::ItemKind k : tx::kAllItemKinds) {
    tx::Item it;
    it.kind = k;
    it.q = 9;
    const std::string line = enc(it);
    const std::string want = std::string("{\"k\":\"") + tx::kindLetter(k) + "\",\"q\":9";
    const bool noteHasClass = k == tx::ItemKind::DeviceNote;   // the note class is always named
    if (!noteHasClass) TEST_ASSERT_EQUAL_STRING(sealed(want).c_str(), line.c_str());
    tx::Item back;
    TEST_ASSERT_TRUE(tx::decodeLine(line.data(), line.size(), back).ok());
    TEST_ASSERT_TRUE(txgen::firstDifference(it, back).empty());
  }
}

// One line: '{"k":"<letter>","q":' first, exactly one newline at the end, no raw control
// byte, and z is the IEEE crc32 of every byte before ',"z":"'.
void test_a_line_is_one_line_with_its_crc_last() {
  Rng r(5);
  for (int i = 0; i < 600; ++i) {
    const tx::ItemKind k = tx::kAllItemKinds[i % tx::kItemKindCount];
    const std::string line = enc(txgen::fullItem(k, i + 1, r));
    const std::string head = std::string("{\"k\":\"") + tx::kindLetter(k) + "\",\"q\":";
    TEST_ASSERT_EQUAL_INT(0, (int)line.compare(0, head.size(), head));
    TEST_ASSERT_EQUAL_CHAR('\n', line.back());
    for (size_t j = 0; j + 1 < line.size(); ++j) TEST_ASSERT_TRUE(static_cast<unsigned char>(line[j]) >= 0x20 && line[j] != 0x7f);
    const size_t body = line.size() - 1 - tx::kLineTailBytes;
    TEST_ASSERT_EQUAL_STRING(sealed(line.substr(0, body)).c_str(), line.c_str());
  }
}

// A torn tail: every cut of every kind's line is Torn (the line without its newline is
// still whole), for decodeLine and checkLine alike.
void test_every_cut_of_a_line_is_torn() {
  for (const std::string& line : oneLinePerKind(23)) {
    for (size_t cut = 0; cut + 1 < line.size(); ++cut) {
      tx::Item it;
      tx::LineHeader h;
      TEST_ASSERT_TRUE(tx::decodeLine(line.data(), cut, it).err == tx::Err::Torn);
      TEST_ASSERT_TRUE(tx::checkLine(line.data(), cut, h).err == tx::Err::Torn);
      const std::string withNewline = line.substr(0, cut) + "\n";   // a cut a later write ended
      TEST_ASSERT_TRUE(tx::decodeLine(withNewline.data(), withNewline.size(), it).err == tx::Err::Torn);
    }
    tx::Item it;
    TEST_ASSERT_TRUE(tx::decodeLine(line.data(), line.size() - 1, it).ok());
  }
}

// Every single-bit flip of every byte of every kind's line is Torn: nothing damaged is
// ever read as an item.
void test_every_flipped_bit_is_torn() {
  for (std::string line : oneLinePerKind(29)) {
    for (size_t j = 0; j < line.size(); ++j) {
      for (int bit = 0; bit < 8; ++bit) {
        line[j] = static_cast<char>(line[j] ^ (1 << bit));
        tx::Item it;
        const tx::Status st = tx::decodeLine(line.data(), line.size(), it);
        TEST_ASSERT_TRUE_MESSAGE(st.err == tx::Err::Torn, ("byte " + std::to_string(j)).c_str());
        line[j] = static_cast<char>(line[j] ^ (1 << bit));
      }
    }
  }
}

// A torn line glued to the next whole one (an append after a cut that left no newline)
// is never read as either item.
void test_a_glued_line_is_torn() {
  const std::vector<std::string> lines = oneLinePerKind(31);
  for (size_t i = 0; i + 1 < lines.size(); ++i) {
    for (size_t cut = 1; cut + 1 < lines[i].size(); cut += 7) {
      const std::string glued = lines[i].substr(0, cut) + lines[i + 1];
      tx::Item it;
      TEST_ASSERT_TRUE(tx::decodeLine(glued.data(), glued.size(), it).err == tx::Err::Torn);
    }
  }
}

// The ceiling: a line of exactly 60,000 B (newline included) is written; one byte more is
// refused whole, also when escaping is what makes it long; decode refuses a longer line.
void test_no_line_passes_sixty_thousand_bytes() {
  tx::Item it;
  it.kind = tx::ItemKind::Assistant;
  it.q = 42;
  it.x = "a";
  const size_t base = enc(it).size() - 1;   // the line is base + n bytes for n bytes of x
  it.x.assign(tx::kMaxLineBytes - base, 'a');
  std::string line;
  TEST_ASSERT_TRUE(tx::encodeLine(it, line).ok());
  TEST_ASSERT_EQUAL_UINT32(tx::kMaxLineBytes, line.size());
  tx::Item back;
  TEST_ASSERT_TRUE(tx::decodeLine(line.data(), line.size(), back).ok());
  it.x.push_back('a');
  const tx::Status st = tx::encodeLine(it, line);
  TEST_ASSERT_TRUE(st.err == tx::Err::TooLarge);
  TEST_ASSERT_EQUAL_UINT32(0, line.size());
  it.x.assign(tx::kMaxLineBytes / 6 + 10, '\x01');   // 10,010 raw bytes, 60,060 escaped
  TEST_ASSERT_TRUE(tx::encodeLine(it, line).err == tx::Err::TooLarge);
  const std::string longer = sealed("{\"k\":\"a\",\"q\":1,\"x\":\"" + std::string(tx::kMaxLineBytes, 'a') + "\"");
  TEST_ASSERT_TRUE(tx::decodeLine(longer.data(), longer.size(), back).err == tx::Err::Torn);
  // Every size around the ceiling, so an off-by-one cannot hide in one point.
  for (size_t n = tx::kMaxLineBytes - base - 40; n < tx::kMaxLineBytes - base + 40; ++n) {
    it.x.assign(n, 'b');
    const tx::Status s = tx::encodeLine(it, line);
    TEST_ASSERT_TRUE(s.ok() == (n + base <= tx::kMaxLineBytes));
    TEST_ASSERT_TRUE(line.size() <= tx::kMaxLineBytes);
  }
}

// The format only grows: keys a reader does not know are skipped, at any depth.
void test_unknown_keys_are_skipped() {
  const std::string line = sealed(
      R"({"k":"u","q":5,"new":{"a":[1,{"b":"]}\""}],"c":null},"x":"hi","more":[true,false,-3.5e2],"m":7)");
  tx::Item it;
  TEST_ASSERT_TRUE(tx::decodeLine(line.data(), line.size(), it).ok());
  TEST_ASSERT_EQUAL_UINT64(5, it.q);
  TEST_ASSERT_EQUAL_STRING("hi", it.x.c_str());
  TEST_ASSERT_EQUAL_UINT64(7, it.m);
}

// Whole lines the codec cannot read answer InvalidArgument, never a half item: kind not
// first, an unknown kind letter, whitespace, numbers out of range, raw control bytes,
// bad escapes, a second k.
void test_unreadable_whole_lines_are_refused() {
  const char* bodies[] = {
      R"({"q":5,"k":"u")",          R"({"k":"w","q":5)",           R"({"k": "u","q":5)",
      R"({"k":"u","q":-5)",         R"({"k":"u","q":05)",          R"({"k":"u","q":18446744073709551616)",
      R"({"k":"t","q":1,"at":256)", R"({"k":"u","q":1,"tz":40000)", "{\"k\":\"u\",\"q\":1,\"x\":\"a\nb\"",
      R"({"k":"u","q":1,"x":"\q")", R"({"k":"u","q":1,"x":"\ud800")", R"({"k":"u","q":1,"k":"u")",
      R"({"k":"uu","q":1)",         R"({"k":"u","q":1,"parts":[{"k":"gif"}])",
      R"({"k":"e","q":1,"ex":[[1,2]])",
  };
  for (const char* body : bodies) {
    const std::string line = sealed(body);
    tx::Item it;
    const tx::Status st = tx::decodeLine(line.data(), line.size(), it);
    TEST_ASSERT_TRUE_MESSAGE(st.err == tx::Err::InvalidArgument, body);
  }
}

// Escapes a reader may meet from another writer decode to the same bytes, and a result
// class the reader does not know reads as outside content (never shown whole).
void test_escapes_and_unknown_classes() {
  const std::string line =
      sealed(R"({"k":"r","q":3,"x":"\u00e9\ud83c\udf53\/\u0000","cls":"later")");
  tx::Item it;
  TEST_ASSERT_TRUE(tx::decodeLine(line.data(), line.size(), it).ok());
  TEST_ASSERT_TRUE(it.x == std::string("\xc3\xa9\xf0\x9f\x8d\x93/\0", 8));
  TEST_ASSERT_TRUE(it.cls == tx::ResultClass::Outside);
  const std::string note = sealed(R"({"k":"h","q":4,"cls":"newer")");
  TEST_ASSERT_TRUE(tx::decodeLine(note.data(), note.size(), it).ok());
  TEST_ASSERT_TRUE(it.note == tx::NoteClass::Unknown);
}

// checkLine reads k, q, turn and t without decoding, and agrees with decodeLine on every kind.
void test_check_line_agrees_with_decode() {
  Rng r(41);
  for (int i = 0; i < 240; ++i) {
    const tx::Item it = txgen::fullItem(tx::kAllItemKinds[i % tx::kItemKindCount], r.next(), r);
    const std::string line = enc(it);
    tx::LineHeader h;
    TEST_ASSERT_TRUE(tx::checkLine(line.data(), line.size(), h).ok());
    TEST_ASSERT_EQUAL_UINT64(it.q, h.q);
    TEST_ASSERT_EQUAL_UINT64(it.t, h.t);
    TEST_ASSERT_EQUAL_UINT32(it.turn, h.turn);
    TEST_ASSERT_TRUE(it.kind == h.kind);
  }
}

// The typed errors a sentinel and a careless caller meet.
void test_typed_errors() {
  tx::Item it;
  std::string s = "keep";
  TEST_ASSERT_TRUE(tx::decodeLine("{}", 2, it).err == tx::Err::Torn);
  TEST_ASSERT_TRUE(tx::decodeLine(nullptr, 0, it).err == tx::Err::InvalidArgument);
  it.kind = static_cast<tx::ItemKind>('w');
  TEST_ASSERT_TRUE(tx::encodeLine(it, s).err == tx::Err::InvalidArgument);
  TEST_ASSERT_EQUAL_UINT32(0, s.size());
  tx::LineHeader h;
  TEST_ASSERT_TRUE(tx::checkLine(nullptr, 0, h).err == tx::Err::InvalidArgument);
}

// Ruling 11: every stamp carries the year, at the owner's offset, across a year end, a
// leap day and both signs of the offset.
void test_message_stamp_carries_the_year() {
  struct Row {
    uint64_t epoch;
    int16_t tz;
    const char* want;
  } rows[] = {
      {1794920520ULL, 0, "[Tue 17 Nov 2026 13:02]"},
      {1794920520ULL, 60, "[Tue 17 Nov 2026 14:02]"},
      {1798761540ULL, 0, "[Thu 31 Dec 2026 23:59]"},
      {1798761540ULL, 60, "[Fri 1 Jan 2027 00:59]"},
      {1798761540ULL, -720, "[Thu 31 Dec 2026 11:59]"},
      {1835395200ULL, 0, "[Tue 29 Feb 2028 00:00]"},
      {1ULL, -60, "[Wed 31 Dec 1969 23:00]"},
  };
  for (const Row& row : rows) {
    char out[tx::kStampMaxBytes];
    TEST_ASSERT_TRUE(tx::messageStamp(row.epoch, row.tz, out, sizeof out).ok());
    TEST_ASSERT_EQUAL_STRING(row.want, out);
  }
  char out[tx::kStampMaxBytes];
  TEST_ASSERT_TRUE(tx::messageStamp(0, 0, out, sizeof out).err == tx::Err::InvalidArgument);
  TEST_ASSERT_TRUE(tx::messageStamp(1, 0, out, 8).err == tx::Err::InvalidArgument);
}

// The stamp against an independent calendar walk, every day of 1970 to 2100 at noon.
void test_message_stamp_matches_a_calendar_walk() {
  static const char* days[] = {"Thu", "Fri", "Sat", "Sun", "Mon", "Tue", "Wed"};
  static const char* months[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
  uint64_t day = 0;
  for (int y = 1970; y <= 2100; ++y) {
    const bool leap = (y % 4 == 0 && y % 100 != 0) || y % 400 == 0;
    const int lengths[] = {31, leap ? 29 : 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    for (int m = 0; m < 12; ++m) {
      for (int d = 1; d <= lengths[m]; ++d, ++day) {
        char want[40], out[tx::kStampMaxBytes];
        std::snprintf(want, sizeof want, "[%s %d %s %d 12:00]", days[day % 7], d, months[m], y);
        TEST_ASSERT_TRUE(tx::messageStamp(day * 86400 + 43200, 0, out, sizeof out).ok());
        TEST_ASSERT_EQUAL_STRING(want, out);
      }
    }
  }
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_every_kind_round_trips_with_every_field_set);
  RUN_TEST(test_default_fields_are_not_written);
  RUN_TEST(test_a_line_is_one_line_with_its_crc_last);
  RUN_TEST(test_every_cut_of_a_line_is_torn);
  RUN_TEST(test_every_flipped_bit_is_torn);
  RUN_TEST(test_a_glued_line_is_torn);
  RUN_TEST(test_no_line_passes_sixty_thousand_bytes);
  RUN_TEST(test_unknown_keys_are_skipped);
  RUN_TEST(test_unreadable_whole_lines_are_refused);
  RUN_TEST(test_escapes_and_unknown_classes);
  RUN_TEST(test_check_line_agrees_with_decode);
  RUN_TEST(test_typed_errors);
  RUN_TEST(test_message_stamp_carries_the_year);
  RUN_TEST(test_message_stamp_matches_a_calendar_walk);
  return UNITY_END();
}
