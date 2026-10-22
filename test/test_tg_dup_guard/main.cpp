#include <unity.h>

#include <cstdint>
#include <cstring>
#include <random>
#include <string>
#include <vector>

#include "nimbus/net/tg_dup_guard.h"
#include "nimbus/sys/reset_reason.h"

// The inbound storm breakers (nimbus/net/tg_dup_guard.h), as the receive path drives
// them: classify -> check; a duplicate is dropped (noteDuplicate, maybe a notice); a
// new message is taken (notePending), runs one turn unless the hourly ceiling says
// no, and is remembered once its turn returned (noteTurned). The 19 Oct storm class:
// K identical pages cost exactly one paid turn, one owner notice and a duplicate
// counter of K-1, across restarts; K distinct messages run until the hourly ceiling;
// a genuine repeat runs. Plus the poison guard and the record's garbage rejection.

using namespace nimbus::net;

void setUp() {}
void tearDown() {}

static constexpr uint32_t kBuild = 0xB0B0CAFEu;
static constexpr int64_t kT0 = 1792400000;   // Telegram date of the first message

static DupKey textKey(const char* chat, int64_t mid, int64_t date, const std::string& text,
                      bool command = false) {
  DupKey k;
  k.chat = dupHash(std::string(chat));
  k.messageId = mid;
  k.date = date;
  k.content = contentKeyText(text);
  k.command = command;
  return k;
}

// The receive path over one page of keys, reduced to the breaker calls. Counts paid
// turns, owner notices and skips the way telegram.cpp does.
struct Device {
  TgGuardState rtc{};   // survives the restarts below unless a power-on clears it
  TgGuard guard{rtc};
  int paid = 0, dupNotices = 0, ceilingNotices = 0, ceilingSkips = 0;

  Device() { guard.boot(BootKind::PowerOn, kBuild); }
  // A new boot: the same RTC bytes; boot() drops the RAM-only pending list.
  void restart(BootKind kind) { guard.boot(kind, kBuild); }
  // One page, one turn per message (the batching is test_tg_burst's). `owner`: the
  // page is the owner's chat (its limit is the whole ceiling).
  void page(const std::vector<DupKey>& keys, uint32_t nowS, bool owner = true) {
    std::vector<DupKey> taken;
    for (const DupKey& k : keys) {
      if (guard.check(k) != DupVerdict::New) {
        if (guard.noteDuplicate(k)) ++dupNotices;
        continue;
      }
      guard.notePending(k);
      taken.push_back(k);
    }
    for (const DupKey& k : taken) {
      if (!k.command && !guard.ceilingAllows(nowS, owner)) {
        ++ceilingSkips;
        if (guard.noteCeilingSkip(nowS, owner)) ++ceilingNotices;
        continue;
      }
      if (!k.command) {
        guard.noteTurn(nowS);
        ++paid;
      }
      guard.noteTurned(k, nowS);
    }
    guard.clearPending();
  }
};

// K identical pages (the same message re-served), K up to 1,000: one paid turn, one
// notice, K-1 duplicates. The answer goes out before the second page.
static void test_k_identical_pages_cost_one_turn() {
  for (int K : {1, 2, 3, 10, 50, 999, 1000}) {
    Device d;
    const DupKey k = textKey("77", 9001, kT0, "what's on my calendar?");
    for (int i = 0; i < K; ++i) d.page({k}, (uint32_t)(kT0 + 1 + i));
    TEST_ASSERT_EQUAL_INT(1, d.paid);
    TEST_ASSERT_EQUAL_INT(K > 1 ? 1 : 0, d.dupNotices);
    TEST_ASSERT_EQUAL_UINT32((uint32_t)(K - 1), d.guard.dupDrops());
  }
}

// Reboot mid-episode: the record survives a restart, a panic, a watchdog and a
// brownout (RTC no-init memory), so the episode stays at one turn and one notice.
// Only a power-on forgets it (then the message may run once more: at-least-once).
static void test_restart_mid_episode_keeps_one_turn() {
  for (BootKind kind : {BootKind::Other, BootKind::Crash}) {
    Device d;
    const DupKey k = textKey("77", 9002, kT0, "hello");
    for (int i = 0; i < 25; ++i) d.page({k}, (uint32_t)(kT0 + i));
    d.restart(kind);
    for (int i = 0; i < 25; ++i) d.page({k}, (uint32_t)(kT0 + 30 + i));
    TEST_ASSERT_EQUAL_INT(1, d.paid);
    TEST_ASSERT_EQUAL_INT(1, d.dupNotices);
    TEST_ASSERT_EQUAL_UINT32(49, d.guard.dupDrops());
  }
  Device p;
  const DupKey k = textKey("77", 9003, kT0, "hello");
  p.page({k}, kT0);
  p.restart(BootKind::PowerOn);
  p.page({k}, kT0 + 5);
  TEST_ASSERT_EQUAL_INT(2, p.paid);
}

