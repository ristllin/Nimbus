#include <unity.h>

#include <cstdio>
#include <cstdlib>
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

// Decoding into a reused item (a renderer keeps one): nothing of the previous item stays,
// whatever kind either was.
void test_decoding_into_a_used_item_leaves_nothing_behind() {
  Rng r(43);
  tx::Item reused = txgen::fullItem(tx::ItemKind::TurnEnd, 1, r);
  for (int i = 0; i < 120; ++i) {
    const tx::Item it = txgen::fullItem(tx::kAllItemKinds[(i * 5) % tx::kItemKindCount], r.next(), r);
    const std::string line = enc(it);
    TEST_ASSERT_TRUE(tx::decodeLine(line.data(), line.size(), reused).ok());
    const std::string diff = txgen::firstDifference(txgen::project(it), reused);
    TEST_ASSERT_TRUE_MESSAGE(diff.empty(), diff.c_str());
    reused = txgen::fullItem(tx::kAllItemKinds[i % tx::kItemKindCount], 1, r);   // every field dirty again
    TEST_ASSERT_TRUE(tx::decodeLine(line.data(), line.size(), reused).ok());
    TEST_ASSERT_TRUE_MESSAGE(txgen::firstDifference(txgen::project(it), reused).empty(), "dirty reuse");
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

// q starts at 1 and is always there; k comes first even behind a key too long to name or
// a key holding a NUL; a field another kind writes is skipped, never read into this item.
void test_header_rules_and_foreign_fields() {
  const char* refused[] = {
      R"({"k":"u")",
      R"({"k":"u","q":0)",
      R"({"longkey1":1,"k":"u","q":1)",
      "{\"k\\u0000zz\":\"u\",\"q\":1",
      R"({"k":"u","q":1,"x":"a","k":"a")",
  };
  for (const char* body : refused) {
    const std::string line = sealed(body);
    tx::Item it;
    tx::LineHeader h;
    TEST_ASSERT_TRUE_MESSAGE(tx::decodeLine(line.data(), line.size(), it).err == tx::Err::InvalidArgument, body);
    TEST_ASSERT_TRUE_MESSAGE(tx::checkLine(line.data(), line.size(), h).err == tx::Err::InvalidArgument, body);
  }
  const std::string line =
      sealed(R"({"k":"u","q":1,"usd":5,"ph":"x","ok":false,"cls":"bulk","fin":true,"rd":3,"ids":[1],"x":"hi")");
  tx::Item it;
  TEST_ASSERT_TRUE(tx::decodeLine(line.data(), line.size(), it).ok());
  tx::Item want;
  want.q = 1;
  want.x = "hi";
  TEST_ASSERT_EQUAL_STRING("", txgen::firstDifference(want, it).c_str());
  tx::Item zero;
  zero.kind = tx::ItemKind::User;
  std::string out = "keep";
  TEST_ASSERT_TRUE(tx::encodeLine(zero, out).err == tx::Err::InvalidArgument);
  TEST_ASSERT_EQUAL_UINT32(0, out.size());
}

// Lists hold at most kMaxListItems values, written and read alike, for every list field:
// a hostile line cannot make the decoder build thousands of values.
void test_lists_are_capped_both_ways() {
  using Fill = void (*)(tx::Item&, size_t);
  struct Row {
    tx::ItemKind kind;
    Fill fill;
    const char* element;   // one more value of the list, as JSON
  } rows[] = {
      {tx::ItemKind::User, [](tx::Item& it, size_t n) { it.src.assign(n, 7); }, "7"},
      {tx::ItemKind::User, [](tx::Item& it, size_t n) { it.parts.assign(n, tx::Part{tx::PartKind::File, "a/b", "", 1}); },
       R"({"k":"file","ref":"a/b","b":1})"},
      {tx::ItemKind::TurnOpen, [](tx::Item& it, size_t n) { it.photos.assign(n, "p1"); }, R"("p1")"},
      {tx::ItemKind::PointerMove, [](tx::Item& it, size_t n) { it.sup.assign(n, 3); }, "3"},
      {tx::ItemKind::Delivery, [](tx::Item& it, size_t n) { it.msgIds.assign(n, -1); }, "-1"},
      {tx::ItemKind::TurnEnd, [](tx::Item& it, size_t n) { it.end.exchanges.assign(n, tx::ExchangeFacts{1, 1, 200}); },
       "[1,1,200]"},
      {tx::ItemKind::TurnEnd, [](tx::Item& it, size_t n) { it.end.events.assign(n, "e"); }, R"("e")"},
  };
  for (const Row& row : rows) {
    tx::Item it;
    it.kind = row.kind;
    it.q = 1;
    row.fill(it, tx::kMaxListItems);
    const std::string atCap = enc(it);
    tx::Item back;
    TEST_ASSERT_TRUE(tx::decodeLine(atCap.data(), atCap.size(), back).ok());
    TEST_ASSERT_EQUAL_STRING("", txgen::firstDifference(it, back).c_str());
    row.fill(it, tx::kMaxListItems + 1);
    std::string over;
    TEST_ASSERT_TRUE(tx::encodeLine(it, over).err == tx::Err::InvalidArgument);
    // The same list one value longer, sealed by another writer: refused on read.
    std::string body = atCap.substr(0, atCap.size() - 1 - tx::kLineTailBytes);
    body.insert(body.find('[') + 1, std::string(row.element) + ",");
    const std::string longer = sealed(body);
    TEST_ASSERT_TRUE_MESSAGE(tx::decodeLine(longer.data(), longer.size(), back).err == tx::Err::InvalidArgument,
                             row.element);
  }
}

// Unknown values are skipped without recursion up to 64 levels; deeper, or with brackets
// that do not match, the line is refused; nothing crashes on 59,000 open brackets.
void test_nesting_is_bounded_and_matched() {
  tx::Item it;
  const std::string ok64 = sealed("{\"k\":\"u\",\"q\":1,\"deep\":" + std::string(64, '[') + std::string(64, ']'));
  TEST_ASSERT_TRUE(tx::decodeLine(ok64.data(), ok64.size(), it).ok());
  const std::string deep65 = sealed("{\"k\":\"u\",\"q\":1,\"deep\":" + std::string(65, '[') + std::string(65, ']'));
  TEST_ASSERT_TRUE(tx::decodeLine(deep65.data(), deep65.size(), it).err == tx::Err::InvalidArgument);
  const std::string open = sealed("{\"k\":\"u\",\"q\":1,\"deep\":" + std::string(59000, '['));
  TEST_ASSERT_TRUE(tx::decodeLine(open.data(), open.size(), it).err == tx::Err::InvalidArgument);
  // A skipped value is checked for its strings and matching brackets, not its full grammar.
  const char* bad[] = {R"({"k":"u","q":1,"w":[})", R"({"k":"u","q":1,"w":{"a":1])", R"({"k":"u","q":1,"w":})",
                       R"({"k":"u","q":1,"w":["a)"};
  for (const char* body : bad) {
    const std::string line = sealed(body);
    TEST_ASSERT_TRUE_MESSAGE(tx::decodeLine(line.data(), line.size(), it).err == tx::Err::InvalidArgument, body);
  }
}

// The edges of every number type come back exactly.
void test_number_edges_round_trip() {
  tx::Item it;
  it.kind = tx::ItemKind::Delivery;
  it.q = UINT64_MAX;
  it.t = UINT64_MAX;
  it.turn = UINT32_MAX;
  it.msgIds = {INT64_MIN, INT64_MAX, -1, 1};
  tx::Item back;
  std::string line = enc(it);
  TEST_ASSERT_TRUE(tx::decodeLine(line.data(), line.size(), back).ok());
  TEST_ASSERT_EQUAL_STRING("", txgen::firstDifference(it, back).c_str());
  tx::Item u;
  u.kind = tx::ItemKind::User;
  u.q = 1;
  u.tzMinutes = INT16_MIN;
  u.src = {UINT64_MAX};
  line = enc(u);
  TEST_ASSERT_TRUE(tx::decodeLine(line.data(), line.size(), back).ok());
  TEST_ASSERT_EQUAL_STRING("", txgen::firstDifference(u, back).c_str());
  const std::string minusZero = sealed(R"({"k":"d","q":1,"ids":[-0])");
  TEST_ASSERT_TRUE(tx::decodeLine(minusZero.data(), minusZero.size(), back).ok());
  TEST_ASSERT_EQUAL_INT64(0, back.msgIds.at(0));
}

// Hostile lines that pass the crc (another writer, a crc collision): 20,000 mutations of
// real lines, each resealed and read from an exact-size heap copy. Every one answers ok or
// InvalidArgument; what decodes re-encodes to a line that decodes to the same item.
void test_resealed_mutations_never_break_the_reader() {
  static const char* tokens[] = {"\"", "\\", "{", "}", "[", "]", ",", ":", "0", "-", "1e9", "null", "true",
                                 "\"k\"", "\"q\"", "\\u", "\\ud800", "99999999999999999999", "\"x\":", "\x01"};
  Rng r(47);
  const std::vector<std::string> seeds = oneLinePerKind(53);
  int decoded = 0;
  for (int i = 0; i < 20000; ++i) {
    const std::string& seed = seeds[r.below(seeds.size())];
    std::string body = seed.substr(0, seed.size() - 1 - tx::kLineTailBytes);
    for (int m = 0, n = 1 + (int)r.below(3); m < n && !body.empty(); ++m) {
      const size_t at = r.below(body.size());
      const std::string tok = tokens[r.below(sizeof tokens / sizeof tokens[0])];
      switch (r.below(3)) {
        case 0: body.replace(at, 1 + r.below(4), tok); break;
        case 1: body.insert(at, tok); break;
        default: body.erase(at, 1 + r.below(8)); break;
      }
    }
    const std::string line = sealed(body);
    char* exact = static_cast<char*>(std::malloc(line.size()));
    std::memcpy(exact, line.data(), line.size());
    tx::Item it;
    tx::LineHeader h;
    const tx::Status st = tx::decodeLine(exact, line.size(), it);
    const tx::Status hs = tx::checkLine(exact, line.size(), h);
    std::free(exact);
    TEST_ASSERT_TRUE_MESSAGE(st.ok() || st.err == tx::Err::InvalidArgument, body.c_str());
    TEST_ASSERT_TRUE(hs.ok() || hs.err == tx::Err::InvalidArgument);
    if (!st.ok()) continue;
    ++decoded;
    TEST_ASSERT_TRUE(hs.ok() && h.q == it.q && h.kind == it.kind);
    std::string again;
    TEST_ASSERT_TRUE(tx::encodeLine(it, again).ok() || it.q == 0);
    tx::Item twice;
    TEST_ASSERT_TRUE(tx::decodeLine(again.data(), again.size(), twice).ok());
    TEST_ASSERT_EQUAL_STRING("", txgen::firstDifference(it, twice).c_str());
  }
  char msg[96];
  std::snprintf(msg, sizeof msg, "%d of 20000 resealed mutations still decode; the rest are refused", decoded);
  TEST_MESSAGE(msg);
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

// Ruling 11 and RB-REC5: every stamp carries the year and the weekday, at the owner's offset,
// across a year end, a leap day and both signs of the offset; before the day start it names
// the day still running; with no clock it says so (orch::day::stamp in brackets; the shared
// function's own suite is test_stamp_day).
void test_message_stamp_carries_the_year() {
  struct Row {
    uint64_t epoch;
    int16_t tz;
    uint16_t start;
    const char* want;
  } rows[] = {
      {1794920520ULL, 0, 240, "[2026-11-17 Tue 13:02]"},
      {1794920520ULL, 60, 240, "[2026-11-17 Tue 14:02]"},
      {1798761540ULL, 0, 240, "[2026-12-31 Thu 23:59]"},
      {1798761540ULL, 60, 240, "[2027-01-01 Fri 00:59 (day 2026-12-31 until 04:00)]"},
      {1798761540ULL, 60, 0, "[2027-01-01 Fri 00:59]"},
      {1798761540ULL, -720, 240, "[2026-12-31 Thu 11:59]"},
      {1835395200ULL, 0, 240, "[2028-02-29 Tue 00:00 (day 2028-02-28 until 04:00)]"},
      {1ULL, -60, 240, "[time unknown]"},
      {0ULL, 0, 240, "[time unknown]"},
  };
  for (const Row& row : rows) {
    char out[tx::kStampMaxBytes];
    TEST_ASSERT_TRUE(tx::messageStamp(row.epoch, row.tz, row.start, out, sizeof out).ok());
    TEST_ASSERT_EQUAL_STRING(row.want, out);
  }
  char out[tx::kStampMaxBytes] = "x";
  TEST_ASSERT_TRUE(tx::messageStamp(1794920520ULL, 0, 240, out, 8).err == tx::Err::InvalidArgument);
  TEST_ASSERT_TRUE(tx::messageStamp(1794920520ULL, 0, 240, nullptr, 64).err == tx::Err::InvalidArgument);
  TEST_ASSERT_TRUE(tx::messageStamp(1794920520ULL, 1200, 240, out, sizeof out).err == tx::Err::InvalidArgument);
  TEST_ASSERT_EQUAL_STRING("", out);
  // The longest stamp fills the buffer exactly.
  TEST_ASSERT_TRUE(tx::messageStamp(1798761540ULL, 60, 240, out, sizeof out).ok());
  TEST_ASSERT_EQUAL_UINT32(tx::kStampMaxBytes - 1, std::strlen(out));
}

// The stamp against an independent calendar walk, every day of 2002 to 2100 at noon.
void test_message_stamp_matches_a_calendar_walk() {
  static const char* days[] = {"Thu", "Fri", "Sat", "Sun", "Mon", "Tue", "Wed"};
  uint64_t day = 0;
  for (int y = 1970; y <= 2100; ++y) {
    const bool leap = (y % 4 == 0 && y % 100 != 0) || y % 400 == 0;
    const int lengths[] = {31, leap ? 29 : 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    for (int m = 0; m < 12; ++m) {
      for (int d = 1; d <= lengths[m]; ++d, ++day) {
        if (y < 2002) continue;
        char want[40], out[tx::kStampMaxBytes];
        std::snprintf(want, sizeof want, "[%04d-%02d-%02d %s 12:00]", y, m + 1, d, days[day % 7]);
        TEST_ASSERT_TRUE(tx::messageStamp(day * 86400 + 43200, 0, 240, out, sizeof out).ok());
        TEST_ASSERT_EQUAL_STRING(want, out);
      }
    }
  }
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_every_kind_round_trips_with_every_field_set);
  RUN_TEST(test_decoding_into_a_used_item_leaves_nothing_behind);
  RUN_TEST(test_default_fields_are_not_written);
  RUN_TEST(test_a_line_is_one_line_with_its_crc_last);
  RUN_TEST(test_every_cut_of_a_line_is_torn);
  RUN_TEST(test_every_flipped_bit_is_torn);
  RUN_TEST(test_a_glued_line_is_torn);
  RUN_TEST(test_no_line_passes_sixty_thousand_bytes);
  RUN_TEST(test_unknown_keys_are_skipped);
  RUN_TEST(test_unreadable_whole_lines_are_refused);
  RUN_TEST(test_escapes_and_unknown_classes);
  RUN_TEST(test_header_rules_and_foreign_fields);
  RUN_TEST(test_lists_are_capped_both_ways);
  RUN_TEST(test_nesting_is_bounded_and_matched);
  RUN_TEST(test_number_edges_round_trip);
  RUN_TEST(test_resealed_mutations_never_break_the_reader);
  RUN_TEST(test_check_line_agrees_with_decode);
  RUN_TEST(test_typed_errors);
  RUN_TEST(test_message_stamp_carries_the_year);
  RUN_TEST(test_message_stamp_matches_a_calendar_walk);
  return UNITY_END();
}
