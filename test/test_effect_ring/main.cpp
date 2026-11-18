// test_effect_ring - each chat's stored ring of recent effects (lane RB-FX, PRD section 8 rail 8).
//
// Properties, each over the class, not one instance:
//   - 50 entries per chat, oldest dropped, order kept, at every count up to 3 x 50;
//   - every tool name and receipt is cleaned: one line, no control character, valid UTF-8,
//     at most 160 B (48 B for a tool), clipped on a UTF-8 boundary, for every byte value and
//     for random inputs;
//   - one append per effect: exactly one TxFiles::append of one whole line per append();
//   - a cut at any point of any append (every byte length) or of a rewrite loses at most
//     that entry, a torn tail is dropped at load and never glued onto the next entry;
//   - the ring survives a restart, and a file never grows past the rewrite bound;
//   - with no card (no storage, or storage refusing) the ring runs from RAM and says so;
//   - chats never see each other's entries, through evictions and restarts, and the chat
//     key is injective;
//   - the RAM rings come from the working (PSRAM) allocator and are given back.
// Storage is RB-T2's fault-injecting fake (test/support/tx_battery/mem_files.h).
#include <unity.h>

#include <algorithm>
#include <climits>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <map>
#include <new>
#include <random>
#include <set>
#include <string>
#include <vector>

#include "../support/tx_battery/mem_files.h"
#include "nimbus/orch/effect_ring.h"
#include "nimbus/orch/psram_alloc.h"

namespace fx = nimbus::orch::fx;
namespace tx = nimbus::orch::tx;

void setUp() {}
void tearDown() {}

// ---- refused allocations (ruling 30) ---------------------------------------------------------
// Every allocation of the test binary (plain new and the working allocator) counts while
// armed; the one with index gFailAt is refused (plain new throws std::bad_alloc, the working
// allocator answers nullptr, which WorkingAllocator turns into std::bad_alloc).
static bool gArmed = false;
static long gAllocs = 0;
static long gFailAt = -1;
static bool gHit = false;
static bool refuseNow() {
  if (!gArmed) return false;
  if (gAllocs++ == gFailAt) {
    gHit = true;
    return true;
  }
  return false;
}
void* operator new(std::size_t n) {
  if (refuseNow()) throw std::bad_alloc();
  void* p = std::malloc(n ? n : 1);
  if (!p) throw std::bad_alloc();
  return p;
}
void* operator new[](std::size_t n) { return operator new(n); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }
static void* refusingWorkingAlloc(std::size_t n) { return refuseNow() ? nullptr : std::malloc(n ? n : 1); }
static void workingFreeHook(void* p) { std::free(p); }

namespace {

// RB-T2's MemFiles with the faults this suite needs on top: no card (every call refused, a
// list included), a cut that tears an append or a whole write at any byte length, and a
// rename that removes its target first (a FAT binding).
class FxFiles : public txb::MemFiles {
 public:
  bool noCard = false;
  long cutAppend = -1;        // the append with this index (0-based) is cut ...
  size_t cutBytes = 0;        // ... after this many of its bytes landed
  long cutWhole = -1;         // the writeWhole with this index is cut after cutBytes
  long cutRename = -1;        // the rename with this index is cut (after the remove, if any)
  bool renameRemovesFirst = false;
  bool refuseWhole = false;   // a whole write lands half its bytes and answers false
  bool refuseList = false;    // list answers false (the rest answers)
  long landThenRefuse = -1;   // the append with this index lands whole, then answers false
  size_t refuseReadsFrom = SIZE_MAX;   // a read at or past this offset answers false
  std::vector<std::string> appended;   // the bytes of every append that ran
  size_t wholeCalls = 0;

  bool append(const std::string& path, const char* data, size_t len) override {
    if (noCard) return false;
    const long idx = (long)appended.size();
    appended.emplace_back(data, len);
    if (idx == landThenRefuse) {
      files[path].append(data, len);
      return false;
    }
    if (idx == cutAppend) {
      files[path].append(data, std::min(cutBytes, len));
      throw txb::Killed{"append"};
    }
    return txb::MemFiles::append(path, data, len);
  }
  bool writeWhole(const std::string& path, const std::string& data) override {
    if (noCard) return false;
    const long idx = (long)wholeCalls++;
    if (refuseWhole) {
      files[path] = data.substr(0, data.size() / 2);
      return false;
    }
    if (idx == cutWhole) {
      files[path] = data.substr(0, std::min(cutBytes, data.size()));
      throw txb::Killed{"whole"};
    }
    return txb::MemFiles::writeWhole(path, data);
  }
  bool rename(const std::string& from, const std::string& to) override {
    if (noCard) return false;
    if (renameRemovesFirst) files.erase(to);
    const long idx = (long)renames++;
    if (idx == cutRename) throw txb::Killed{"rename"};
    if (refuseAll || refuseRename) return false;
    // Like a filesystem rename: a refused allocation changes nothing beyond the removal above.
    auto it = files.find(from);
    if (it == files.end()) return false;
    std::string data = it->second;
    files[to] = std::move(data);
    files.erase(from);
    return true;
  }
  bool readAt(const std::string& path, size_t offset, char* buf, size_t want, size_t& got) override {
    got = 0;
    if (noCard || offset >= refuseReadsFrom) return false;
    return txb::MemFiles::readAt(path, offset, buf, want, got);
  }
  bool size(const std::string& path, size_t& out) override {
    return !noCard && txb::MemFiles::size(path, out);
  }
  bool remove(const std::string& path) override { return !noCard && txb::MemFiles::remove(path); }
  bool list(const std::string& dir, std::vector<std::string>& names) override {
    names.clear();
    return !noCard && !refuseList && txb::MemFiles::list(dir, names);
  }
  void clearCuts() {
    cutAppend = cutWhole = cutRename = -1;
    cutBytes = 0;
  }
};

std::string receiptOf(const std::string& chat, int i) {
  return "added L" + std::to_string(1000 + i) + " for " + chat + " item " + std::to_string(i);
}

std::vector<std::string> receipts(fx::EffectRing& r, const std::string& chat, size_t n = fx::kRingEntries) {
  std::vector<fx::RingEntry> v;
  r.recent(chat, n, v);
  std::vector<std::string> out;
  for (const auto& e : v) out.push_back(e.receipt);
  return out;
}

// The expected ring after appending items [0, n) of a chat: the newest 50, oldest first.
std::vector<std::string> expected(const std::string& chat, int from, int n) {
  std::vector<std::string> out;
  for (int i = std::max(from, n - (int)fx::kRingEntries); i < n; ++i) out.push_back(receiptOf(chat, i));
  return out;
}

size_t lineCount(const std::string& s) {
  size_t n = 0;
  for (char c : s) n += c == '\n';
  return n;
}

// The length a lead byte announces (1 to 4), 0 for a byte that cannot start a sequence.
size_t announced(uint8_t b) {
  if (b < 0x80) return 1;
  if ((b >> 5) == 6) return 2;
  if ((b >> 4) == 14) return 3;
  return (b >> 3) == 30 ? 4 : 0;
}

// The scalar of a whole sequence of `len` bytes, or UINT32_MAX when a byte is not a continuation.
uint32_t scalar(const std::string& s, size_t i, size_t len) {
  const uint8_t b = (uint8_t)s[i];
  uint32_t cp = len == 1 ? b : (uint32_t)(b & (0x7F >> len));
  for (size_t k = 1; k < len; ++k) {
    const uint8_t c = (uint8_t)s[i + k];
    if ((c & 0xC0) != 0x80) return UINT32_MAX;
    cp = (cp << 6) | (c & 0x3F);
  }
  return cp;
}

bool validUtf8(const std::string& s) {
  static const uint32_t kMin[5] = {0, 0, 0x80, 0x800, 0x10000};
  for (size_t i = 0; i < s.size();) {
    const size_t len = announced((uint8_t)s[i]);
    if (!len || i + len > s.size()) return false;
    const uint32_t cp = scalar(s, i, len);
    if (cp < kMin[len] || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) return false;
    i += len;
  }
  return true;
}

// One line, no control character of any kind (C0, DEL, C1, U+2028, U+2029).
bool oneCleanLine(const std::string& s) {
  if (!validUtf8(s)) return false;
  for (size_t i = 0; i < s.size(); ++i) {
    const uint8_t b = (uint8_t)s[i];
    if (b < 0x20 || b == 0x7F) return false;
    if (b == 0xC2 && i + 1 < s.size() && (uint8_t)s[i + 1] >= 0x80 && (uint8_t)s[i + 1] <= 0x9F) return false;
    if (b == 0xE2 && i + 2 < s.size() && (uint8_t)s[i + 1] == 0x80 &&
        ((uint8_t)s[i + 2] == 0xA8 || (uint8_t)s[i + 2] == 0xA9))
      return false;
  }
  return true;
}

// ---- working allocator accounting -------------------------------------------------------
size_t gWorkingLive = 0;
size_t gWorkingBytes = 0;
std::map<void*, size_t> gWorkingBlocks;
void* countingAlloc(size_t n) {
  void* p = std::malloc(n);
  if (p) {
    ++gWorkingLive;
    gWorkingBytes += n;
    gWorkingBlocks[p] = n;
  }
  return p;
}
void countingFree(void* p) {
  if (!p) return;
  auto it = gWorkingBlocks.find(p);
  if (it != gWorkingBlocks.end()) {
    --gWorkingLive;
    gWorkingBytes -= it->second;
    gWorkingBlocks.erase(it);
  }
  std::free(p);
}

}  // namespace