// A fresh device (zeroed or garbage RTC bytes, any reset reason): the record is
// rebuilt and the first message runs.
static void test_fresh_device_runs_the_first_message() {
  for (int fill : {0x00, 0xFF, 0x5A}) {
    for (BootKind kind : {BootKind::PowerOn, BootKind::Other, BootKind::Crash}) {
      TgGuardState rtc;
      std::memset(&rtc, fill, sizeof rtc);
      TgGuard g(rtc);
      TEST_ASSERT_FALSE(g.boot(kind, kBuild));
      TEST_ASSERT_TRUE(g.valid(kBuild));
      TEST_ASSERT_EQUAL_INT((int)DupVerdict::New, (int)g.check(textKey("5", 1, kT0, "hi")));
      TEST_ASSERT_EQUAL_UINT32(0, g.dupDrops());
      TEST_ASSERT_TRUE(g.ceilingAllows(kT0, true));
      TEST_ASSERT_TRUE(g.ceilingAllows(kT0, false));
      TEST_ASSERT_FALSE(g.poisoned(10, kPhasePage));
    }
  }
}

// The record from another build, or with one flipped byte, is rejected.
static void test_record_rejects_other_builds_and_corruption() {
  TgGuardState rtc{};
  TgGuard g(rtc);
  g.boot(BootKind::PowerOn, kBuild);
  g.noteTurned(textKey("5", 1, kT0, "hi"), kT0);
  TEST_ASSERT_TRUE(g.valid(kBuild));
  TEST_ASSERT_FALSE(g.valid(kBuild + 1));
  TgGuardState copy = rtc;
  TgGuard other(copy);
  TEST_ASSERT_FALSE(other.boot(BootKind::Other, kBuild + 1));   // an update: start clean
  TEST_ASSERT_EQUAL_INT((int)DupVerdict::New, (int)other.check(textKey("5", 1, kT0, "hi")));
  for (size_t at = 0; at < sizeof rtc; at += 37) {
    TgGuardState bad = rtc;
    reinterpret_cast<uint8_t*>(&bad)[at] ^= 0x10;
    TgGuard b(bad);
    TEST_ASSERT_FALSE(b.valid(kBuild));
  }
}

// Counter-test: K distinct messages at a steady rate (one every 10 s) all run until
// the hourly ceiling, then skip with one notice per trip, and run again as the hour
// rolls on. The ceiling counts turns, not duplicates.
static void test_distinct_messages_run_until_the_ceiling() {
  Device d;
  const int K = 720;   // two hours at one message per 10 s
  int paidFirstHour = 0;
  for (int i = 0; i < K; ++i) {
    const uint32_t now = (uint32_t)(kT0 + 10 * i);
    d.page({textKey("77", 100 + i, now, "message " + std::to_string(i))}, now);
    if (i == 359) paidFirstHour = d.paid;
  }
  TEST_ASSERT_EQUAL_INT((int)kInboundTurnsPerHour, paidFirstHour);
  TEST_ASSERT_EQUAL_INT(2 * (int)kInboundTurnsPerHour, d.paid);
  TEST_ASSERT_EQUAL_INT(K - d.paid, d.ceilingSkips);
  TEST_ASSERT_EQUAL_INT(2, d.ceilingNotices);   // one per tripped hour
  TEST_ASSERT_EQUAL_UINT32(0, d.guard.dupDrops());
  // Under the ceiling, every distinct message runs.
  Device under;
  for (int i = 0; i < (int)kInboundTurnsPerHour; ++i)
    under.page({textKey("77", 5000 + i, kT0 + 60 * i, "m" + std::to_string(i))}, (uint32_t)(kT0 + 60 * i));
  TEST_ASSERT_EQUAL_INT((int)kInboundTurnsPerHour, under.paid);
  TEST_ASSERT_EQUAL_INT(0, under.ceilingSkips);
}

