// test_tx_digest - the digest block, its archive file, the recent-files list and the
// chatsum mirror (PRD section 3, "Shape" and "Swap"; lane RB-C1). Properties:
// - the block is a pure function of the digests: byte-identical while nothing in it
//   changed, extended at its end by an append, and only a move to the archive changes its
//   start (prompt-cache stability), checked over 30 appends at every profile;
// - when the block passes its cap the oldest digest moves to
//   /mem/tx/<chatKey>/digests-YYYY-MM.md and nothing is lost or doubled between block and
//   archive, also across a cut or a refusal at any storage step (RB-T2's MemFiles);
// - a restart (loadDigests) gives the same block byte for byte;
// - the mirror is v5.1.1 summary text of at most 4,096 B, read unchanged by the real
//   FoldStore decoder (lib/core/src/orch_compact.cpp), with a pointer that only moves
//   forward and the other chats' records kept;
// - the recent-files list is rebuilt at a cold open and after a move or delete, and is
//   byte-identical otherwise.
#include <unity.h>

#include <algorithm>
#include <cstdlib>
#include <new>
#include <cstdio>
#include <map>
#include <random>
#include <set>
#include <string>
#include <vector>

#include "../support/tx_battery/items.h"
#include "../support/tx_battery/mem_files.h"
#include "../support/tx_battery/pending.h"
#include "nimbus/orch/compact.h"
#include "nimbus/orch/tx/digest.h"
#include "nimbus/orch/tx/log.h"

namespace tx = nimbus::orch::tx;
namespace orch = nimbus::orch;

void setUp() {}
void tearDown() {}

// Ruling 30: the test binary's operator new refuses once `gAllow` allocations have been
// made (-1 = never), as a PSRAM-first scope refuses on the device.
namespace {
long gAllow = -1;
long gCount = 0;   // allocations made
}
void* operator new(std::size_t n) {
  ++gCount;
  if (gAllow == 0) throw std::bad_alloc();
  if (gAllow > 0) --gAllow;
  if (void* p = std::malloc(n ? n : 1)) return p;
  throw std::bad_alloc();
}
void operator delete(void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }

namespace {

const char* kChat = "1001";
const tx::Profile kProfiles[] = {tx::Profile::Tight, tx::Profile::Lean, tx::Profile::Standard, tx::Profile::Wide};

// "YYYY-MM-DD" for day n after 2026-10-20 (crosses into November and December).
std::string dayAt(int n) {
  static const int kLen[] = {31, 30, 31};   // Oct, Nov, Dec
  int month = 10, day = 20 + n;
  while (day > kLen[month - 10]) {
    day -= kLen[month - 10];
    ++month;
  }
  char b[16];
  std::snprintf(b, sizeof(b), "2026-%02d-%02d", month, day);
  return b;
}

// A digest of a given body size for day n, built the way RB-C2 builds one.
tx::DigestEntry makeDigest(int n, size_t bodyBytes, uint64_t q, uint64_t thru) {
  std::string body = "Day " + std::to_string(n) + ": the owner talked about the boat engine.";
  while (body.size() < bodyBytes) body += " They planned the next trip and logged a meal.";
  body.resize(bodyBytes);
  tx::DigestEntry d;
  d.day = dayAt(n);
  TXB_OK(tx::digestText(d.day, body, d.text));
  d.q = q;
  d.thru = thru;
  return d;
}

std::vector<std::string> linesOf(const std::string& s) {
  std::vector<std::string> out;
  size_t at = 0;
  while (at < s.size()) {
    size_t nl = s.find('\n', at);
    if (nl == std::string::npos) {
      out.push_back(s.substr(at));
      break;
    }
    out.push_back(s.substr(at, nl - at));
    at = nl + 1;
  }
  return out;
}

// The whole lines of every archive file of the chat (a torn tail is not a line).
std::vector<std::string> archivedLines(txb::MemFiles& files) {
  std::vector<std::string> out;
  for (const auto& kv : files.files) {
    if (kv.first.find("/digests-") == std::string::npos) continue;
    const std::string& f = kv.second;
    size_t at = 0;
    for (size_t nl = f.find('\n'); nl != std::string::npos; nl = f.find('\n', at)) {
      out.push_back(f.substr(at, nl - at));
      at = nl + 1;
    }
  }
  return out;
}

// ---- texts ------------------------------------------------------------------------

void test_day_header_carries_the_year() {
  std::string s;
  TXB_OK(tx::dayHeader("2026-11-17", s));
  TEST_ASSERT_EQUAL_STRING("Tue 17 Nov 2026", s.c_str());
  TXB_OK(tx::dayHeader("2027-01-01", s));
  TEST_ASSERT_EQUAL_STRING("Fri 1 Jan 2027", s.c_str());
  TXB_OK(tx::dayHeader("2028-02-29", s));
  TEST_ASSERT_EQUAL_STRING("Tue 29 Feb 2028", s.c_str());
  // RB-REC5: a date is a date whether or not a clock was set at its noon.
  const char* const early[][2] = {{"1970-01-01", "Thu 1 Jan 1970"}, {"1999-05-05", "Wed 5 May 1999"},
                                  {"2001-09-08", "Sat 8 Sep 2001"}, {"2100-03-01", "Mon 1 Mar 2100"}};
  for (const auto& e : early) {
    TXB_OK(tx::dayHeader(e[0], s));
    TEST_ASSERT_EQUAL_STRING(e[1], s.c_str());
  }
  for (const char* bad : {"", "2026-11", "2026-13-01", "2026-02-30", "2027-02-29", "26-11-17", "2026/11/17",
                          "2026-11-1x", "2026-00-10", "2026-11-00"})
    TEST_ASSERT_TRUE_MESSAGE(tx::dayHeader(bad, s).err == tx::Err::InvalidArgument, bad);
}

void test_digest_text_is_one_line_under_its_cap() {
  std::string s;
  TXB_OK(tx::digestText("2026-11-17", "  Talked about\nthe boat.\r\n\tNothing pending.  ", s));
  TEST_ASSERT_EQUAL_STRING("Tue 17 Nov 2026: Talked about the boat. Nothing pending.", s.c_str());
  // Control bytes, U+2028, bidi controls and invalid UTF-8 never survive.
  TXB_OK(tx::digestText("2026-11-17", "a\x01" "b\xE2\x80\xA8" "c\xE2\x80\xAE" "d\xFF" "e caf\xC3\xA9", s));
  TEST_ASSERT_EQUAL_STRING("Tue 17 Nov 2026: a b c d?e caf\xC3\xA9", s.c_str());
  TEST_ASSERT_TRUE(tx::digestText("2026-11-17", " \n\t ", s).err == tx::Err::InvalidArgument);
  TEST_ASSERT_TRUE(tx::digestText("2026-02-30", "x", s).err == tx::Err::InvalidArgument);

  // A long body is cut at the last sentence end that fits, whatever its length.
  std::mt19937 rng(7);
  for (int i = 0; i < 300; ++i) {
    std::string body;
    const size_t want = 100 + rng() % 3000;
    while (body.size() < want) body += (rng() % 4 == 0) ? "Short one. " : "A longer clause with words, ";
    TXB_OK(tx::digestText("2026-11-17", body, s));
    TEST_ASSERT_TRUE(s.size() <= tx::kDigestMaxBytes);
    TEST_ASSERT_TRUE(s.find('\n') == std::string::npos);
    TEST_ASSERT_EQUAL_STRING_LEN("Tue 17 Nov 2026: ", s.c_str(), 17);
    if (body.size() + 17 > tx::kDigestMaxBytes && body.find(". ") < tx::kDigestMaxBytes - 20)
      TEST_ASSERT_EQUAL_CHAR('.', s.back());
  }
  // No sentence end: cut at a space; no space: at a whole character.
  TXB_OK(tx::digestText("2026-11-17", std::string(400, 'a') + " " + std::string(800, 'b'), s));
  TEST_ASSERT_EQUAL(17u + 400u, s.size());
  std::string accents;
  while (accents.size() < 2000) accents += "\xC3\xA9";
  TXB_OK(tx::digestText("2026-11-17", accents, s));
  TEST_ASSERT_TRUE(s.size() <= tx::kDigestMaxBytes && s.size() >= tx::kDigestMaxBytes - 1);
  TEST_ASSERT_EQUAL(0u, (s.size() - 17) % 2);
}

void test_digest_item_round_trips() {
  tx::DigestEntry d = makeDigest(3, 300, 41, 40);
  tx::Item it;
  TXB_OK(tx::digestItem(d, 7, txb::kT0, it));
  TEST_ASSERT_TRUE(it.kind == tx::ItemKind::Digest);
  std::string line;
  TXB_OK(tx::encodeLine(it, line));
  tx::Item back;
  TXB_OK(tx::decodeLine(line.data(), line.size(), back));
  tx::DigestEntry e;
  TXB_OK(tx::digestOf(back, e));
  TEST_ASSERT_EQUAL_STRING(d.text.c_str(), e.text.c_str());
  TEST_ASSERT_EQUAL_STRING(d.day.c_str(), e.day.c_str());
  TEST_ASSERT_EQUAL_UINT64(41, e.q);
  TEST_ASSERT_EQUAL_UINT64(40, e.thru);
  back.ext = true;
  TEST_ASSERT_TRUE(tx::digestOf(back, e).err == tx::Err::InvalidArgument);
  back.ext = false;
  back.kind = tx::ItemKind::User;
  TEST_ASSERT_TRUE(tx::digestOf(back, e).err == tx::Err::InvalidArgument);
  tx::DigestEntry bad = d;
  bad.text += "\nsecond line";
  TEST_ASSERT_TRUE(tx::digestItem(bad, 7, txb::kT0, it).err == tx::Err::InvalidArgument);
}

// ---- the block --------------------------------------------------------------------

void test_block_caps() {
  TEST_ASSERT_EQUAL(2048u, tx::blockCapFor(tx::Profile::Tight));
  TEST_ASSERT_EQUAL(4096u, tx::blockCapFor(tx::Profile::Lean));
  TEST_ASSERT_EQUAL(6144u, tx::blockCapFor(tx::Profile::Standard));
  TEST_ASSERT_EQUAL(6144u, tx::blockCapFor(tx::Profile::Wide));
}

// The class property over 30 appends at every profile and many size mixes.
void test_block_is_stable_over_30_appends() {
  std::mt19937 rng(30);
  for (int run = 0; run < 40; ++run)
    for (tx::Profile p : kProfiles) {
      std::vector<tx::DigestEntry> all;
      std::string prev;
      std::set<std::string> everShown;
      for (int i = 0; i < 30; ++i) {
        all.push_back(makeDigest(i, 100 + rng() % 700, 2 * i + 2, 2 * i + 1));
        std::string block, again;
        std::vector<tx::DigestEntry> archive, archive2;
        TXB_OK(tx::digestBlock(all, p, block, archive));
        TXB_OK(tx::digestBlock(all, p, again, archive2));
        TEST_ASSERT_TRUE_MESSAGE(block == again, "the block is not a pure function of the digests");
        TEST_ASSERT_TRUE(block.size() <= tx::blockCapFor(p));
        // block + archive is every digest once, in order; the archive is the oldest.
        std::vector<std::string> lines = linesOf(block);
        TEST_ASSERT_EQUAL(all.size(), archive.size() + lines.size());
        for (size_t k = 0; k < archive.size(); ++k) TEST_ASSERT_TRUE(archive[k].text == all[k].text);
        for (size_t k = 0; k < lines.size(); ++k) TEST_ASSERT_TRUE(lines[k] == all[archive.size() + k].text);
        // Newest that fit: the next older one would not.
        if (!archive.empty()) TEST_ASSERT_TRUE(block.size() + 1 + archive.back().text.size() > tx::blockCapFor(p));
        // Extended at its end, or (a move to the archive) a line suffix of that.
        const std::string extended = prev.empty() ? all.back().text : prev + "\n" + all.back().text;
        if (block != extended) {
          TEST_ASSERT_TRUE_MESSAGE(extended.size() > block.size() &&
                                       extended.compare(extended.size() - block.size(), block.size(), block) == 0 &&
                                       extended[extended.size() - block.size() - 1] == '\n',
                                   "the block changed other than at an append or an archive move");
        }
        // A digest that left the block never comes back.
        for (const tx::DigestEntry& a : archive)
          if (everShown.count(a.text)) TEST_ASSERT_TRUE(block.find(a.text) == std::string::npos);
        for (const std::string& l : lines) everShown.insert(l);
        prev = block;
      }
    }
}

void test_block_refuses_what_it_cannot_show() {
  std::vector<tx::DigestEntry> d = {makeDigest(1, 200, 3, 2), makeDigest(2, 200, 5, 4)};
  std::string block;
  std::vector<tx::DigestEntry> archive;
  TXB_OK(tx::digestBlock({}, tx::Profile::Lean, block, archive));
  TEST_ASSERT_TRUE(block.empty() && archive.empty());
  auto bad = [&](std::vector<tx::DigestEntry> v, const char* why) {
    block = "x";
    TEST_ASSERT_TRUE_MESSAGE(tx::digestBlock(v, tx::Profile::Lean, block, archive).err == tx::Err::InvalidArgument,
                             why);
    TEST_ASSERT_TRUE_MESSAGE(block.empty() && archive.empty(), why);
  };
  auto v = d;
  v[1].thru = 2;
  bad(v, "thru not rising");
  v = d;
  v[0].text += "\nmore";
  bad(v, "two lines");
  v = d;
  v[0].text = std::string(tx::kDigestMaxBytes + 1, 'a');
  bad(v, "over 768 B");
  v = d;
  v[0].text.clear();
  bad(v, "empty");
  v = d;
  v[0].day = "2026-11-31";
  bad(v, "bad day");
}

// ---- archive, settle and restart over the real log ----------------------------------

// MemFiles with a cut that keeps a chosen part of the cut append: all but its last byte
// (a digest line without its line end) or its first byte only.
struct TearFiles : txb::MemFiles {
  long tearAt = -1;
  bool keepAllButLast = false;
  bool append(const std::string& path, const char* data, size_t len) override {
    if ((long)appends != tearAt) return MemFiles::append(path, data, len);
    ++appends;
    files[path].append(data, keepAllButLast ? len - 1 : 1);
    throw txb::Killed{"tear " + path};
  }
};

struct Chat {
  TearFiles files;
  std::vector<std::string> stored;   // every digest text whose s item was stored
  uint64_t nextQ = 1;
};

// A log append either stores or answers NotStored (a refusal); anything else is a failure.
bool stored(const tx::Status& st) {
  if (st.err == tx::Err::NotStored) return false;
  TXB_OK(st);
  return true;
}

// One compaction step: an owner item, then the s item that covers it.
void appendDay(Chat& c, tx::TxLog& log, int n, size_t bytes, std::vector<tx::DigestEntry>& live, int dayN = -1) {
  tx::Item u;
  u.kind = tx::ItemKind::User;
  u.q = c.nextQ++;
  u.turn = (uint32_t)n + 1;
  u.t = txb::kT0;
  u.x = "message of day " + std::to_string(n);
  u.m = 100 + (uint64_t)n;
  tx::StoreState st = tx::StoreState::NotStored;
  stored(log.append(u, st));
  tx::DigestEntry d = makeDigest(dayN < 0 ? n : dayN, bytes, c.nextQ++, u.q);
  tx::Item s;
  TXB_OK(tx::digestItem(d, u.turn, txb::kT0, s));
  if (!stored(log.append(s, st)) || st != tx::StoreState::Stored) return;
  c.stored.push_back(d.text);
  live.push_back(d);
}

size_t sizeOf(int n) { return 150 + (size_t)(n * 97) % 600; }

void checkNothingLost(Chat& c, const std::string& block, const char* when) {
  std::vector<std::string> arch = archivedLines(c.files);
  std::vector<std::string> shown = linesOf(block);
  for (const std::string& t : c.stored) {
    const long inArch = std::count(arch.begin(), arch.end(), t);
    const long inBlock = std::count(shown.begin(), shown.end(), t);
    TEST_ASSERT_TRUE_MESSAGE(inArch <= 1, when);
    TEST_ASSERT_TRUE_MESSAGE(inArch + inBlock == 1, when);
  }
}

void test_overflow_moves_the_oldest_to_its_month_file() {
  Chat c;
  tx::TxLog log(c.files, kChat);
  tx::HeadState head;
  TXB_OK(log.open(head));
  std::vector<tx::DigestEntry> live;
  std::string block;
  tx::SettleReport rep;
  size_t movedTotal = 0;
  for (int n = 0; n < 40; ++n) {
    appendDay(c, log, n, sizeOf(n), live);
    TXB_OK(tx::settleDigests(c.files, kChat, tx::Profile::Lean, live, block, rep));
    TEST_ASSERT_EQUAL(0u, rep.pending);
    TEST_ASSERT_EQUAL_MESSAGE(linesOf(block).size(), live.size(), "an archived digest stayed live");
    movedTotal += rep.moved;
    TEST_ASSERT_TRUE(block.size() <= tx::kDigestBlockLean);
    checkNothingLost(c, block, "settle");
  }
  TEST_ASSERT_TRUE(movedTotal > 20);
  // Each archived digest sits in the file of its own day's month, in thru order.
  for (const auto& kv : c.files.files) {
    if (kv.first.find("/digests-") == std::string::npos) continue;
    const std::string month = kv.first.substr(kv.first.size() - 10, 7);
    TEST_ASSERT_TRUE(kv.first == "/mem/tx/1001/digests-" + month + ".md");
    for (const std::string& l : linesOf(kv.second)) {
      std::string head;
      bool found = false;
      for (int n = 0; n < 40 && !found; ++n) {
        TXB_OK(tx::dayHeader(dayAt(n), head));
        if (l.rfind(head + ": ", 0) == 0) {
          TEST_ASSERT_EQUAL_STRING(month.c_str(), dayAt(n).substr(0, 7).c_str());
          found = true;
        }
      }
      TEST_ASSERT_TRUE(found);
    }
  }
  TEST_ASSERT_TRUE(c.files.files.count("/mem/tx/1001/digests-2026-10.md") == 1);
  TEST_ASSERT_TRUE(c.files.files.count("/mem/tx/1001/digests-2026-11.md") == 1);
}

void test_archive_is_idempotent_and_heals_a_torn_tail() {
  txb::MemFiles files;
  tx::DigestEntry d = makeDigest(25, 300, 9, 8);
  TXB_OK(tx::archiveDigest(files, kChat, d));
  TXB_OK(tx::archiveDigest(files, kChat, d));
  const std::string path = "/mem/tx/1001/digests-2026-11.md";
  TEST_ASSERT_EQUAL_STRING((d.text + "\n").c_str(), files.files[path].c_str());
  // A torn line of this digest (half an append, or all but its line end) is completed,
  // never repeated.
  tx::DigestEntry e = makeDigest(26, 300, 11, 10);
  for (size_t keep : {(size_t)50, e.text.size()}) {
    files.files[path] = d.text + "\n" + e.text.substr(0, keep);
    TXB_OK(tx::archiveDigest(files, kChat, e));
    TEST_ASSERT_EQUAL_STRING((d.text + "\n" + e.text + "\n").c_str(), files.files[path].c_str());
  }
  // A torn line of another digest is closed first, never glued to this one.
  tx::DigestEntry f = makeDigest(27, 300, 13, 12);
  files.files[path] += f.text.substr(0, 40);
  TXB_OK(tx::archiveDigest(files, kChat, makeDigest(28, 300, 15, 14)));
  std::vector<std::string> lines = archivedLines(files);
  TEST_ASSERT_EQUAL(4u, lines.size());
  TEST_ASSERT_TRUE(lines[2] == f.text.substr(0, 40) && lines[3] == makeDigest(28, 300, 15, 14).text);
  // A refusal answers Io and leaves no whole line behind.
  files.refuseAll = true;
  TEST_ASSERT_TRUE(tx::archiveDigest(files, kChat, makeDigest(29, 300, 17, 16)).err == tx::Err::Io);
  files.refuseAll = false;
  TEST_ASSERT_EQUAL(4u, archivedLines(files).size());
  TEST_ASSERT_TRUE(tx::archiveDigest(files, "BAD", d).err == tx::Err::InvalidArgument);
}

// A cut (clean or torn) or a refusal at every storage step: after a restart and one
// settle, every stored digest is in the block or in the archive, exactly once, and the
// remaining steps keep it so.
void test_nothing_lost_across_a_cut_at_any_step() {
  const int kDays = 18;
  // Count the storage appends of a clean run.
  size_t total = 0;
  {
    Chat c;
    tx::TxLog log(c.files, kChat);
    tx::HeadState head;
    TXB_OK(log.open(head));
    std::vector<tx::DigestEntry> live;
    std::string block;
    tx::SettleReport rep;
    for (int n = 0; n < kDays; ++n) {
      appendDay(c, log, n, sizeOf(n), live);
      TXB_OK(tx::settleDigests(c.files, kChat, tx::Profile::Tight, live, block, rep));
    }
    total = c.files.appends;
  }
  TEST_ASSERT_TRUE(total > 2 * kDays);
  // Modes: 0 a clean cut, 1 a cut that keeps half the append, 2 a refusal, 3 a cut that
  // keeps all but the last byte (the line end), 4 a cut that keeps only the first byte.
  for (int mode = 0; mode < 5; ++mode)
    for (size_t at = 0; at < total; ++at) {
      Chat c;
      if (mode == 2) {
        c.files.refuseAppend = (long)at;
      } else if (mode >= 3) {
        c.files.tearAt = (long)at;
        c.files.keepAllButLast = mode == 3;
      } else {
        c.files.killAtAppend = (long)at;
        c.files.tornKill = mode == 1;
      }
      std::vector<tx::DigestEntry> live;
      std::string block;
      tx::SettleReport rep;
      int n = 0;
      bool killed = false;
      {
        tx::TxLog log(c.files, kChat);
        tx::HeadState head;
        TXB_OK(log.open(head));
        try {
          for (; n < kDays; ++n) {
            appendDay(c, log, n, sizeOf(n), live);
            TXB_OK(tx::settleDigests(c.files, kChat, tx::Profile::Tight, live, block, rep));
          }
        } catch (const txb::Killed&) {
          killed = true;
          ++n;   // the cut day is not retried: its s item may or may not be stored
        }
      }
      c.files.clearFaults();
      c.files.tearAt = -1;
      // Restart: open, load, settle, then the rest of the days.
      tx::TxLog log(c.files, kChat);
      tx::HeadState head;
      TXB_OK(log.open(head));
      c.nextQ = head.nextQ;
      if (killed) {
        // The s item of the cut day counts when its line made it to the segment whole.
        std::vector<tx::DigestEntry> all;
        TXB_OK(tx::loadDigests(c.files, kChat, head, all));
        for (const tx::DigestEntry& d : all)
          if (std::find(c.stored.begin(), c.stored.end(), d.text) == c.stored.end()) c.stored.push_back(d.text);
      }
      live.clear();
      TXB_OK(tx::loadDigests(c.files, kChat, head, live));
      TXB_OK(tx::settleDigests(c.files, kChat, tx::Profile::Tight, live, block, rep));
      TEST_ASSERT_EQUAL(0u, rep.pending);
      char msg[64];
      std::snprintf(msg, sizeof(msg), "mode %d at append %u (after restart)", mode, (unsigned)at);
      checkNothingLost(c, block, msg);
      for (; n < kDays; ++n) {
        appendDay(c, log, n, sizeOf(n), live);
        TXB_OK(tx::settleDigests(c.files, kChat, tx::Profile::Tight, live, block, rep));
      }
      std::snprintf(msg, sizeof(msg), "mode %d at append %u (end)", mode, (unsigned)at);
      checkNothingLost(c, block, msg);
    }
}

// A refused archive append keeps the digest live (pending) and it moves next time.
void test_a_refused_move_stays_live_and_moves_later() {
  Chat c;
  tx::TxLog log(c.files, kChat);
  tx::HeadState head;
  TXB_OK(log.open(head));
  std::vector<tx::DigestEntry> live;
  std::string block;
  tx::SettleReport rep;
  for (int n = 0; n < 6; ++n) {
    appendDay(c, log, n, 700, live);
    c.files.refuseWhen = [](const char*, size_t) { return true; };   // the archive append
    TXB_OK(tx::settleDigests(c.files, kChat, tx::Profile::Tight, live, block, rep));
    c.files.refuseWhen = nullptr;
  }
  TEST_ASSERT_TRUE(rep.pending > 0);
  TEST_ASSERT_TRUE(block.size() <= tx::kDigestBlockTight);
  TEST_ASSERT_EQUAL(c.stored.size(), live.size());
  TXB_OK(tx::settleDigests(c.files, kChat, tx::Profile::Tight, live, block, rep));
  TEST_ASSERT_EQUAL(0u, rep.pending);
  checkNothingLost(c, block, "after the retry");
}

void test_restart_gives_the_same_block_and_stops_at_the_archive() {
  for (tx::Profile p : kProfiles) {
    Chat c;
    std::string block;
    {
      tx::TxLog log(c.files, kChat);
      tx::HeadState head;
      TXB_OK(log.open(head));
      std::vector<tx::DigestEntry> live;
      tx::SettleReport rep;
      for (int n = 0; n < 60; ++n) {
        // Two 55 KB owner messages a day: a segment rolls about every nine days, so the
        // block's digests sit in more than one segment.
        for (int k = 0; k < 2; ++k) {
          tx::Item big;
          big.kind = tx::ItemKind::User;
          big.q = c.nextQ++;
          big.x = std::string(55000, 'a' + n % 26);
          tx::StoreState st;
          TXB_OK(log.append(big, st));
        }
        appendDay(c, log, n, sizeOf(n), live);
        TXB_OK(tx::settleDigests(c.files, kChat, p, live, block, rep));
      }
      head.thru = c.nextQ - 2;
      head.nextQ = c.nextQ;
      TXB_OK(log.writeHead(head));
    }
    tx::TxLog log(c.files, kChat);
    tx::HeadState head;
    TXB_OK(log.open(head));
    std::vector<tx::DigestEntry> live;
    tx::DigestScan scan;
    TXB_OK(tx::loadDigests(c.files, kChat, head, live, &scan));
    TEST_ASSERT_TRUE(scan.reachedArchive);
    if (p != tx::Profile::Tight) TEST_ASSERT_TRUE(scan.segments >= 2);
    TEST_ASSERT_TRUE(scan.segments < c.files.segments("/mem/tx/1001").size());
    std::string again;
    std::vector<tx::DigestEntry> archive;
    TXB_OK(tx::digestBlock(live, p, again, archive));
    TEST_ASSERT_TRUE_MESSAGE(again == block, "a restart changed the block");
    TEST_ASSERT_TRUE(archive.empty());
  }
}

// A clock set back: a later digest belongs to an earlier month and is archived last.
// The restart still finds the same live digests and the same block.
void test_a_clock_set_back_keeps_the_restart_exact() {
  for (int jump = 3; jump < 12; ++jump) {
    Chat c;
    std::vector<tx::DigestEntry> live;
    std::string block;
    {
      tx::TxLog log(c.files, kChat);
      tx::HeadState head;
      TXB_OK(log.open(head));
      tx::SettleReport rep;
      for (int n = 0; n < 16; ++n) {
        // Days run 20 Nov onward, then fall back to October after `jump` days.
        appendDay(c, log, n, 600, live, n < jump ? 31 + n : n - jump);
        TXB_OK(tx::settleDigests(c.files, kChat, tx::Profile::Tight, live, block, rep));
        checkNothingLost(c, block, "clock set back");
      }
      head.thru = c.nextQ - 2;
      head.nextQ = c.nextQ;
      TXB_OK(log.writeHead(head));
    }
    tx::TxLog log(c.files, kChat);
    tx::HeadState head;
    TXB_OK(log.open(head));
    std::vector<tx::DigestEntry> again;
    tx::DigestScan scan;
    TXB_OK(tx::loadDigests(c.files, kChat, head, again, &scan));
    TEST_ASSERT_TRUE(scan.reachedArchive);
    TEST_ASSERT_EQUAL(live.size(), again.size());
    for (size_t i = 0; i < live.size(); ++i) TEST_ASSERT_TRUE(live[i].text == again[i].text);
  }
}

void test_a_chat_without_digests_reads_nothing() {
  txb::MemFiles files;
  tx::HeadState head;
  {
    tx::TxLog log(files, kChat);
    TXB_OK(log.open(head));
    for (uint64_t q = 1; q <= 20; ++q) {
      tx::Item u;
      u.kind = tx::ItemKind::User;
      u.q = q;
      u.x = "no digest yet " + std::to_string(q);
      tx::StoreState st;
      TXB_OK(log.append(u, st));
    }
  }
  tx::TxLog log(files, kChat);
  TXB_OK(log.open(head));
  TEST_ASSERT_EQUAL_UINT64(0, head.thru);
  std::vector<tx::DigestEntry> live = {makeDigest(1, 200, 3, 2)};
  tx::DigestScan scan;
  TXB_OK(tx::loadDigests(files, kChat, head, live, &scan));
  TEST_ASSERT_TRUE(live.empty());
  TEST_ASSERT_EQUAL(0u, scan.reads);
  head.thru = 4;
  struct NoList : txb::MemFiles {
    bool list(const std::string&, std::vector<std::string>&) override { return false; }
  } nolist;
  TEST_ASSERT_TRUE(tx::loadDigests(nolist, kChat, head, live).err == tx::Err::Io);
  TEST_ASSERT_TRUE(tx::loadDigests(files, "Bad/key", head, live).err == tx::Err::InvalidArgument);
}

// ---- recent files ----------------------------------------------------------------

void test_recent_files_list_shape() {
  std::string s;
  TXB_OK(tx::recentFilesList({}, 0, s));
  TEST_ASSERT_TRUE(s.empty());
  std::vector<tx::RecentFile> f = {
      {"notes/boat.md", txb::kT0},
      {"food/2026-11.csv", txb::kT0 + 60},
      {"inbox/a\nb.txt", txb::kT0 - 60},
      {"notes/boat.md", txb::kT0 - 3600},   // the same path again: its newest write counts
      {"x/" + std::string(200, 'y'), txb::kT0 + 120},   // too long: left out
      {"misc/unknown.txt", 0},
      {"", txb::kT0},
      {"bad/ms.md", txb::kT0 * 1000},   // milliseconds passed as seconds: shown without a time
  };
  TXB_OK(tx::recentFilesList(f, 60, s));
  TEST_ASSERT_EQUAL_STRING(
      "bad/ms.md\n"
      "food/2026-11.csv (Tue 17 Nov 2026 14:03)\n"
      "notes/boat.md (Tue 17 Nov 2026 14:02)\n"
      "inbox/a b.txt (Tue 17 Nov 2026 14:01)\n"
      "misc/unknown.txt",
      s.c_str());
  // At most 600 B of whole lines, newest first, and the same bytes for any input order.
  std::mt19937 rng(5);
  for (int run = 0; run < 100; ++run) {
    std::vector<tx::RecentFile> many;
    for (int i = 0; i < 60; ++i)
      many.push_back({"p" + std::to_string(rng() % 40) + "/file" + std::to_string(i % 7) + ".md",
                      txb::kT0 + rng() % 1000});
    std::string a, b;
    TXB_OK(tx::recentFilesList(many, 0, a));
    std::shuffle(many.begin(), many.end(), rng);
    TXB_OK(tx::recentFilesList(many, 0, b));
    TEST_ASSERT_TRUE(a == b);
    TEST_ASSERT_TRUE(a.size() <= tx::kRecentFilesMaxBytes);
    TEST_ASSERT_TRUE(a.size() > tx::kRecentFilesMaxBytes - 60);
    TEST_ASSERT_TRUE(a.back() == ')');
  }
}

void test_changes_file_set() {
  for (const char* t : {"delete_file", "move_file", "rename_file"}) TEST_ASSERT_TRUE_MESSAGE(tx::changesFileSet(t), t);
  for (const char* t : {"write_file", "append_file", "edit_file", "read_file", "", "more", "delete_file2"})
    TEST_ASSERT_FALSE_MESSAGE(tx::changesFileSet(t), t);
}

// Rebuilt at a cold open and at the close of a turn that moved or deleted a file;
// byte-identical at every other point, whatever the store does meanwhile.
void test_recent_files_rebuilt_only_at_its_change_points() {
  std::mt19937 rng(11);
  for (int run = 0; run < 50; ++run) {
    std::vector<tx::RecentFile> store;
    bool readable = true;
    tx::RecentFilesBlock::Source src = [&](std::vector<tx::RecentFile>& out) {
      out = store;
      return readable;
    };
    tx::RecentFilesBlock list;
    std::string expect;
    uint32_t rebuilds = 0;
    for (int step = 0; step < 60; ++step) {
      // The store changes between steps (writes, deletes, moves).
      const int op = rng() % 3;
      if (op == 0 || store.empty())
        store.push_back({"n/f" + std::to_string(rng() % 20) + ".md", txb::kT0 + (uint64_t)step * 60});
      else if (op == 1)
        store.erase(store.begin() + (long)(rng() % store.size()));
      else
        store[rng() % store.size()].path = "moved/f" + std::to_string(step) + ".md";
      readable = rng() % 10 != 0;
      std::string fresh;
      if (readable) TXB_OK(tx::recentFilesList(store, 0, fresh));
      const bool cold = rng() % 3 == 0, moved = rng() % 3 == 0;
      if (rng() % 2) {
        tx::Status st = list.atOpen(cold, src, 0);
        if (cold) {
          ++rebuilds;
          expect = fresh;
          TEST_ASSERT_TRUE(st.ok() == readable);
        } else {
          TXB_OK(st);
        }
      } else {
        tx::Status st = list.atClose(moved, src, 0);
        if (moved) {
          ++rebuilds;
          expect = fresh;
          TEST_ASSERT_TRUE(st.ok() == readable);
        } else {
          TXB_OK(st);
        }
      }
      TEST_ASSERT_TRUE_MESSAGE(list.text() == expect, "the list changed outside its change points");
      TEST_ASSERT_EQUAL(rebuilds, list.rebuilds());
    }
  }
}

// ---- the chatsum mirror ------------------------------------------------------------

struct BlobIO : orch::FoldStoreIO {
  std::string blob;
  std::string load() override { return blob; }
  void save(const std::string& b) override { blob = b; }
};

// The real decoder of the shipped codec (FoldStore::begin of lib/core/src/orch_compact.cpp).
// v5.1.1's begin splits a record the same way (v511Fields below mirrors its split) and its
// capSummary keeps a summary of at most 4,096 B whole, which the test also asserts.
orch::ChatFold decoded(const std::string& blob, const std::string& chat, size_t* chats = nullptr) {
  BlobIO io;
  io.blob = blob;
  orch::FoldStore store;
  store.begin(&io);
  if (chats) *chats = store.chatCount();
  const orch::ChatFold* f = store.peek(chat);
  return f ? *f : orch::ChatFold{};
}

// v5.1.1's field split (its begin(): 7 fields, the summary after the 7th separator).
std::vector<std::string> v511Fields(const std::string& blob, const std::string& chat) {
  size_t start = 0;
  while (start < blob.size()) {
    const size_t end = blob.find('\x1E', start);
    if (end == std::string::npos) break;
    const std::string rec = blob.substr(start, end - start);
    start = end + 1;
    std::vector<std::string> f;
    size_t p = 0;
    while (f.size() < 7) {
      const size_t q = rec.find('\x1F', p);
      if (q == std::string::npos) break;
      f.push_back(rec.substr(p, q - p));
      p = q + 1;
    }
    f.push_back(rec.substr(p));
    if (f.size() == 8 && f[0] == chat) return f;
  }
  return {};
}

std::vector<tx::DigestEntry> digestRun(int count, size_t bytes, int first = 0) {
  std::vector<tx::DigestEntry> v;
  for (int i = 0; i < count; ++i)
    v.push_back(makeDigest(first + i, bytes, 2 * (first + i) + 2, 2 * (first + i) + 1));
  return v;
}

void test_mirror_text_newest_through_thru() {
  std::string s;
  TXB_OK(tx::chatsumMirror({}, 0, s));
  TEST_ASSERT_TRUE(s.empty());
  std::vector<tx::DigestEntry> v = digestRun(12, 700);
  TXB_OK(tx::chatsumMirror(v, 1000, s));
  TEST_ASSERT_TRUE(s.size() <= tx::kChatsumMirrorMaxBytes);
  std::vector<std::string> lines = linesOf(s);
  TEST_ASSERT_TRUE(lines.back() == v.back().text);
  TEST_ASSERT_TRUE(s.size() + 1 + v[v.size() - lines.size() - 1].text.size() > tx::kChatsumMirrorMaxBytes);
  TXB_OK(tx::chatsumMirror(v, v[3].thru, s));
  TEST_ASSERT_TRUE(linesOf(s).back() == v[3].text);
  TXB_OK(tx::chatsumMirror(v, v[0].thru - 1, s));
  TEST_ASSERT_TRUE(s.empty());
}

void test_mirror_is_read_by_the_fold_store_decoder() {
  std::mt19937 rng(51);
  // Other chats as engine 1 wrote them: one paused with failures (v5.1.1), one with lane F's schedule.
  const std::string other1 = std::string("2002\x1F") + "500\x1F" "3\x1F" "1794000000;m00000010\x1F" "1\x1F" "3\x1F" "1\x1F" +
                             "their summary\x1E";
  const std::string other2 = std::string("3003\x1F") + "0\x1F" "0\x1F" "0\x1F" "0\x1F" "0;2;1794000000;1794003600\x1F" "0\x1F" +
                             "another\x1E";
  const std::string mine = std::string("1001\x1F") + "900\x1F" "7\x1F" "1794000000;m00000050\x1F" "4;m00000060\x1F" "3\x1F" "2\x1F" +
                           "old summary of mine\x1E";
  for (int run = 0; run < 200; ++run) {
    std::vector<tx::DigestEntry> v = digestRun(1 + rng() % 20, 120 + rng() % 640);
    tx::MirrorFacts f{"1001", v.back().thru, 0x51 + rng() % 0x1000, (uint32_t)txb::kT0};
    const std::string blob = run % 2 ? other1 + mine + other2 : other1 + other2;
    std::string out, text;
    TXB_OK(tx::mirrorChatsum(blob, f, v, out));
    TXB_OK(tx::chatsumMirror(v, f.thru, text));
    TEST_ASSERT_TRUE(text.size() <= tx::kChatsumMirrorMaxBytes && !text.empty());
    size_t chats = 0;
    orch::ChatFold mf = decoded(out, "1001", &chats);
    TEST_ASSERT_TRUE_MESSAGE(mf.summary == text, "the decoder did not read the mirror text unchanged");
    TEST_ASSERT_TRUE(orch::capSummary(text, tx::kChatsumMirrorMaxBytes) == text);
    char ptr[16];
    std::snprintf(ptr, sizeof(ptr), "m%08x", (unsigned)f.thruRow);
    TEST_ASSERT_EQUAL_STRING(ptr, mf.thru.c_str());
    TEST_ASSERT_TRUE(mf.softStart.empty());
    TEST_ASSERT_EQUAL(0u, mf.bytesSinceFold);
    TEST_ASSERT_EQUAL(0u, mf.failedCycles);
    TEST_ASSERT_EQUAL(0u, mf.nextTry);
    TEST_ASSERT_EQUAL((uint32_t)txb::kT0, mf.lastFoldEpoch);
    TEST_ASSERT_EQUAL(3u, chats);
    // v5.1.1's own fields: failures and pause 0, the summary last.
    std::vector<std::string> raw = v511Fields(out, "1001");
    TEST_ASSERT_EQUAL(8u, raw.size());
    TEST_ASSERT_EQUAL_STRING("0", raw[5].c_str());
    TEST_ASSERT_EQUAL_STRING("0", raw[6].c_str());
    TEST_ASSERT_TRUE(raw[7] == text);
    // Other chats byte for byte, ours newest (last).
    TEST_ASSERT_TRUE(out.find(other1) != std::string::npos && out.find(other2) != std::string::npos);
    TEST_ASSERT_TRUE(out.compare(out.size() - text.size() - 1, text.size() + 1, text + "\x1E") == 0);
    TEST_ASSERT_TRUE(decoded(out, "2002").summary == "their summary");
  }
}

void test_mirror_pointer_only_moves_forward() {
  std::mt19937 rng(9);
  for (int run = 0; run < 50; ++run) {
    std::string blob;
    uint64_t best = 0;
    std::string bestText;
    for (int step = 0; step < 30; ++step) {
      std::vector<tx::DigestEntry> v = digestRun(1 + rng() % 8, 300, (int)(rng() % 10));
      tx::MirrorFacts f{"1001", v.back().thru, rng() % 400, (uint32_t)(txb::kT0 + step)};
      std::string out;
      TXB_OK(tx::mirrorChatsum(blob, f, v, out));
      orch::ChatFold mf = decoded(out, "1001");
      if (f.thruRow >= best) {
        best = f.thruRow;
        TXB_OK(tx::chatsumMirror(v, f.thru, bestText));
        TEST_ASSERT_TRUE(out != blob || mf.summary == bestText);
      } else {
        TEST_ASSERT_TRUE_MESSAGE(out == blob, "a mirror with an older pointer changed the blob");
      }
      if (best) {
        char ptr[16];
        std::snprintf(ptr, sizeof(ptr), "m%08x", (unsigned)best);
        TEST_ASSERT_EQUAL_STRING(ptr, mf.thru.c_str());
      }
      TEST_ASSERT_TRUE(mf.summary == bestText);
      blob = out;
    }
  }
}

void test_mirror_limits_and_refusals() {
  std::vector<tx::DigestEntry> v = digestRun(10, 700);
  tx::MirrorFacts f{"1001", v.back().thru, 0x99, 1};
  std::string out;
  // Twenty other chats with large summaries: at most 16 records and 48 KB, ours kept.
  std::string blob;
  for (int i = 0; i < 20; ++i)
    blob += std::to_string(5000 + i) + "\x1F" "0\x1F" "0\x1F" "0\x1F" "0\x1F" "0\x1F" "0\x1F" + std::string(3900, 'a' + i % 26) +
            "\x1E";
  TXB_OK(tx::mirrorChatsum(blob + "999\x1F" "partial", f, v, out));
  size_t chats = 0;
  TEST_ASSERT_TRUE(!decoded(out, "1001", &chats).summary.empty());
  TEST_ASSERT_TRUE(chats <= tx::kMirrorMaxRecords);
  TEST_ASSERT_TRUE(out.size() <= tx::kMirrorBlobMaxBytes);
  TEST_ASSERT_TRUE(out.find("partial") == std::string::npos);
  TEST_ASSERT_TRUE(decoded(out, "5000").summary.empty());   // the oldest went first
  TEST_ASSERT_FALSE(decoded(out, "5019").summary.empty());
  // Twenty small records: the count limit, ours included, oldest dropped.
  blob.clear();
  for (int i = 0; i < 20; ++i)
    blob += std::to_string(6000 + i) + "\x1F" "0\x1F" "0\x1F" "0\x1F" "0\x1F" "0\x1F" "0\x1F" "s\x1E";
  TXB_OK(tx::mirrorChatsum(blob, f, v, out));
  TEST_ASSERT_EQUAL(16, std::count(out.begin(), out.end(), '\x1E'));
  TEST_ASSERT_TRUE(out.find("6004\x1F") == std::string::npos && out.find("6005\x1F") != std::string::npos);
  // Nothing to mirror: the blob stays as it is.
  TXB_OK(tx::mirrorChatsum("x\x1E", f, {}, out));
  TEST_ASSERT_EQUAL_STRING("x\x1E", out.c_str());
  for (const char* bad : {"", "10\x1F" "01", "10\x1E", "10\n"}) {
    tx::MirrorFacts b = f;
    b.chatId = bad;
    TEST_ASSERT_TRUE(tx::mirrorChatsum("", b, v, out).err == tx::Err::InvalidArgument);
  }
}

void test_row_thru() {
  std::vector<tx::Item> items(5);
  for (size_t i = 0; i < items.size(); ++i) {
    items[i].q = i + 1;
    items[i].kind = i % 2 ? tx::ItemKind::Assistant : tx::ItemKind::User;
    items[i].m = 10 * (i + 1);
  }
  TEST_ASSERT_EQUAL_UINT64(30, tx::rowThru(items, 4));
  TEST_ASSERT_EQUAL_UINT64(50, tx::rowThru(items, 5));
  TEST_ASSERT_EQUAL_UINT64(0, tx::rowThru(items, 0));
}

// Ruling 30: a refused allocation at any point of digestBlock, archiveDigest, loadDigests,
// settleDigests and mirrorChatsum is a TooLarge status with the outputs cleared, never an
// exception, and nothing stored is lost: the same call without the refusal gives the same
// result, and a chat settled under refusals keeps every digest in the block or the archive.
template <class Call, class Check>
void refusedAtEveryAllocation(const char* what, Call call, Check check) {
  bool done = false;
  for (long n = 0; n < 200000 && !done; ++n) {
    gAllow = n;
    tx::Status st;
    try {
      st = call();
    } catch (const std::bad_alloc&) {
      gAllow = -1;
      TEST_FAIL_MESSAGE(what);
    }
    gAllow = -1;
    if (!st.ok()) TEST_ASSERT_TRUE_MESSAGE(st.err == tx::Err::TooLarge, what);
    check(st.ok());
    done = st.ok();
  }
  TEST_ASSERT_TRUE_MESSAGE(done, what);
}

void test_a_refused_allocation_is_a_status_and_loses_nothing() {
  // digestBlock
  std::vector<tx::DigestEntry> all = digestRun(30, 400);
  std::string ref, block;
  std::vector<tx::DigestEntry> refArch, arch;
  TXB_OK(tx::digestBlock(all, tx::Profile::Lean, ref, refArch));
  refusedAtEveryAllocation(
      "digestBlock", [&] { return tx::digestBlock(all, tx::Profile::Lean, block, arch); },
      [&](bool ok) {
        TEST_ASSERT_TRUE(ok ? block == ref && arch.size() == refArch.size() : block.empty() && arch.empty());
      });
  // mirrorChatsum
  const std::string blob = std::string("2002\x1F") + "0\x1F" "0\x1F" "0\x1F" "0\x1F" "0\x1F" "0\x1F" "theirs\x1E";
  tx::MirrorFacts f{"1001", all.back().thru, 0x99, 1};
  std::string refBlob, out;
  TXB_OK(tx::mirrorChatsum(blob, f, all, refBlob));
  refusedAtEveryAllocation(
      "mirrorChatsum", [&] { return tx::mirrorChatsum(blob, f, all, out); },
      [&](bool ok) { TEST_ASSERT_TRUE(ok ? out == refBlob : out.empty()); });
  // archiveDigest: the month file is unchanged by a refusal, one whole line more after.
  txb::MemFiles files;
  TXB_OK(tx::archiveDigest(files, kChat, all[0]));
  const std::string path = "/mem/tx/1001/digests-2026-10.md";
  const std::string before = files.files[path];
  refusedAtEveryAllocation(
      "archiveDigest", [&] { return tx::archiveDigest(files, kChat, all[1]); },
      [&](bool ok) { TEST_ASSERT_TRUE(files.files[path] == (ok ? before + all[1].text + "\n" : before)); });
  // loadDigests and settleDigests over a chat with an archive.
  Chat c;
  std::vector<tx::DigestEntry> live;
  tx::HeadState head;
  {
    tx::TxLog log(c.files, kChat);
    TXB_OK(log.open(head));
    tx::SettleReport rep;
    for (int n = 0; n < 24; ++n) {
      appendDay(c, log, n, sizeOf(n), live);
      // A refusal somewhere inside each settle; the digests it did not move stay live.
      gAllow = (long)(n * 37) % 300;
      tx::Status st = tx::settleDigests(c.files, kChat, tx::Profile::Tight, live, block, rep);
      gAllow = -1;
      TEST_ASSERT_TRUE(st.ok() || st.err == tx::Err::TooLarge);
    }
    TXB_OK(tx::settleDigests(c.files, kChat, tx::Profile::Tight, live, block, rep));
    checkNothingLost(c, block, "settled under refusals");
    // A refusal inside the archive move (after the block's own allocations) is a refused
    // move: settle answers ok, the digest stays live and pending.
    for (int n = 24; n < 30; ++n) {
      appendDay(c, log, n, 700, live);
      std::string b;
      std::vector<tx::DigestEntry> a;
      const long before = gCount;
      TXB_OK(tx::digestBlock(live, tx::Profile::Tight, b, a));
      gAllow = gCount - before + 1;
      tx::Status st = tx::settleDigests(c.files, kChat, tx::Profile::Tight, live, block, rep);
      gAllow = -1;
      TXB_OK(st);
      TEST_ASSERT_TRUE(rep.pending > 0);
      TEST_ASSERT_TRUE(block == b);
    }
    TXB_OK(tx::settleDigests(c.files, kChat, tx::Profile::Tight, live, block, rep));
    checkNothingLost(c, block, "after refused moves");
    head.thru = c.nextQ - 2;
    head.nextQ = c.nextQ;
    TXB_OK(log.writeHead(head));
  }
  tx::TxLog log(c.files, kChat);
  TXB_OK(log.open(head));
  std::vector<tx::DigestEntry> refLive, got = {all[0]};
  TXB_OK(tx::loadDigests(c.files, kChat, head, refLive));
  refusedAtEveryAllocation(
      "loadDigests", [&] { return tx::loadDigests(c.files, kChat, head, got); },
      [&](bool ok) {
        TEST_ASSERT_EQUAL(ok ? refLive.size() : 0u, got.size());
        for (size_t i = 0; ok && i < got.size(); ++i) TEST_ASSERT_TRUE(got[i].text == refLive[i].text);
      });
}


// ---- the cumulative summary (owner rulings 105 and 106; lane RB-C3) ------------------------

tx::Summary sampleSummary() {
  tx::Summary s;
  s.lines = {{tx::SummarySection::Facts, 1, "The person sails a boat named Tern."},
             {tx::SummarySection::Facts, 2, "Prefers short replies."},
             {tx::SummarySection::Decisions, 3, "Chose blue paint for the hull."},
             {tx::SummarySection::Quantities, 4, "Paint: 3 liters bought on Tue 17 Nov 2026."},
             {tx::SummarySection::Open, 5, "Call the marina about the berth by Fri 27 Nov 2026."},
             {tx::SummarySection::Files, 6, "notes/boat.md (Tue 17 Nov 2026 13:02)"}};
  return s;
}

// Is the line with this id present, and with exactly this text?
const tx::SummaryLine* lineOf(const tx::Summary& s, uint32_t id) {
  for (const tx::SummaryLine& l : s.lines)
    if (l.id == id) return &l;
  return nullptr;
}

void test_summary_round_trips_and_is_told_from_a_day_digest() {
  tx::Summary s = sampleSummary();
  s.archived = true;
  std::string text;
  TXB_OK(tx::encodeSummary(s, text));
  TEST_ASSERT_TRUE(tx::isSummaryText(text));
  TEST_ASSERT_EQUAL('\n', text.back());
  TEST_ASSERT_TRUE(text.find(std::string(tx::digest_text::kFacts) + "\nF1 The person sails") == 0);
  TEST_ASSERT_TRUE(text.find("\nP6 notes/boat.md") != std::string::npos);
  tx::Summary back;
  TXB_OK(tx::parseSummary(text, back));
  TEST_ASSERT_TRUE(back.archived);
  TEST_ASSERT_EQUAL(s.lines.size(), back.lines.size());
  for (size_t i = 0; i < s.lines.size(); ++i) {
    TEST_ASSERT_EQUAL(s.lines[i].id, back.lines[i].id);
    TEST_ASSERT_EQUAL((int)s.lines[i].section, (int)back.lines[i].section);
    TEST_ASSERT_TRUE(s.lines[i].text == back.lines[i].text);
  }
  TEST_ASSERT_EQUAL(7u, back.nextId());
  // An empty summary still has a line; a day digest is never a summary.
  std::string empty;
  TXB_OK(tx::encodeSummary(tx::Summary{}, empty));
  TEST_ASSERT_TRUE(tx::isSummaryText(empty));
  TXB_OK(tx::parseSummary(empty, back));
  TEST_ASSERT_TRUE(back.lines.empty());
  const tx::DigestEntry day = makeDigest(1, 300, 9, 8);
  TEST_ASSERT_FALSE(tx::isSummaryText(day.text));
  TEST_ASSERT_TRUE(tx::parseSummary(day.text, back).err == tx::Err::InvalidArgument);
  // Counter-tests: a line out of its section, an id used twice, a broken line.
  tx::Summary bad = sampleSummary();
  bad.lines[1].id = 1;
  TEST_ASSERT_TRUE(tx::encodeSummary(bad, text).err == tx::Err::InvalidArgument);
  bad = sampleSummary();
  std::swap(bad.lines[0], bad.lines[2]);
  TEST_ASSERT_TRUE(tx::encodeSummary(bad, text).err == tx::Err::InvalidArgument);
  bad = sampleSummary();
  bad.lines[0].text = "two\nlines";
  TEST_ASSERT_TRUE(tx::encodeSummary(bad, text).err == tx::Err::InvalidArgument);
  TEST_ASSERT_TRUE(tx::parseSummary(std::string(tx::digest_text::kFacts) + "\nQ1 wrong letter\n", back).err ==
                   tx::Err::InvalidArgument);
}

void test_summary_edits_are_applied_by_code_and_an_untouched_line_stays_byte_for_byte() {
  tx::Summary s = sampleSummary();
  const tx::SummaryEdits e = tx::parseSummaryEdits(
      "+F Has a dog named Pip.\n"
      "  * =Q4 Paint: 4 liters, one more bought on Wed 18 Nov 2026.\n"
      "-o5\n"
      "Here is the updated summary:\n"
      "-P6\n"
      "+P notes/other.md\n"
      "+d Will repaint in spring.\n");
  TEST_ASSERT_EQUAL(5u, e.edits.size());   // the Files edits parse; apply refuses them
  TEST_ASSERT_EQUAL(2u, e.unread);         // the prose line and "+P"
  TEST_ASSERT_FALSE(e.none);
  std::vector<tx::SummaryLine> left;
  size_t unread = 0;
  TXB_OK(tx::applySummaryEdits(s, e.edits, nullptr, left, unread));
  TEST_ASSERT_EQUAL(1u, unread);           // -P6: the device's own line
  TEST_ASSERT_TRUE(lineOf(s, 1)->text == "The person sails a boat named Tern.");
  TEST_ASSERT_TRUE(lineOf(s, 2)->text == "Prefers short replies.");
  TEST_ASSERT_TRUE(lineOf(s, 4)->text == "Paint: 4 liters, one more bought on Wed 18 Nov 2026.");
  TEST_ASSERT_NULL(lineOf(s, 5));
  TEST_ASSERT_NOT_NULL(lineOf(s, 6));
  TEST_ASSERT_EQUAL(2u, left.size());      // the old Q4 and the dropped O5, to the archive
  TEST_ASSERT_TRUE(left[0].text == "Paint: 3 liters bought on Tue 17 Nov 2026.");
  TEST_ASSERT_EQUAL(5u, left[1].id);
  std::string text;
  TXB_OK(tx::encodeSummary(s, text));
  TEST_ASSERT_TRUE(text.find("F7 Has a dog named Pip.") != std::string::npos);
  TEST_ASSERT_TRUE(text.find("D8 Will repaint in spring.") != std::string::npos);
  // "=" is an answer that changes nothing; a whole rewritten summary is no change at all.
  TEST_ASSERT_TRUE(tx::parseSummaryEdits(" =\n").none);
  const tx::SummaryEdits rewrite = tx::parseSummaryEdits(text);
  TEST_ASSERT_TRUE(rewrite.edits.empty());
  TEST_ASSERT_FALSE(rewrite.none);
  TEST_ASSERT_TRUE(rewrite.unread > 0);
  // An add already in its section is not added twice; an unknown id is unread.
  left.clear();
  TXB_OK(tx::applySummaryEdits(s, tx::parseSummaryEdits("+F Prefers short replies.\n=F99 nope\n").edits, nullptr, left,
                               unread));
  TEST_ASSERT_EQUAL(1u, unread);
  std::string again;
  TXB_OK(tx::encodeSummary(s, again));
  TEST_ASSERT_TRUE(again == text);
}

std::string framed(const std::string& clean) { return "<data>" + clean + "</data>"; }

void test_lines_from_a_slice_that_read_outside_content_are_framed() {
  tx::Summary s;
  std::vector<tx::SummaryLine> left;
  size_t unread = 0;
  TXB_OK(tx::applySummaryEdits(s, tx::parseSummaryEdits("+F The article said the tide is at 6.\n").edits, framed,
                               left, unread));
  TEST_ASSERT_TRUE(s.lines[0].text == "<data>The article said the tide is at 6.</data>");
  TEST_ASSERT_TRUE(s.lines[0].text.size() <= tx::kSummaryLineMaxBytes);
}

// The class rule (ruling 106 item 2): over any set of changes that does not name a line, the
// line stays byte for byte, through any number of updates.
void test_a_line_no_change_names_survives_every_update() {
  std::mt19937 rng(106);
  tx::Summary s = sampleSummary();
  const std::string keep = lineOf(s, 1)->text;
  const char letters[] = "FDQO";
  for (int round = 0; round < 300; ++round) {
    std::string answer;
    for (int k = 0; k < 6; ++k) {
      const int kind = (int)(rng() % 3);
      uint32_t id = 2 + (uint32_t)(rng() % (s.nextId() + 3));   // never 1
      if (kind == 0) answer += std::string("+") + letters[rng() % 4] + " fact " + std::to_string(rng() % 1000) + "\n";
      if (kind == 1) answer += std::string("=") + letters[rng() % 4] + std::to_string(id) + " changed " + std::to_string(round) + "\n";
      if (kind == 2) answer += std::string("-") + letters[rng() % 4] + std::to_string(id) + "\n";
    }
    std::vector<tx::SummaryLine> left;
    size_t unread = 0;
    TXB_OK(tx::applySummaryEdits(s, tx::parseSummaryEdits(answer).edits, nullptr, left, unread));
    TXB_OK(tx::fitSummary(s, tx::kSummaryMaxBytes, left));
    std::string text;
    TXB_OK(tx::encodeSummary(s, text));
    tx::Summary back;
    TXB_OK(tx::parseSummary(text, back));
    s = back;
    TEST_ASSERT_NOT_NULL_MESSAGE(lineOf(s, 1), std::to_string(round).c_str());
    TEST_ASSERT_TRUE(lineOf(s, 1)->text == keep);
  }
}

size_t encodedSize(const tx::Summary& s) {
  std::string t;
  TXB_OK(tx::encodeSummary(s, t));
  return t.size();
}

void test_over_its_budget_the_section_furthest_over_its_share_loses_its_oldest_line() {
  tx::Summary s;
  uint32_t id = 1;
  for (int i = 0; i < 40; ++i)   // decisions far over their 20% share
    s.lines.push_back({tx::SummarySection::Decisions, id++, "Decision number " + std::to_string(i) + " was made."});
  for (int i = 0; i < 3; ++i) s.lines.push_back({tx::SummarySection::Open, id++, "Open item " + std::to_string(i) + "."});
  const size_t cap = encodedSize(s) / 2;
  std::vector<tx::SummaryLine> left;
  TXB_OK(tx::fitSummary(s, cap, left));
  TEST_ASSERT_TRUE(encodedSize(s) <= cap);
  TEST_ASSERT_TRUE(s.archived);
  TEST_ASSERT_FALSE(left.empty());
  for (size_t i = 0; i < left.size(); ++i) {   // the oldest decisions, in order; no open item
    TEST_ASSERT_EQUAL((int)tx::SummarySection::Decisions, (int)left[i].section);
    TEST_ASSERT_EQUAL(i + 1, left[i].id);
  }
  // Counter-test: under its budget nothing leaves and nothing is marked.
  tx::Summary small = sampleSummary();
  left.clear();
  TXB_OK(tx::fitSummary(small, tx::kSummaryMaxBytes, left));
  TEST_ASSERT_TRUE(left.empty());
  TEST_ASSERT_FALSE(small.archived);
}

void test_the_device_writes_the_files_lines() {
  tx::Summary s = sampleSummary();
  std::vector<tx::SummaryLine> left;
  tx::noteSummaryFile(s, "notes/boat.md", "Wed 18 Nov 2026 09:00", false, left);
  TEST_ASSERT_EQUAL(1u, left.size());
  TEST_ASSERT_TRUE(s.lines.back().text == "notes/boat.md (Wed 18 Nov 2026 09:00)");
  TEST_ASSERT_EQUAL(6u, s.lines.back().id);
  tx::noteSummaryFile(s, "logs/fuel.csv", "", false, left);
  TEST_ASSERT_TRUE(s.lines.back().text == "logs/fuel.csv");
  tx::noteSummaryFile(s, "notes/boat.md", "", true, left);
  TEST_ASSERT_EQUAL(2u, left.size());
  for (const tx::SummaryLine& l : s.lines) TEST_ASSERT_TRUE(l.text.find("boat.md") == std::string::npos);
  tx::noteSummaryFile(s, "bad\npath", "", false, left);
  TEST_ASSERT_EQUAL(2u, left.size());
  TEST_ASSERT_TRUE(s.lines.back().text == "logs/fuel.csv");
}

void test_lines_that_leave_are_archived_with_their_day() {
  Chat c;
  std::vector<tx::SummaryLine> left = {{tx::SummarySection::Quantities, 4, "Paint: 3 liters."},
                                       {tx::SummarySection::Open, 5, "Call the marina."}};
  TXB_OK(tx::archiveSummaryLines(c.files, kChat, "2026-11-18", left));
  TXB_OK(tx::archiveSummaryLines(c.files, kChat, "2026-11-18", left));   // once
  const std::vector<std::string> arch = archivedLines(c.files);
  TEST_ASSERT_EQUAL(2u, arch.size());
  TEST_ASSERT_TRUE(arch[0] == "Wed 18 Nov 2026: Q4 Paint: 3 liters.");
  TEST_ASSERT_TRUE(arch[1] == "Wed 18 Nov 2026: O5 Call the marina.");
  c.files.refuseAll = true;
  TEST_ASSERT_TRUE(tx::archiveSummaryLines(c.files, kChat, "2026-12-01", left).err == tx::Err::Io);
}

tx::DigestEntry summaryEntry(const tx::Summary& s, int dayN, uint64_t q, uint64_t thru) {
  tx::DigestEntry d;
  d.day = dayAt(dayN);
  TXB_OK(tx::encodeSummary(s, d.text));
  d.q = q;
  d.thru = thru;
  return d;
}

void test_the_block_is_the_newest_summary_and_settle_archives_the_day_digests_before_it() {
  Chat c;
  std::vector<tx::DigestEntry> live = digestRun(4, 300);
  tx::Summary s1 = sampleSummary(), s2 = sampleSummary();
  s2.lines.insert(s2.lines.end() - 1, {tx::SummarySection::Open, 9, "Book the haul-out."});   // before Files
  live.push_back(summaryEntry(s1, 10, live.back().q + 2, live.back().q + 1));
  live.push_back(summaryEntry(s2, 11, live.back().q + 2, live.back().q + 1));
  std::string block;
  std::vector<tx::DigestEntry> archive;
  TXB_OK(tx::digestBlock(live, tx::Profile::Full, block, archive));
  TEST_ASSERT_TRUE(block + "\n" == live.back().text);   // the newest summary alone, no line end
  TEST_ASSERT_EQUAL(4u, archive.size());               // the day digests; the older summary is superseded
  // A tight profile never cuts the summary: its budget is its own.
  TXB_OK(tx::digestBlock(live, tx::Profile::Tight, block, archive));
  TEST_ASSERT_TRUE(block + "\n" == live.back().text);
  // A refused archive keeps the day digests live (they move later); never lost.
  tx::SettleReport rep;
  c.files.refuseAll = true;
  std::vector<tx::DigestEntry> kept = live;
  TXB_OK(tx::settleDigests(c.files, kChat, tx::Profile::Full, kept, block, rep));
  TEST_ASSERT_EQUAL(4u, rep.pending);
  c.files.refuseAll = false;
  TXB_OK(tx::settleDigests(c.files, kChat, tx::Profile::Full, live, block, rep));
  TEST_ASSERT_EQUAL(0u, rep.pending);
  TEST_ASSERT_EQUAL(1u, live.size());
  TEST_ASSERT_TRUE(live[0].text == summaryEntry(s2, 11, 0, 0).text);
  TEST_ASSERT_EQUAL(4u, archivedLines(c.files).size());
}

void test_a_summary_is_valid_up_to_its_hard_cap_only() {
  tx::Summary s;
  uint32_t id = 1;
  std::string text;
  while (tx::encodeSummary(s, text).ok())
    s.lines.push_back({tx::SummarySection::Facts, id++, std::string(300, 'a' + (char)(id % 26))});
  TEST_ASSERT_TRUE(tx::encodeSummary(s, text).err == tx::Err::InvalidArgument);
  s.lines.pop_back();
  TXB_OK(tx::encodeSummary(s, text));
  tx::DigestEntry d{dayAt(3), text, 20, 19};
  tx::Item it;
  TXB_OK(tx::digestItem(d, 0, txb::kT0, it));
  tx::DigestEntry back;
  TXB_OK(tx::digestOf(it, back));
  TEST_ASSERT_TRUE(back.text == text);
  d.text = text + std::string(tx::kSummaryMaxBytes, 'x') + "\n";
  TEST_ASSERT_TRUE(tx::digestItem(d, 0, txb::kT0, it).err == tx::Err::InvalidArgument);
}

void test_a_restart_stops_at_the_newest_summary() {
  Chat c;
  {
    tx::TxLog log(c.files, kChat);
    tx::HeadState head;
    TXB_OK(log.open(head));
    std::vector<tx::DigestEntry> live;
    std::string block;
    tx::SettleReport rep;
    for (int n = 0; n < 30; ++n) {
      for (int k = 0; k < 2; ++k) {   // segments roll: the summaries sit in later segments
        tx::Item big;
        big.kind = tx::ItemKind::User;
        big.q = c.nextQ++;
        big.x = std::string(55000, 'a' + n % 26);
        tx::StoreState st;
        TXB_OK(log.append(big, st));
      }
      appendDay(c, log, n, sizeOf(n), live);
    }
    TXB_OK(tx::settleDigests(c.files, kChat, tx::Profile::Full, live, block, rep));
    for (int k = 0; k < 2; ++k) {
      tx::Summary s = sampleSummary();
      s.lines.insert(s.lines.end() - 1, {tx::SummarySection::Open, 20u + (uint32_t)k, "Version " + std::to_string(k) + "."});
      tx::DigestEntry d = summaryEntry(s, 31 + k, c.nextQ + 1, c.nextQ);
      c.nextQ += 2;
      tx::Item it;
      TXB_OK(tx::digestItem(d, 0, txb::kT0, it));
      tx::StoreState st;
      TXB_OK(log.append(it, st));
      live.push_back(d);
    }
    TXB_OK(tx::settleDigests(c.files, kChat, tx::Profile::Full, live, block, rep));
    TEST_ASSERT_EQUAL(1u, live.size());
    head.thru = c.nextQ - 2;
    head.nextQ = c.nextQ;
    TXB_OK(log.writeHead(head));
  }
  tx::TxLog log(c.files, kChat);
  tx::HeadState head;
  TXB_OK(log.open(head));
  std::vector<tx::DigestEntry> live;
  tx::DigestScan scan;
  TXB_OK(tx::loadDigests(c.files, kChat, head, live, &scan));
  TEST_ASSERT_EQUAL(1u, live.size());
  TEST_ASSERT_TRUE(live[0].text.find("O21 Version 1.") != std::string::npos);
  TEST_ASSERT_EQUAL(1u, scan.segments);   // the newest summary is in the newest segment
}

void test_the_mirror_carries_the_summary_in_whole_lines() {
  tx::Summary s;
  for (uint32_t i = 1; i <= 60; ++i)
    s.lines.push_back({tx::SummarySection::Facts, i, "Fact " + std::to_string(i) + std::string(100, '.')});
  std::vector<tx::DigestEntry> v = digestRun(2, 300);
  v.push_back(summaryEntry(s, 5, v.back().q + 2, v.back().q + 1));
  std::string m;
  TXB_OK(tx::chatsumMirror(v, v.back().thru, m));
  TEST_ASSERT_TRUE(m.size() <= tx::kChatsumMirrorMaxBytes && m.size() > tx::kChatsumMirrorMaxBytes / 2);
  TEST_ASSERT_TRUE(v.back().text.compare(0, m.size(), m) == 0);
  TEST_ASSERT_EQUAL('\n', v.back().text[m.size()]);
  // Before the summary's thru the day digests are the mirror, as before.
  TXB_OK(tx::chatsumMirror(v, v[1].thru, m));
  TEST_ASSERT_TRUE(linesOf(m).back() == v[1].text);
}

}  // namespace

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_day_header_carries_the_year);
  RUN_TEST(test_digest_text_is_one_line_under_its_cap);
  RUN_TEST(test_digest_item_round_trips);
  RUN_TEST(test_block_caps);
  RUN_TEST(test_block_is_stable_over_30_appends);
  RUN_TEST(test_block_refuses_what_it_cannot_show);
  RUN_TEST(test_overflow_moves_the_oldest_to_its_month_file);
  RUN_TEST(test_archive_is_idempotent_and_heals_a_torn_tail);
  RUN_TEST(test_nothing_lost_across_a_cut_at_any_step);
  RUN_TEST(test_a_refused_move_stays_live_and_moves_later);
  RUN_TEST(test_restart_gives_the_same_block_and_stops_at_the_archive);
  RUN_TEST(test_a_clock_set_back_keeps_the_restart_exact);
  RUN_TEST(test_a_chat_without_digests_reads_nothing);
  RUN_TEST(test_recent_files_list_shape);
  RUN_TEST(test_changes_file_set);
  RUN_TEST(test_recent_files_rebuilt_only_at_its_change_points);
  RUN_TEST(test_mirror_text_newest_through_thru);
  RUN_TEST(test_mirror_is_read_by_the_fold_store_decoder);
  RUN_TEST(test_mirror_pointer_only_moves_forward);
  RUN_TEST(test_mirror_limits_and_refusals);
  RUN_TEST(test_row_thru);
  RUN_TEST(test_a_refused_allocation_is_a_status_and_loses_nothing);
  RUN_TEST(test_summary_round_trips_and_is_told_from_a_day_digest);
  RUN_TEST(test_summary_edits_are_applied_by_code_and_an_untouched_line_stays_byte_for_byte);
  RUN_TEST(test_lines_from_a_slice_that_read_outside_content_are_framed);
  RUN_TEST(test_a_line_no_change_names_survives_every_update);
  RUN_TEST(test_over_its_budget_the_section_furthest_over_its_share_loses_its_oldest_line);
  RUN_TEST(test_the_device_writes_the_files_lines);
  RUN_TEST(test_lines_that_leave_are_archived_with_their_day);
  RUN_TEST(test_the_block_is_the_newest_summary_and_settle_archives_the_day_digests_before_it);
  RUN_TEST(test_a_summary_is_valid_up_to_its_hard_cap_only);
  RUN_TEST(test_a_restart_stops_at_the_newest_summary);
  RUN_TEST(test_the_mirror_carries_the_summary_in_whole_lines);
  return UNITY_END();
}