// ---- 50 entries per chat ---------------------------------------------------------------------
void test_fifty_per_chat_oldest_dropped_at_every_count() {
  for (int n = 0; n <= 3 * (int)fx::kRingEntries; ++n) {
    FxFiles f;
    fx::EffectRing r(&f);
    for (int i = 0; i < n; ++i)
      TEST_ASSERT_EQUAL(int(fx::AppendResult::Stored), int(r.append("42", 1700000000 + i, "ledger_write", receiptOf("42", i))));
    const auto got = receipts(r, "42");
    TEST_ASSERT_EQUAL_UINT32(std::min(n, (int)fx::kRingEntries), got.size());
    TEST_ASSERT_TRUE(got == expected("42", 0, n));
    // The newest k, for every k.
    for (size_t k = 0; k <= fx::kRingEntries + 1; ++k) {
      std::vector<fx::RingEntry> v;
      const size_t m = r.recent("42", k, v);
      TEST_ASSERT_EQUAL_UINT32(std::min(k, got.size()), m);
      TEST_ASSERT_EQUAL_UINT32(m, v.size());
      for (size_t j = 0; j < m; ++j) TEST_ASSERT_EQUAL_STRING(got[got.size() - m + j].c_str(), v[j].receipt);
    }
  }
}

void test_entry_holds_time_tool_and_receipt() {
  FxFiles f;
  fx::EffectRing r(&f);
  r.append("7", 1763470920, "ledger_write", "added L00A3 skyr 150 g");
  r.append("7", 0, "memory.write", "stored m1a2b3c4");
  std::vector<fx::RingEntry> v;
  TEST_ASSERT_EQUAL_UINT32(2, r.recent("7", 10, v));
  TEST_ASSERT_EQUAL_INT64(1763470920, v[0].epoch);
  TEST_ASSERT_EQUAL_STRING("ledger_write", v[0].tool);
  TEST_ASSERT_EQUAL_STRING("added L00A3 skyr 150 g", v[0].receipt);
  TEST_ASSERT_TRUE(v[0].stored);
  TEST_ASSERT_EQUAL_INT64(0, v[1].epoch);   // the clock was not set
  // A negative time is stored as unset.
  r.append("7", -5, "x", "y");
  r.recent("7", 1, v);
  TEST_ASSERT_EQUAL_INT64(0, v[0].epoch);
  // And reads back the same after a restart.
  fx::EffectRing r2(&f);
  r2.recent("7", 10, v);
  TEST_ASSERT_EQUAL_UINT32(3, v.size());
  TEST_ASSERT_EQUAL_INT64(1763470920, v[0].epoch);
  TEST_ASSERT_EQUAL_STRING("memory.write", v[1].tool);
}

// ---- cleaning -------------------------------------------------------------------------------
void test_every_byte_value_is_cleaned_to_one_line() {
  for (int b = 0; b < 256; ++b) {
    for (const char* ctx : {"", "a", "ab\xC3\xA9"}) {
      std::string in = std::string(ctx) + (char)b + "z";
      const std::string out = fx::cleanText(in, fx::kReceiptMax);
      TEST_ASSERT_TRUE_MESSAGE(oneCleanLine(out), ("byte " + std::to_string(b)).c_str());
      TEST_ASSERT_TRUE(out.size() <= fx::kReceiptMax);
      TEST_ASSERT_EQUAL_CHAR('z', out.back());   // nothing after it is swallowed
      if (b >= 0x20 && b < 0x7F) TEST_ASSERT_EQUAL_STRING(in.c_str(), out.c_str());
      if (b < 0x20 || b == 0x7F) TEST_ASSERT_EQUAL_STRING((std::string(ctx) + " z").c_str(), out.c_str());
    }
  }
  // Every C1 control, U+2028 and U+2029 become one space; other valid characters stay.
  for (uint32_t cp = 0x80; cp <= 0x9F; ++cp) {
    std::string in = "a";
    in += (char)0xC2;
    in += (char)cp;
    in += "b";
    TEST_ASSERT_EQUAL_STRING("a b", fx::cleanText(in, 160).c_str());
  }
  TEST_ASSERT_EQUAL_STRING("a b c", fx::cleanText("a\xE2\x80\xA8" "b\xE2\x80\xA9" "c", 160).c_str());
  TEST_ASSERT_EQUAL_STRING("caf\xC3\xA9 \xE2\x82\xAC \xF0\x9F\x8D\x8E",
                           fx::cleanText("caf\xC3\xA9 \xE2\x82\xAC \xF0\x9F\x8D\x8E", 160).c_str());
  // Invalid UTF-8: overlong, surrogate, past U+10FFFF, a lone continuation, a cut sequence.
  TEST_ASSERT_EQUAL_STRING("??x", fx::cleanText("\xC0\xAFx", 160).c_str());
  TEST_ASSERT_EQUAL_STRING("???x", fx::cleanText("\xED\xA0\x80x", 160).c_str());
  TEST_ASSERT_EQUAL_STRING("????x", fx::cleanText("\xF4\x90\x80\x80x", 160).c_str());
  TEST_ASSERT_EQUAL_STRING("?x", fx::cleanText("\x80x", 160).c_str());
  TEST_ASSERT_EQUAL_STRING("??", fx::cleanText("\xE2\x82", 160).c_str());
  // A sequence cut by the length (its last byte is past n, and valid) is not read past n.
  TEST_ASSERT_EQUAL_STRING("??", fx::cleanText("\xE2\x82\xAC", 2, 160).c_str());
  // Newline, carriage return and tab: a space each.
  TEST_ASSERT_EQUAL_STRING("a b c d", fx::cleanText("a\nb\rc\td", 160).c_str());
}