// The resume time is the oldest counted turn plus an hour; a clock that moved back
// counts the turns as recent (never as expired).
static void test_ceiling_resume_time_and_clock_jumps() {
  TgGuardState rtc{};
  TgGuard g(rtc);
  g.boot(BootKind::PowerOn, kBuild);
  for (uint32_t i = 0; i < kInboundTurnsPerHour; ++i) g.noteTurn((uint32_t)kT0 + i);
  TEST_ASSERT_FALSE(g.ceilingAllows((uint32_t)kT0 + 100, true));
  TEST_ASSERT_EQUAL_UINT32((uint32_t)kT0 + 3600, g.ceilingResumeAt((uint32_t)kT0 + 100, true));
  TEST_ASSERT_TRUE(g.ceilingAllows((uint32_t)kT0 + 3600, true));
  TEST_ASSERT_FALSE(g.ceilingAllows((uint32_t)kT0 - 5000, true));   // clock jumped back
  // The other chats' limit leaves the owner's reserve: they resume once enough of the
  // counted turns aged out (the 13th oldest), the owner once the oldest did.
  const uint32_t others = kInboundTurnsPerHour - kOwnerReserveTurns;
  TEST_ASSERT_EQUAL_UINT32((uint32_t)kT0 + (kInboundTurnsPerHour - others) + 3600,
                           g.ceilingResumeAt((uint32_t)kT0 + 100, false));
  TEST_ASSERT_EQUAL_UINT32(0, g.ceilingResumeAt((uint32_t)kT0 + 4000, false));
}

// A genuine repeat runs: after the window, or after the answer went out. Inside the
// window and before the answer it is a duplicate.
static void test_genuine_repeat_runs() {
  TgGuardState rtc{};
  TgGuard g(rtc);
  g.boot(BootKind::PowerOn, kBuild);
  const DupKey yes = textKey("77", 10, kT0, "yes");
  g.noteTurned(yes, (uint32_t)kT0 + 8);   // answered 8 s later
  TEST_ASSERT_EQUAL_INT((int)DupVerdict::Repeat, (int)g.check(textKey("77", 11, kT0 + 3, "yes")));
  TEST_ASSERT_EQUAL_INT((int)DupVerdict::New, (int)g.check(textKey("77", 12, kT0 + 9, "yes")));
  TEST_ASSERT_EQUAL_INT((int)DupVerdict::New, (int)g.check(textKey("77", 13, kT0 + 121, "yes")));
  TEST_ASSERT_EQUAL_INT((int)DupVerdict::New, (int)g.check(textKey("78", 14, kT0 + 3, "yes")));   // other chat
  TEST_ASSERT_EQUAL_INT((int)DupVerdict::Replay, (int)g.check(textKey("77", 10, kT0, "yes")));
  // Clock unset when it was answered: the window alone decides.
  g.noteTurned(textKey("77", 20, kT0 + 500, "ok"), 0);
  TEST_ASSERT_EQUAL_INT((int)DupVerdict::Repeat, (int)g.check(textKey("77", 21, kT0 + 600, "ok")));
  TEST_ASSERT_EQUAL_INT((int)DupVerdict::New, (int)g.check(textKey("77", 22, kT0 + 621, "ok")));
}

// Commands: a re-served command is a Replay (it must not run twice), a repeated one is
// never a Repeat; neither counts against the ceiling.
static void test_commands() {
  Device d;
  const DupKey c1 = textKey("77", 30, kT0, "/status", true);
  d.page({c1}, kT0);
  TEST_ASSERT_EQUAL_INT((int)DupVerdict::Replay, (int)d.guard.check(c1));
  TEST_ASSERT_EQUAL_INT((int)DupVerdict::New, (int)d.guard.check(textKey("77", 31, kT0 + 1, "/status", true)));
  TEST_ASSERT_EQUAL_INT(0, d.paid);
}

// Within one page: the owner tapped Send twice (two message ids, same text) - one turn,
// one notice; the same update twice in one injected page is a Replay.
static void test_duplicates_inside_one_page() {
  Device d;
  d.page({textKey("77", 40, kT0, "order pizza"), textKey("77", 41, kT0, "order pizza"),
          textKey("77", 41, kT0, "order pizza")},
         kT0 + 1);
  TEST_ASSERT_EQUAL_INT(1, d.paid);
  TEST_ASSERT_EQUAL_INT(1, d.dupNotices);
  TEST_ASSERT_EQUAL_UINT32(2, d.guard.dupDrops());
}