void test_clip_is_on_a_utf8_boundary_and_keeps_the_prefix() {
  std::string ascii(400, 'x');
  for (size_t i = 0; i < ascii.size(); ++i) ascii[i] = (char)('a' + i % 26);
  TEST_ASSERT_EQUAL_STRING(ascii.substr(0, 160).c_str(), fx::cleanText(ascii, 160).c_str());
  // Each width of character at every alignment against the cap.
  const char* chars[] = {"\xC3\xA9", "\xE2\x82\xAC", "\xF0\x9F\x8D\x8E"};
  for (const char* ch : chars) {
    for (size_t lead = 0; lead < 4; ++lead) {
      std::string in(lead, 'a');
      while (in.size() < 300) in += ch;
      const std::string out = fx::cleanText(in, 160);
      TEST_ASSERT_TRUE(out.size() <= 160);
      TEST_ASSERT_TRUE(out.size() > 160 - std::strlen(ch));   // nothing dropped short of the cap
      TEST_ASSERT_TRUE(validUtf8(out));
      TEST_ASSERT_EQUAL_STRING(in.substr(0, out.size()).c_str(), out.c_str());
    }
  }
  // Random bytes: always one clean line under the cap.
  std::mt19937 rng(7);
  for (int t = 0; t < 4000; ++t) {
    std::string in(rng() % 400, '\0');
    for (char& c : in) c = (char)(rng() % 256);
    const size_t cap = rng() % 200;
    const std::string out = fx::cleanText(in, cap);
    TEST_ASSERT_TRUE(out.size() <= cap);
    TEST_ASSERT_TRUE(oneCleanLine(out));
    TEST_ASSERT_EQUAL_STRING(out.c_str(), fx::cleanText(out, cap).c_str());   // idempotent
  }
}

void test_appended_text_is_cleaned_and_capped() {
  FxFiles f;
  fx::EffectRing r(&f);
  std::string tool(100, 't');
  std::string rec = "line one\nline two\x1b[31m red\x7f";
  rec += std::string(300, 'r');
  r.append("9", 1, tool, rec);
  r.append("9", 1, std::string("ev\nil\ttool"), "\xE2\x80\xA8x");
  std::vector<fx::RingEntry> v;
  r.recent("9", 2, v);
  TEST_ASSERT_EQUAL_UINT32(fx::kToolMax, std::strlen(v[0].tool));
  TEST_ASSERT_EQUAL_UINT32(fx::kReceiptMax, std::strlen(v[0].receipt));
  TEST_ASSERT_EQUAL_STRING(fx::cleanText(rec, fx::kReceiptMax).c_str(), v[0].receipt);
  TEST_ASSERT_TRUE(oneCleanLine(v[0].receipt));
  TEST_ASSERT_EQUAL_STRING("ev il tool", v[1].tool);
  TEST_ASSERT_EQUAL_STRING(" x", v[1].receipt);
  // The file holds exactly two lines, the tabs only the format's own.
  const std::string& file = f.files["/mem/fx/9.ring"];
  TEST_ASSERT_EQUAL_UINT32(2, lineCount(file));
}

// ---- the line format ------------------------------------------------------------------------
void test_line_round_trip_and_refusals() {
  std::mt19937 rng(11);
  for (int t = 0; t < 500; ++t) {
    fx::RingEntry e;
    e.epoch = (int64_t)(rng() % 4000000000u) * (t % 3);
    std::string tool(rng() % 60, 'q'), rec(rng() % 220, '\0');
    for (char& c : rec) c = (char)(rng() % 256);
    std::snprintf(e.tool, sizeof e.tool, "%s", fx::cleanText(tool, fx::kToolMax).c_str());
    std::snprintf(e.receipt, sizeof e.receipt, "%s", fx::cleanText(rec, fx::kReceiptMax).c_str());
    const std::string line = fx::encodeLine(e);
    TEST_ASSERT_TRUE(line.size() <= fx::kLineMax);
    TEST_ASSERT_EQUAL_CHAR('\n', line.back());
    TEST_ASSERT_EQUAL_UINT32(1, lineCount(line));
    fx::RingEntry d;
    TEST_ASSERT_TRUE(fx::decodeLine(line.data(), line.size() - 1, d));
    TEST_ASSERT_EQUAL_INT64(e.epoch, d.epoch);
    TEST_ASSERT_EQUAL_STRING(e.tool, d.tool);
    TEST_ASSERT_EQUAL_STRING(e.receipt, d.receipt);
    // Every strict prefix (a torn tail) and every single-byte change is refused.
    for (size_t k = 0; k + 1 < line.size(); ++k) {
      fx::RingEntry x;
      TEST_ASSERT_FALSE(fx::decodeLine(line.data(), k, x));
    }
    if (t < 40) {
      for (size_t k = 0; k + 1 < line.size(); ++k) {
        std::string bad = line.substr(0, line.size() - 1);
        bad[k] = (char)(bad[k] ^ (1 + rng() % 254));
        fx::RingEntry x;
        TEST_ASSERT_FALSE(fx::decodeLine(bad.data(), bad.size(), x));
      }
    }
  }
  fx::RingEntry x;
  TEST_ASSERT_FALSE(fx::decodeLine("", 0, x));
  TEST_ASSERT_FALSE(fx::decodeLine("~", 1, x));
  // A crc-valid line in another form (an extra field, a sign, a raw control) is refused.
  fx::RingEntry e;
  std::snprintf(e.tool, sizeof e.tool, "a\tb");
  std::string l = fx::encodeLine(e);
  TEST_ASSERT_FALSE(fx::decodeLine(l.data(), l.size() - 1, x));
  fx::RingEntry e2;
  std::snprintf(e2.receipt, sizeof e2.receipt, "a\x01" "b");
  l = fx::encodeLine(e2);
  TEST_ASSERT_FALSE(fx::decodeLine(l.data(), l.size() - 1, x));
}

// ---- one append per effect ------------------------------------------------------------------
void test_one_append_of_one_whole_line_per_effect() {
  FxFiles f;
  fx::EffectRing r(&f);
  size_t wholes = 0;
  for (int i = 0; i < 260; ++i) {
    const size_t before = f.appended.size();
    const size_t wBefore = f.wholeCalls;
    r.append("5", i, "files.append", receiptOf("5", i));
    TEST_ASSERT_EQUAL_UINT32(before + 1, f.appended.size());
    const std::string& a = f.appended.back();
    TEST_ASSERT_TRUE(a.size() <= fx::kLineMax);
    TEST_ASSERT_EQUAL_UINT32(1, lineCount(a));
    TEST_ASSERT_EQUAL_CHAR('\n', a.back());
    wholes += f.wholeCalls - wBefore;
    // The file stays bounded: at most kCompactLines lines, rewritten from RAM.
    TEST_ASSERT_TRUE(lineCount(f.files["/mem/fx/5.ring"]) <= fx::kCompactLines);
  }
  // A rewrite once per (kCompactLines - kRingEntries) appends, never per append.
  TEST_ASSERT_TRUE(wholes >= 1);
  TEST_ASSERT_TRUE(wholes <= 260 / (fx::kCompactLines - fx::kRingEntries) + 1);
  TEST_ASSERT_TRUE(f.files["/mem/fx/5.ring"].size() <= fx::kCompactLines * fx::kLineMax);
  TEST_ASSERT_EQUAL_UINT32(0, f.files.count("/mem/fx/5.ring.tmp"));
  fx::EffectRing r2(&f);
  TEST_ASSERT_TRUE(receipts(r2, "5") == expected("5", 0, 260));
}

// ---- crash safety ---------------------------------------------------------------------------
// Cut the append of item `k` after every byte length; reopen; the ring holds every earlier
// item and item k only if its whole line landed; later appends land cleanly and survive.
void test_a_cut_at_any_point_of_an_append_loses_at_most_that_entry() {
  const int kItems = 8;
  for (int k = 0; k < kItems; ++k) {
    FxFiles probe;
    {
      fx::EffectRing r(&probe);
      for (int i = 0; i <= k; ++i) r.append("11", i, "t", receiptOf("11", i));
    }
    const size_t lineLen = probe.appended.back().size();
    for (size_t cut = 0; cut <= lineLen; ++cut) {
      FxFiles f;
      f.cutAppend = k;
      f.cutBytes = cut;
      bool killed = false;
      try {
        fx::EffectRing r(&f);
        for (int i = 0; i < kItems; ++i) r.append("11", i, "t", receiptOf("11", i));
      } catch (const txb::Killed&) {
        killed = true;
      }
      TEST_ASSERT_TRUE(killed);
      f.clearCuts();
      fx::EffectRing r2(&f);
      auto got = receipts(r2, "11");
      const int kept = cut == lineLen ? k + 1 : k;
      TEST_ASSERT_TRUE_MESSAGE(got == expected("11", 0, kept), ("k=" + std::to_string(k) + " cut=" + std::to_string(cut)).c_str());
      // Keep going after the restart: nothing glued, nothing completed.
      for (int i = 100; i < 104; ++i) r2.append("11", i, "t", receiptOf("11", i));
      std::vector<std::string> want = expected("11", 0, kept);
      for (int i = 100; i < 104; ++i) want.push_back(receiptOf("11", i));
      TEST_ASSERT_TRUE(receipts(r2, "11") == want);
      fx::EffectRing r3(&f);
      TEST_ASSERT_TRUE(receipts(r3, "11") == want);
      if (cut > 0 && cut < lineLen) TEST_ASSERT_TRUE(r3.stats().dropped >= 1);
    }
  }
}

void test_a_torn_tail_is_never_completed_into_a_line() {
  // The tail lacks only its '\n': the next append must not turn it into a whole line.
  FxFiles f;
  {
    fx::EffectRing r(&f);
    r.append("12", 1, "t", "first");
  }
  fx::RingEntry e;
  e.epoch = 2;
  std::snprintf(e.tool, sizeof e.tool, "t");
  std::snprintf(e.receipt, sizeof e.receipt, "torn");
  std::string line = fx::encodeLine(e);
  f.files["/mem/fx/12.ring"] += line.substr(0, line.size() - 1);
  fx::EffectRing r(&f);
  r.append("12", 3, "t", "third");
  TEST_ASSERT_EQUAL_STRING("~\n", f.appended.back().substr(0, 2).c_str());
  fx::EffectRing r2(&f);
  auto got = receipts(r2, "12");
  TEST_ASSERT_EQUAL_UINT32(2, got.size());
  TEST_ASSERT_EQUAL_STRING("first", got[0].c_str());
  TEST_ASSERT_EQUAL_STRING("third", got[1].c_str());
}

void test_a_cut_during_a_rewrite_keeps_the_ring() {
  const int n = (int)fx::kCompactLines + 3;
  for (int mode = 0; mode < 2; ++mode) {
    for (int stage = 0; stage < 3; ++stage) {   // whole torn short, whole torn full, rename
      for (size_t cut : {(size_t)0, (size_t)1, (size_t)300, (size_t)100000}) {
        FxFiles f;
        f.renameRemovesFirst = mode == 1;
        if (stage < 2) {
          f.cutWhole = 0;
          f.cutBytes = stage == 0 ? cut : 1000000;
        } else {
          f.cutRename = 0;
        }
        int done = 0;
        try {
          fx::EffectRing r(&f);
          for (int i = 0; i < n; ++i, ++done) r.append("13", i, "t", receiptOf("13", i));
        } catch (const txb::Killed&) {
        }
        TEST_ASSERT_TRUE(done < n);
        f.clearCuts();
        fx::EffectRing r2(&f);
        // The append that triggered the rewrite had landed before the rewrite began.
        TEST_ASSERT_TRUE(receipts(r2, "13") == expected("13", 0, done + 1));
        TEST_ASSERT_EQUAL_UINT32(0, f.files.count("/mem/fx/13.ring.tmp"));
        r2.append("13", 500, "t", "after");
        fx::EffectRing r3(&f);
        auto want = expected("13", 0, done + 1);
        want.push_back("after");
        if (want.size() > fx::kRingEntries) want.erase(want.begin());
        TEST_ASSERT_TRUE(receipts(r3, "13") == want);
      }
    }
  }
}

void test_garbage_lines_are_dropped_and_good_ones_kept() {
  FxFiles f;
  {
    fx::EffectRing r(&f);
    r.append("14", 1, "t", "a");
  }
  std::string& file = f.files["/mem/fx/14.ring"];
  file += "garbage\n";
  file += std::string(5000, 'z') + "\n";   // longer than any line, across read slices
  file += "f1\t1\tt\tb\t00000000\n";        // bad crc
  {
    fx::EffectRing r(&f);
    r.append("14", 2, "t", "c");
  }
  fx::EffectRing r(&f);
  auto got = receipts(r, "14");
  TEST_ASSERT_EQUAL_UINT32(2, got.size());
  TEST_ASSERT_EQUAL_STRING("a", got[0].c_str());
  TEST_ASSERT_EQUAL_STRING("c", got[1].c_str());
  TEST_ASSERT_EQUAL_UINT32(3, r.stats().dropped);
}

// ---- restart --------------------------------------------------------------------------------
void test_the_ring_survives_restarts_at_every_count() {
  FxFiles f;
  int total = 0;
  for (int round = 0; round < 12; ++round) {
    fx::EffectRing r(&f);
    TEST_ASSERT_TRUE(receipts(r, "15") == expected("15", 0, total));
    TEST_ASSERT_EQUAL_INT(int(fx::Backing::Storage), int(r.backing("15")));
    for (int i = 0; i < 17; ++i, ++total) r.append("15", total, "t", receiptOf("15", total));
  }
}

void test_load_reads_in_bounded_slices() {
  FxFiles f;
  {
    fx::EffectRing r(&f);
    for (int i = 0; i < 99; ++i) r.append("16", i, "t", std::string(150, 'x') + std::to_string(i));
  }
  // Re-open over a wrapper that records each read's size.
  class Reads : public tx::TxFiles {
   public:
    explicit Reads(FxFiles& in) : in_(in) {}
    size_t maxWant = 0, reads = 0;
    bool append(const std::string& p, const char* d, size_t n) override { return in_.append(p, d, n); }
    bool readAt(const std::string& p, size_t o, char* b, size_t w, size_t& g) override {
      maxWant = std::max(maxWant, w);
      ++reads;
      return in_.readAt(p, o, b, w, g);
    }
    bool size(const std::string& p, size_t& o) override { return in_.size(p, o); }
    bool writeWhole(const std::string& p, const std::string& d) override { return in_.writeWhole(p, d); }
    bool rename(const std::string& a, const std::string& b) override { return in_.rename(a, b); }
    bool remove(const std::string& p) override { return in_.remove(p); }
    bool list(const std::string& d, std::vector<std::string>& n) override { return in_.list(d, n); }
    FxFiles& in_;
  } reads(f);
  fx::EffectRing r(&reads);
  TEST_ASSERT_EQUAL_UINT32(fx::kRingEntries, receipts(r, "16").size());
  TEST_ASSERT_TRUE(reads.maxWant <= fx::kReadSlice);
  const size_t bytes = f.files["/mem/fx/16.ring"].size();
  TEST_ASSERT_TRUE(reads.reads <= bytes / fx::kReadSlice + 2);
}

// ---- no card --------------------------------------------------------------------------------
void test_no_storage_runs_from_ram_and_says_so() {
  fx::EffectRing r(nullptr);
  for (int i = 0; i < 70; ++i)
    TEST_ASSERT_EQUAL(int(fx::AppendResult::RamOnly), int(r.append("17", i, "t", receiptOf("17", i))));
  TEST_ASSERT_TRUE(receipts(r, "17") == expected("17", 0, 70));
  TEST_ASSERT_EQUAL(int(fx::Backing::RamOnly), int(r.backing("17")));
  std::vector<fx::RingEntry> v;
  r.recent("17", 1, v);
  TEST_ASSERT_FALSE(v[0].stored);
  TEST_ASSERT_EQUAL_UINT32(70, r.stats().ramOnly);
}