// Media keys: the same photo (file_unique_id) with the same caption is a repeat; a new
// caption or a new photo is new; a missing file_unique_id falls back to the file id.
static void test_media_content_keys() {
  TEST_ASSERT_EQUAL_UINT32(contentKeyMedia('p', "Uabc", "f1", "dinner"), contentKeyMedia('p', "Uabc", "f2", "dinner"));
  TEST_ASSERT_TRUE(contentKeyMedia('p', "Uabc", "f1", "dinner") != contentKeyMedia('p', "Uabc", "f1", "lunch"));
  TEST_ASSERT_TRUE(contentKeyMedia('p', "Uabc", "f1", "") != contentKeyMedia('p', "Uxyz", "f1", ""));
  TEST_ASSERT_EQUAL_UINT32(contentKeyMedia('p', "", "f1", ""), contentKeyMedia('p', "", "f1", ""));
  TEST_ASSERT_TRUE(contentKeyMedia('p', "U1", "f", "") != contentKeyMedia('d', "U1", "f", ""));
  TEST_ASSERT_TRUE(contentKeyMedia('p', "", "abc", "") != contentKeyText("abc"));
}

// The poison guard's record: two crash resets on the same work (id and phase), then it
// is skipped once; a commit, other work, a restart or a power-on never count; marking
// the same work again keeps its tries. (The receive cycle's use of the marks across
// pages, items and buckets is test_tg_burst's.)
static void test_poison_guard_acks_after_two_crash_resets() {
  TgGuardState rtc{};
  TgGuard g(rtc);
  g.boot(BootKind::PowerOn, kBuild);
  TEST_ASSERT_FALSE(g.poisoned(700, kPhasePage));
  g.mark(700, kPhasePage);
  g.boot(BootKind::Crash, kBuild);
  TEST_ASSERT_EQUAL_UINT8(1, g.tries());
  TEST_ASSERT_FALSE(g.poisoned(700, kPhasePage));
  g.mark(700, kPhasePage);   // the same work: tries kept
  TEST_ASSERT_EQUAL_UINT8(1, g.tries());
  g.boot(BootKind::Crash, kBuild);
  TEST_ASSERT_EQUAL_UINT8(2, g.tries());
  TEST_ASSERT_FALSE(g.poisoned(700, kPhaseBucket));   // another phase is other work
  TEST_ASSERT_TRUE(g.poisoned(700, kPhasePage));      // skipped
  TEST_ASSERT_EQUAL_UINT32(1, g.poisonAcks());
  TEST_ASSERT_FALSE(g.poisoned(700, kPhasePage));     // the mark was cleared with the skip
  TEST_ASSERT_EQUAL_INT32(0, g.markedId());
  // A restart (software, update, USB) counts nothing.
  TgGuardState r2{};
  TgGuard h(r2);
  h.boot(BootKind::PowerOn, kBuild);
  h.mark(800, kPhaseItem);
  for (int i = 0; i < 5; ++i) h.boot(BootKind::Other, kBuild);
  TEST_ASSERT_EQUAL_UINT8(0, h.tries());
  // A commit clears the mark, so a later crash counts nothing.
  h.commitPage();
  h.boot(BootKind::Crash, kBuild);
  TEST_ASSERT_EQUAL_UINT8(0, h.tries());
  // Other work resets the count.
  h.mark(900, kPhaseBucket);
  h.boot(BootKind::Crash, kBuild);
  h.mark(901, kPhaseBucket);
  TEST_ASSERT_EQUAL_UINT8(0, h.tries());
  // A power-on clears everything; ids <= 0 are never marked.
  h.boot(BootKind::Crash, kBuild);
  h.boot(BootKind::PowerOn, kBuild);
  TEST_ASSERT_EQUAL_UINT8(0, h.tries());
  h.mark(0, kPhasePage);
  TEST_ASSERT_EQUAL_INT32(0, h.markedId());
}