void test_no_card_then_card_back() {
  FxFiles f;
  {
    fx::EffectRing r(&f);
    for (int i = 0; i < 3; ++i) r.append("18", i, "t", receiptOf("18", i));
  }
  f.noCard = true;
  fx::EffectRing r(&f);
  TEST_ASSERT_EQUAL(int(fx::AppendResult::RamOnly), int(r.append("18", 3, "t", receiptOf("18", 3))));
  TEST_ASSERT_EQUAL(int(fx::Backing::RamOnly), int(r.backing("18")));
  {
    auto got = receipts(r, "18");
    TEST_ASSERT_EQUAL_UINT32(1, got.size());   // the card's entries are not readable now
    TEST_ASSERT_EQUAL_STRING(receiptOf("18", 3).c_str(), got[0].c_str());
  }
  f.noCard = false;
  // The card is back: the stored entries come first, the RAM-only one after them, then new.
  TEST_ASSERT_EQUAL(int(fx::AppendResult::Stored), int(r.append("18", 4, "t", receiptOf("18", 4))));
  TEST_ASSERT_EQUAL(int(fx::Backing::Storage), int(r.backing("18")));
  TEST_ASSERT_TRUE(receipts(r, "18") == expected("18", 0, 5));
  std::vector<fx::RingEntry> v;
  r.recent("18", 5, v);
  TEST_ASSERT_TRUE(v[0].stored);
  TEST_ASSERT_FALSE(v[3].stored);
  TEST_ASSERT_TRUE(v[4].stored);
  // After a restart the RAM-only entry is gone (it was never written), nothing else is.
  fx::EffectRing r2(&f);
  auto got = receipts(r2, "18");
  TEST_ASSERT_EQUAL_UINT32(4, got.size());
  TEST_ASSERT_EQUAL_STRING(receiptOf("18", 4).c_str(), got.back().c_str());
}

void test_card_pulled_mid_life_then_back_has_no_duplicates() {
  FxFiles f;
  fx::EffectRing r(&f);
  for (int i = 0; i < 10; ++i) r.append("19", i, "t", receiptOf("19", i));
  f.noCard = true;
  for (int i = 10; i < 13; ++i) TEST_ASSERT_EQUAL(int(fx::AppendResult::RamOnly), int(r.append("19", i, "t", receiptOf("19", i))));
  TEST_ASSERT_EQUAL(int(fx::Backing::RamOnly), int(r.backing("19")));
  TEST_ASSERT_TRUE(receipts(r, "19") == expected("19", 0, 13));
  f.noCard = false;
  for (int i = 13; i < 200; ++i) r.append("19", i, "t", receiptOf("19", i));
  TEST_ASSERT_TRUE(receipts(r, "19") == expected("19", 0, 200));
  r.visit("19", fx::kRingEntries, [](const fx::RingEntry& e) { TEST_ASSERT_TRUE(e.stored); });   // rewritten
  fx::EffectRing r2(&f);
  TEST_ASSERT_TRUE(receipts(r2, "19") == expected("19", 0, 200));
}

void test_a_refused_append_keeps_ram_and_the_file_stays_readable() {
  FxFiles f;
  fx::EffectRing r(&f);
  r.append("20", 0, "t", "zero");
  f.refuseAppend = (long)f.appends;   // MemFiles counts every append it sees
  // A refusal may have left part of the line behind: model it by a torn fragment.
  f.files["/mem/fx/20.ring"] += "f1\t1\tt\tpar";
  TEST_ASSERT_EQUAL(int(fx::AppendResult::RamOnly), int(r.append("20", 1, "t", "one")));
  TEST_ASSERT_EQUAL(int(fx::AppendResult::Stored), int(r.append("20", 2, "t", "two")));
  TEST_ASSERT_TRUE((std::vector<std::string>{"zero", "one", "two"}) == receipts(r, "20"));
  fx::EffectRing r2(&f);
  TEST_ASSERT_TRUE((std::vector<std::string>{"zero", "two"}) == receipts(r2, "20"));
}

// ---- chats apart ----------------------------------------------------------------------------
void test_chats_never_see_each_others_entries() {
  FxFiles f;
  std::vector<std::string> chats = {"1", "-1001234567890", "2", "web", "serial", "voice", "Web", "k776562",
                                    "3", "-2", "owner", "mcp", "42", "x", "9999999999"};
  std::map<std::string, int> count;
  std::mt19937 rng(3);
  {
    fx::EffectRing r(&f);
    for (int t = 0; t < 1500; ++t) {
      const std::string& c = chats[rng() % chats.size()];
      TEST_ASSERT_EQUAL(int(fx::AppendResult::Stored), int(r.append(c, t, "t", receiptOf(c, count[c]))));
      ++count[c];
      if (t % 97 == 0) {
        const std::string& q = chats[rng() % chats.size()];
        TEST_ASSERT_TRUE(receipts(r, q) == expected(q, 0, count[q]));
      }
    }
    TEST_ASSERT_TRUE(r.residentChats() <= fx::kMaxChats);
    TEST_ASSERT_TRUE(r.stats().evictions > 0);
    for (const auto& c : chats) TEST_ASSERT_TRUE_MESSAGE(receipts(r, c) == expected(c, 0, count[c]), c.c_str());
  }
  fx::EffectRing r(&f);
  for (const auto& c : chats) TEST_ASSERT_TRUE_MESSAGE(receipts(r, c) == expected(c, 0, count[c]), c.c_str());
  // Each chat's file holds its own entries only.
  for (const auto& c : chats) {
    std::string p;
    TEST_ASSERT_TRUE(fx::ringPath(c, p));
    TEST_ASSERT_TRUE(f.files[p].find(" for " + c + " item") != std::string::npos);
  }
}

void test_ram_only_chats_are_kept_apart_too() {
  fx::EffectRing r(nullptr);
  for (int i = 0; i < 6; ++i) {
    r.append("100", i, "t", receiptOf("100", i));
    r.append("200", i, "t", receiptOf("200", i));
  }
  TEST_ASSERT_TRUE(receipts(r, "100") == expected("100", 0, 6));
  TEST_ASSERT_TRUE(receipts(r, "200") == expected("200", 0, 6));
  TEST_ASSERT_EQUAL_UINT32(0, receipts(r, "300").size());
}

void test_the_chat_key_is_injective_and_safe() {
  std::set<std::string> keys;
  std::vector<std::string> ids;
  for (int i = -300; i < 300; ++i) ids.push_back(std::to_string(i));
  for (const char* s : {"web", "Web", "WEB", "serial", "voice", "k776562", "k", "-", "--", "0", "00", "owner", "a b",
                        "\xC3\xA9", "k6b", "_", "-0", "12345678901234567890123456789012345678901234567"})
    ids.push_back(s);
  std::mt19937 rng(5);
  for (int t = 0; t < 3000; ++t) {
    std::string s(1 + rng() % fx::kOtherIdMax, '\0');
    for (char& c : s) c = (char)(1 + rng() % 255);
    ids.push_back(s);
  }
  std::set<std::string> seen;
  for (const auto& id : ids) {
    if (!seen.insert(id).second) continue;
    std::string k, p;
    TEST_ASSERT_TRUE_MESSAGE(fx::ringKey(id, k), id.c_str());
    TEST_ASSERT_TRUE(k.size() >= 1 && k.size() <= fx::kKeyMax);
    for (char c : k) TEST_ASSERT_TRUE((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || c == '-' || c == 'k');
    TEST_ASSERT_TRUE_MESSAGE(keys.insert(k).second, id.c_str());
    TEST_ASSERT_TRUE(fx::ringPath(id, p));
    TEST_ASSERT_EQUAL_STRING(("/mem/fx/" + k + ".ring").c_str(), p.c_str());
  }
  // Refused: empty, too long, a path.
  for (const std::string& bad : {std::string(), std::string(48, '1'), std::string(fx::kOtherIdMax + 1, 'a')}) {
    std::string k = "junk", p = "junk";
    TEST_ASSERT_FALSE(fx::ringKey(bad, k));
    TEST_ASSERT_TRUE(k.empty());
    TEST_ASSERT_FALSE(fx::ringPath(bad, p));
  }
  FxFiles f;
  fx::EffectRing r(&f);
  TEST_ASSERT_EQUAL(int(fx::AppendResult::Refused), int(r.append("", 1, "t", "x")));
  TEST_ASSERT_EQUAL(int(fx::AppendResult::Refused), int(r.append(std::string(60, 'a'), 1, "t", "x")));
  TEST_ASSERT_EQUAL_UINT32(0, f.appended.size());
  TEST_ASSERT_EQUAL_UINT32(2, r.stats().refused);
  // "../x" is keyed as hex: it can never name a path.
  std::string p;
  TEST_ASSERT_TRUE(fx::ringPath("../x", p));
  TEST_ASSERT_TRUE(p.find("..") == std::string::npos);
}

// ---- memory ---------------------------------------------------------------------------------
void test_ram_rings_live_in_the_working_allocator() {
  nimbus::orch::setWorkingAllocators(countingAlloc, countingFree);
  const size_t baseLive = gWorkingLive, baseBytes = gWorkingBytes;
  {
    FxFiles f;
    fx::EffectRing r(&f);
    for (size_t c = 0; c < fx::kMaxChats + 4; ++c)
      for (int i = 0; i < 60; ++i) r.append(std::to_string(c), i, "t", receiptOf("c", i));
    // Every resident ring is in the working allocator: at least one ring's worth per chat.
    TEST_ASSERT_TRUE(gWorkingBytes - baseBytes >= fx::kMaxChats * fx::kRingEntries * sizeof(fx::RingEntry));
    // And bounded: kMaxChats rings plus the slot table.
    TEST_ASSERT_TRUE(gWorkingBytes - baseBytes <= fx::kMaxChats * (fx::kRingEntries * sizeof(fx::RingEntry) + 256) + 4096);
  }
  TEST_ASSERT_EQUAL_UINT32(baseLive, gWorkingLive);   // all given back
  nimbus::orch::setWorkingAllocators(nullptr, nullptr);
  TEST_ASSERT_TRUE(sizeof(fx::RingEntry) <= 224);
}

// ---- the stored form is fixed (a change is a format change, not a refactor) -------------------
void test_the_stored_form_is_fixed() {
  fx::RingEntry e;
  e.epoch = 1763470920;
  std::snprintf(e.tool, sizeof e.tool, "ledger_write");
  std::snprintf(e.receipt, sizeof e.receipt, "added L00A3 skyr 150 g");
  TEST_ASSERT_EQUAL_STRING("f1\t1763470920\tledger_write\tadded L00A3 skyr 150 g\tabd007e9\n", fx::encodeLine(e).c_str());
  fx::RingEntry d;
  const char* big = "f1\t9223372036854775807\tt\tx\tc0f0528b";
  TEST_ASSERT_TRUE(fx::decodeLine(big, std::strlen(big), d));
  TEST_ASSERT_EQUAL_INT64(INT64_MAX, d.epoch);
  const char* over = "f1\t9999999999999999999\tt\tx\td884ebc9";   // past int64, crc good
  TEST_ASSERT_FALSE(fx::decodeLine(over, std::strlen(over), d));
  const char* wraps = "f1\t18446744073709551617\tt\tx\t29e1a663";   // 2^64 + 1: twenty digits
  TEST_ASSERT_FALSE(fx::decodeLine(wraps, std::strlen(wraps), d));
  // The crc of "f1 TAB 12 TAB t TAB r" is 4071e101; "4071e0g1" would read as the same value
  // if a digit past f were taken, so only the exact lower-case form is accepted.
  const char* ok = "f1\t12\tt\tr\t4071e101";
  const char* g = "f1\t12\tt\tr\t4071e0g1";
  TEST_ASSERT_TRUE(fx::decodeLine(ok, std::strlen(ok), d));
  TEST_ASSERT_FALSE(fx::decodeLine(g, std::strlen(g), d));
  // A Telegram chat id is its own key; anything else is "k" + hex.
  std::string k;
  TEST_ASSERT_TRUE(fx::ringKey("-1001234567890", k));
  TEST_ASSERT_EQUAL_STRING("-1001234567890", k.c_str());
  TEST_ASSERT_TRUE(fx::ringKey("web", k));
  TEST_ASSERT_EQUAL_STRING("k776562", k.c_str());
  TEST_ASSERT_TRUE(fx::ringKey(std::string(fx::kOtherIdMax, 'a'), k));
  TEST_ASSERT_EQUAL_UINT32(1 + 2 * fx::kOtherIdMax, k.size());
}

void test_a_refused_rewrite_leaves_no_temp_and_retries() {
  FxFiles f;
  fx::EffectRing r(&f);
  f.refuseWhole = true;
  for (int i = 0; i < (int)fx::kCompactLines + 5; ++i)
    TEST_ASSERT_EQUAL(int(fx::AppendResult::Stored), int(r.append("21", i, "t", receiptOf("21", i))));
  TEST_ASSERT_EQUAL_UINT32(0, f.files.count("/mem/fx/21.ring.tmp"));
  TEST_ASSERT_EQUAL_UINT32(0, r.stats().rewrites);
  f.refuseWhole = false;
  r.append("21", 999, "t", receiptOf("21", (int)fx::kCompactLines + 5));
  TEST_ASSERT_EQUAL_UINT32(1, r.stats().rewrites);
  TEST_ASSERT_EQUAL_UINT32(fx::kRingEntries, lineCount(f.files["/mem/fx/21.ring"]));
  fx::EffectRing r2(&f);
  TEST_ASSERT_TRUE(receipts(r2, "21") == expected("21", 0, (int)fx::kCompactLines + 6));
}

// ---- review findings (fresh-context review, lens: lost or cross-chat effects, torn files) ------
// A rename refused on a binding that removes its target first leaves the temp as the only copy:
// it is kept, nothing is written beside it, and it becomes the file once storage answers.
void test_a_refused_rename_keeps_the_only_copy() {
  for (int restartAt = 0; restartAt < 2; ++restartAt) {
    FxFiles f;
    f.renameRemovesFirst = true;
    fx::EffectRing r(&f);
    const int n = (int)fx::kCompactLines;
    for (int i = 0; i < n - 1; ++i) r.append("22", i, "t", receiptOf("22", i));
    f.refuseRename = true;
    TEST_ASSERT_EQUAL(int(fx::AppendResult::Stored), int(r.append("22", n - 1, "t", receiptOf("22", n - 1))));
    TEST_ASSERT_EQUAL_UINT32(0, f.files.count("/mem/fx/22.ring"));
    TEST_ASSERT_EQUAL_UINT32(1, f.files.count("/mem/fx/22.ring.tmp"));
    TEST_ASSERT_EQUAL(int(fx::Backing::RamOnly), int(r.backing("22")));
    if (restartAt == 0) {
      f.refuseRename = false;
      fx::EffectRing r2(&f);
      TEST_ASSERT_TRUE(receipts(r2, "22") == expected("22", 0, n));
      continue;
    }
    // Still refused: the next effect stays in RAM and no second file appears.
    TEST_ASSERT_EQUAL(int(fx::AppendResult::RamOnly), int(r.append("22", n, "t", receiptOf("22", n))));
    TEST_ASSERT_EQUAL_UINT32(0, f.files.count("/mem/fx/22.ring"));
    f.refuseRename = false;
    TEST_ASSERT_EQUAL(int(fx::AppendResult::Stored), int(r.append("22", n + 1, "t", receiptOf("22", n + 1))));
    TEST_ASSERT_EQUAL_UINT32(0, f.files.count("/mem/fx/22.ring.tmp"));
    TEST_ASSERT_EQUAL(int(fx::Backing::Storage), int(r.backing("22")));
    TEST_ASSERT_TRUE(receipts(r, "22") == expected("22", 0, n + 2));
    // The file still counts past the rewrite bound, so that append rewrote it from RAM: the
    // RAM-only entry rode along (the contract), and every stored entry is there.
    fx::EffectRing r2(&f);
    TEST_ASSERT_TRUE(receipts(r2, "22") == expected("22", 0, n + 2));
  }
}

// A lone temp whose rename is refused at load: nothing is written until it is the file.
void test_a_lone_temp_is_never_shadowed_by_a_new_file() {
  FxFiles f;
  {
    fx::EffectRing r(&f);
    for (int i = 0; i < 10; ++i) r.append("23", i, "t", receiptOf("23", i));
  }
  f.files["/mem/fx/23.ring.tmp"] = f.files["/mem/fx/23.ring"];
  f.files.erase("/mem/fx/23.ring");
  f.refuseRename = true;
  {
    fx::EffectRing r(&f);
    TEST_ASSERT_EQUAL(int(fx::AppendResult::RamOnly), int(r.append("23", 10, "t", receiptOf("23", 10))));
    TEST_ASSERT_EQUAL_UINT32(0, f.files.count("/mem/fx/23.ring"));
    TEST_ASSERT_EQUAL(int(fx::Backing::RamOnly), int(r.backing("23")));
  }
  f.refuseRename = false;
  fx::EffectRing r(&f);
  TEST_ASSERT_TRUE(receipts(r, "23") == expected("23", 0, 10));
  r.append("23", 11, "t", receiptOf("23", 11));
  fx::EffectRing r2(&f);
  auto want = expected("23", 0, 10);
  want.push_back(receiptOf("23", 11));
  TEST_ASSERT_TRUE(receipts(r2, "23") == want);
}

// A refused append, then a list refusal with appends accepted: order is kept in RAM and on
// storage, and nothing comes back twice.
void test_order_is_kept_through_refusals() {
  FxFiles f;
  fx::EffectRing r(&f);
  r.append("24", 0, "t", "A");
  f.refuseAppend = (long)f.appends;
  r.append("24", 1, "t", "B");
  f.refuseList = true;
  TEST_ASSERT_EQUAL(int(fx::AppendResult::Stored), int(r.append("24", 2, "t", "C")));
  f.refuseList = false;
  TEST_ASSERT_TRUE((std::vector<std::string>{"A", "B", "C"}) == receipts(r, "24"));
  for (int i = 3; i < (int)fx::kCompactLines + 2; ++i) r.append("24", i, "t", "x" + std::to_string(i));
  std::vector<std::string> ram = receipts(r, "24");
  fx::EffectRing r2(&f);
  TEST_ASSERT_TRUE(ram == receipts(r2, "24"));   // the rewrite carried RAM's order
}

// A refused append whose bytes all landed: read back once, in this run and after a restart.
void test_a_refused_append_that_landed_is_not_doubled() {
  FxFiles f;
  fx::EffectRing r(&f);
  r.append("25", 0, "t", "A");
  f.landThenRefuse = (long)f.appended.size();
  TEST_ASSERT_EQUAL(int(fx::AppendResult::RamOnly), int(r.append("25", 1, "t", "B")));
  r.append("25", 2, "t", "C");
  TEST_ASSERT_TRUE((std::vector<std::string>{"A", "B", "C"}) == receipts(r, "25"));
  fx::EffectRing r2(&f);
  TEST_ASSERT_TRUE((std::vector<std::string>{"A", "B", "C"}) == receipts(r2, "25"));
  for (int i = 3; i < (int)fx::kCompactLines + 2; ++i) r.append("25", i, "t", "x" + std::to_string(i));
  fx::EffectRing r3(&f);
  auto got = receipts(r3, "25");
  TEST_ASSERT_TRUE(got == receipts(r, "25"));
  std::set<std::string> uniq(got.begin(), got.end());
  TEST_ASSERT_EQUAL_UINT32(got.size(), uniq.size());
}

// ---- mutation survivors (first pass), each closed by a case ----------------------------------
void test_a_line_of_another_format_is_refused() {
  // A crc-valid line whose tag is not f1 (a newer firmware's form, after a rollback).
  const std::string body = "f2\t1\tt\tx";
  fx::RingEntry e;
  std::snprintf(e.tool, sizeof e.tool, "t");
  std::snprintf(e.receipt, sizeof e.receipt, "x");
  e.epoch = 1;
  const std::string good = fx::encodeLine(e);
  // The f2 line carries its own good crc, so only the tag can refuse it.
  uint32_t c = 0xFFFFFFFFu;
  for (char ch : body) {
    c ^= (uint8_t)ch;
    for (int k = 0; k < 8; ++k) c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1u)));
  }
  char hex[16];
  std::snprintf(hex, sizeof hex, "%08x", (unsigned)~c);
  const std::string other = body + "\t" + hex;
  fx::RingEntry d;
  TEST_ASSERT_TRUE(fx::decodeLine(good.data(), good.size() - 1, d));
  TEST_ASSERT_FALSE(fx::decodeLine(other.data(), other.size(), d));
}