// Every reset reason maps to one boot kind: panic, the three watchdogs, a CPU lockup
// and a brownout count as a crash; power-on, unknown, a power glitch and out-of-range
// values clear; everything else keeps the record and counts nothing.
static void test_boot_kind_of_every_reset_reason() {
  using nimbus::sys::ResetReason;
  for (int r = -1; r <= nimbus::sys::kLastResetReason + 2; ++r) {
    const BootKind k = bootKindOf(r);
    const bool crash = r == (int)ResetReason::Panic || r == (int)ResetReason::IntWdt ||
                       r == (int)ResetReason::TaskWdt || r == (int)ResetReason::Wdt ||
                       r == (int)ResetReason::CpuLockup || r == (int)ResetReason::Brownout;
    const bool power = r <= 1 || r == (int)ResetReason::PwrGlitch || r > nimbus::sys::kLastResetReason;
    if (crash) TEST_ASSERT_EQUAL_INT_MESSAGE((int)BootKind::Crash, (int)k, std::to_string(r).c_str());
    else if (power) TEST_ASSERT_EQUAL_INT_MESSAGE((int)BootKind::PowerOn, (int)k, std::to_string(r).c_str());
    else TEST_ASSERT_EQUAL_INT_MESSAGE((int)BootKind::Other, (int)k, std::to_string(r).c_str());
  }
}

// Property: random streams of new messages, re-served ones and restarts. Each message
// costs at most one paid turn (two only across a power-on), and every drop is counted.
static void test_random_streams_never_pay_twice() {
  std::mt19937 rng(3);
  for (int trial = 0; trial < 200; ++trial) {
    Device d;
    std::vector<DupKey> sent;
    int powerOns = 0;
    uint32_t now = kT0;
    for (int step = 0; step < 300; ++step) {
      now += 5 + rng() % 30;
      const int r = (int)(rng() % 10);
      if (r < 3 || sent.empty()) {
        sent.push_back(textKey("77", 1000 + (int64_t)sent.size(), now, "msg " + std::to_string(sent.size())));
        d.page({sent.back()}, now);
      } else if (r < 9) {
        d.page({sent[rng() % sent.size()]}, now);   // a replay of something answered
      } else {
        const BootKind k = (BootKind)(rng() % 3);
        if (k == BootKind::PowerOn) ++powerOns;
        d.restart(k);
      }
    }
    TEST_ASSERT_TRUE(d.paid >= (int)sent.size() - d.ceilingSkips);
    TEST_ASSERT_TRUE(d.paid <= (int)sent.size() * (1 + powerOns));
  }
}

// The owner's reserve: a busy shared chat (a family member, a group) stops at the
// ceiling minus the reserve, so the owner still gets kOwnerReserveTurns replies in the
// same hour, and the device as a whole never passes the ceiling. Each trip gets its own
// notice.
static void test_owner_keeps_a_reserve() {
  Device d;
  const uint32_t others = kInboundTurnsPerHour - kOwnerReserveTurns;
  for (int i = 0; i < 100; ++i)
    d.page({textKey("300", 2000 + i, kT0 + i, "member " + std::to_string(i))}, (uint32_t)(kT0 + i), false);
  TEST_ASSERT_EQUAL_INT((int)others, d.paid);
  TEST_ASSERT_EQUAL_INT(1, d.ceilingNotices);
  for (int i = 0; i < 30; ++i)
    d.page({textKey("77", 3000 + i, kT0 + 200 + i, "owner " + std::to_string(i))}, (uint32_t)(kT0 + 200 + i));
  TEST_ASSERT_EQUAL_INT((int)kInboundTurnsPerHour, d.paid);   // the owner used the reserve, no more
  TEST_ASSERT_EQUAL_INT(2, d.ceilingNotices);                  // the owner's own trip
  // Counter-test: an owner-only flood still stops at the whole ceiling.
  Device o;
  for (int i = 0; i < 100; ++i) o.page({textKey("77", 4000 + i, kT0 + i, "o" + std::to_string(i))}, (uint32_t)(kT0 + i));
  TEST_ASSERT_EQUAL_INT((int)kInboundTurnsPerHour, o.paid);
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_k_identical_pages_cost_one_turn);
  RUN_TEST(test_restart_mid_episode_keeps_one_turn);
  RUN_TEST(test_fresh_device_runs_the_first_message);
  RUN_TEST(test_record_rejects_other_builds_and_corruption);
  RUN_TEST(test_distinct_messages_run_until_the_ceiling);
  RUN_TEST(test_ceiling_resume_time_and_clock_jumps);
  RUN_TEST(test_genuine_repeat_runs);
  RUN_TEST(test_commands);
  RUN_TEST(test_duplicates_inside_one_page);
  RUN_TEST(test_media_content_keys);
  RUN_TEST(test_poison_guard_acks_after_two_crash_resets);
  RUN_TEST(test_boot_kind_of_every_reset_reason);
  RUN_TEST(test_random_streams_never_pay_twice);
  RUN_TEST(test_owner_keeps_a_reserve);
  return UNITY_END();
}