void test_a_rewrite_marks_ram_only_entries_stored() {
  FxFiles f;
  fx::EffectRing r(&f);
  for (int i = 0; i < 70; ++i) r.append("26", i, "t", receiptOf("26", i));   // 70 lines, no rewrite yet
  f.refuseAppend = (long)f.appends;
  r.append("26", 70, "t", receiptOf("26", 70));
  int i = 71;
  while (r.stats().rewrites == 0 && i < 200) r.append("26", i, "t", receiptOf("26", i)), ++i;
  TEST_ASSERT_EQUAL_UINT32(1, r.stats().rewrites);
  TEST_ASSERT_TRUE(i - 70 < (int)fx::kRingEntries);   // the RAM-only entry is still in the window
  r.visit("26", fx::kRingEntries, [](const fx::RingEntry& e) { TEST_ASSERT_TRUE(e.stored); });
  fx::EffectRing r2(&f);
  TEST_ASSERT_TRUE(receipts(r2, "26") == expected("26", 0, i));   // and it is on storage now
}

void test_a_load_cut_short_by_a_refused_read_trusts_nothing() {
  FxFiles f;
  {
    fx::EffectRing r(&f);
    for (int i = 0; i < 90; ++i) r.append("27", i, "t", std::string(120, 'p') + std::to_string(i));
  }
  const std::string before = f.files["/mem/fx/27.ring"];
  TEST_ASSERT_TRUE(before.size() > fx::kReadSlice);
  f.refuseReadsFrom = fx::kReadSlice;   // the first slice reads, the second is refused
  fx::EffectRing r(&f);
  TEST_ASSERT_EQUAL(int(fx::Backing::RamOnly), int(r.backing("27")));
  for (int i = 90; i < 200; ++i) TEST_ASSERT_EQUAL(int(fx::AppendResult::RamOnly), int(r.append("27", i, "t", "new")));
  TEST_ASSERT_TRUE(before == f.files["/mem/fx/27.ring"]);   // nothing written over a part read
  f.refuseReadsFrom = SIZE_MAX;
  fx::EffectRing r2(&f);
  TEST_ASSERT_EQUAL_UINT32(fx::kRingEntries, receipts(r2, "27").size());
  TEST_ASSERT_EQUAL_STRING((std::string(120, 'p') + "89").c_str(), receipts(r2, "27").back().c_str());
}

void test_the_least_recently_used_chat_is_evicted() {
  FxFiles f;
  fx::EffectRing r(&f);
  for (size_t c = 0; c < fx::kMaxChats; ++c) r.append(std::to_string(c), 1, "t", "x");
  const uint32_t loads = r.stats().loads;
  r.append("0", 2, "t", "y");                  // chat 0 is now the newest use
  r.append("99", 1, "t", "x");                 // evicts chat 1, the oldest use
  TEST_ASSERT_EQUAL_UINT32(loads + 1, r.stats().loads);
  r.append("0", 3, "t", "z");                  // still resident: no load
  TEST_ASSERT_EQUAL_UINT32(loads + 1, r.stats().loads);
  r.append("1", 2, "t", "y");                  // was evicted: loaded again, entries back
  TEST_ASSERT_EQUAL_UINT32(loads + 2, r.stats().loads);
  TEST_ASSERT_TRUE((std::vector<std::string>{"x", "y"}) == receipts(r, "1"));
}

// A refused allocation at every allocation of an append (load of a 99-line file, the append,
// the rewrite it triggers) and of a read: the call answers a designed state, nothing stored is
// lost, nothing half-done is left, and the ring goes on working once memory is back.
void test_a_refused_allocation_at_every_site_is_a_designed_state() {
  nimbus::orch::setWorkingAllocators(refusingWorkingAlloc, workingFreeHook);
  const int pre = (int)fx::kCompactLines - 1;
  int hits = 0, ramOnly = 0;
  for (int mode = 0; mode < 2; ++mode)
  for (long k = 0;; ++k) {
    FxFiles f;
    {
      fx::EffectRing seed(&f);
      for (int i = 0; i < pre; ++i) seed.append("31", i, "t", receiptOf("31", i));
    }
    f.renameRemovesFirst = mode == 1;   // a FAT binding: a refusal inside rename may leave only the temp
    const std::string fileBefore = f.files["/mem/fx/31.ring"];
    fx::EffectRing r(&f);
    const std::string chat = "31", tool = "t", X = receiptOf("31", pre);   // built before arming
    gAllocs = 0;
    gFailAt = k;
    gHit = false;
    gArmed = true;
    fx::AppendResult res = fx::AppendResult::Refused;
    bool threw = false;
    try {
      res = r.append(chat, pre, tool, X);
    } catch (...) {
      threw = true;
    }
    gArmed = false;
    TEST_ASSERT_FALSE_MESSAGE(threw, ("an exception left append at allocation " + std::to_string(k)).c_str());
    if (!gHit) break;   // every allocation of the call has been refused once
    ++hits;
    auto ram = receipts(r, "31");
    ramOnly += res == fx::AppendResult::RamOnly;
    if (res == fx::AppendResult::NoMemory) {
      TEST_ASSERT_TRUE(std::find(ram.begin(), ram.end(), X) == ram.end());
      TEST_ASSERT_TRUE(f.files["/mem/fx/31.ring"] == fileBefore);   // nothing written
    } else if (res == fx::AppendResult::RamOnly) {
      TEST_ASSERT_EQUAL_STRING(X.c_str(), ram.back().c_str());       // the load was refused: RAM only
      TEST_ASSERT_TRUE(f.files["/mem/fx/31.ring"] == fileBefore);
    } else {
      TEST_ASSERT_EQUAL(int(fx::AppendResult::Stored), int(res));
      TEST_ASSERT_TRUE(ram == expected("31", 0, pre + 1));          // a refused rewrite lost nothing
    }
    // Memory is back: the same ring goes on first (before any other load could settle the
    // files for it), then storage holds every stored entry in order and no temp is left.
    // Whole writes are refused for it, so no rewrite can mend a line written in the wrong place.
    f.refuseWhole = true;
    TEST_ASSERT_EQUAL(int(fx::AppendResult::Stored), int(r.append("31", 500, "t", "after")));
    f.refuseWhole = false;
    {
      fx::EffectRing fresh(&f);
      auto want = expected("31", 0, res == fx::AppendResult::Stored ? pre + 1 : pre);
      want.push_back("after");
      if (want.size() > fx::kRingEntries) want.erase(want.begin());
      // Either exactly the stored entries, or (when "after" reached the rewrite bound) the
      // ring as RAM holds it, RAM-only entries included: the order of record.
      const auto onDisk = receipts(fresh, "31");
      TEST_ASSERT_TRUE(onDisk == want || onDisk == receipts(r, "31"));
      TEST_ASSERT_EQUAL_STRING("after", onDisk.back().c_str());
      TEST_ASSERT_EQUAL_UINT32(0, f.files.count("/mem/fx/31.ring.tmp"));
    }
  }
  TEST_ASSERT_TRUE(hits > 5);   // the sweep reached the load, the append and the rewrite
  TEST_ASSERT_TRUE(ramOnly > 0);  // a refused load still keeps the effect, in RAM
  // Reads: recent and visit at every allocation answer 0 or the whole, never throw.
  for (long k = 0;; ++k) {
    FxFiles f;
    {
      fx::EffectRing seed(&f);
      for (int i = 0; i < 60; ++i) seed.append("32", i, "t", receiptOf("32", i));
    }
    fx::EffectRing r(&f);
    std::vector<fx::RingEntry> v;
    const std::string chat = "32";
    gAllocs = 0;
    gFailAt = k;
    gHit = false;
    gArmed = true;
    size_t got = 0;
    bool threw = false;
    try {
      got = r.recent(chat, 12, v);
    } catch (...) {
      threw = true;
    }
    gArmed = false;
    TEST_ASSERT_FALSE(threw);
    if (!gHit) break;
    TEST_ASSERT_TRUE(got == 0 || got == 12);
    TEST_ASSERT_EQUAL_UINT32(got, v.size());
    TEST_ASSERT_TRUE(receipts(r, "32") == expected("32", 0, 60));   // nothing lost, loads next time
    TEST_ASSERT_TRUE(r.stats().noMemory >= 1);
  }
  nimbus::orch::setWorkingAllocators(nullptr, nullptr);
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_fifty_per_chat_oldest_dropped_at_every_count);
  RUN_TEST(test_entry_holds_time_tool_and_receipt);
  RUN_TEST(test_every_byte_value_is_cleaned_to_one_line);
  RUN_TEST(test_clip_is_on_a_utf8_boundary_and_keeps_the_prefix);
  RUN_TEST(test_appended_text_is_cleaned_and_capped);
  RUN_TEST(test_line_round_trip_and_refusals);
  RUN_TEST(test_one_append_of_one_whole_line_per_effect);
  RUN_TEST(test_a_cut_at_any_point_of_an_append_loses_at_most_that_entry);
  RUN_TEST(test_a_torn_tail_is_never_completed_into_a_line);
  RUN_TEST(test_a_cut_during_a_rewrite_keeps_the_ring);
  RUN_TEST(test_garbage_lines_are_dropped_and_good_ones_kept);
  RUN_TEST(test_the_ring_survives_restarts_at_every_count);
  RUN_TEST(test_load_reads_in_bounded_slices);
  RUN_TEST(test_no_storage_runs_from_ram_and_says_so);
  RUN_TEST(test_no_card_then_card_back);
  RUN_TEST(test_card_pulled_mid_life_then_back_has_no_duplicates);
  RUN_TEST(test_a_refused_append_keeps_ram_and_the_file_stays_readable);
  RUN_TEST(test_chats_never_see_each_others_entries);
  RUN_TEST(test_ram_only_chats_are_kept_apart_too);
  RUN_TEST(test_the_chat_key_is_injective_and_safe);
  RUN_TEST(test_ram_rings_live_in_the_working_allocator);
  RUN_TEST(test_the_stored_form_is_fixed);
  RUN_TEST(test_a_refused_rewrite_leaves_no_temp_and_retries);
  RUN_TEST(test_a_refused_rename_keeps_the_only_copy);
  RUN_TEST(test_a_lone_temp_is_never_shadowed_by_a_new_file);
  RUN_TEST(test_order_is_kept_through_refusals);
  RUN_TEST(test_a_refused_append_that_landed_is_not_doubled);
  RUN_TEST(test_a_line_of_another_format_is_refused);
  RUN_TEST(test_a_rewrite_marks_ram_only_entries_stored);
  RUN_TEST(test_a_load_cut_short_by_a_refused_read_trusts_nothing);
  RUN_TEST(test_the_least_recently_used_chat_is_evicted);
  RUN_TEST(test_a_refused_allocation_at_every_site_is_a_designed_state);
  return UNITY_END();
}
