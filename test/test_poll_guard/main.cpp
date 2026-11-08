#include <unity.h>

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "nimbus/crash_summary.h"
#include "nimbus/net/tg_dup_guard.h"
#include "nimbus/orch/restart_notice.h"
#include "nimbus/orch/turn_journal.h"
#include "nimbus/sys/poll_guard.h"
#include "nimbus/sys/reset_reason.h"

// The poll task's liveness guard (nimbus/sys/poll_guard.h), driven the way the device
// drives it: the poll task beats at its progress points, the loop task checks once a
// second with the task's run state. Lumi, 4 to 7 Nov 2026: the poll task stopped inside
// a turn three times and nothing noticed (28 h the first time). Every stall class the
// poll task has is a row of kClasses below; each must be noticed within the limit,
// reported once, restarted once and told to the owner once, and none of the long but
// healthy work in kHealthy may ever trip it.

using namespace nimbus::sys;

void setUp() {}
void tearDown() {}

namespace {

constexpr uint32_t kBuild = 0x7E57B11Du;
constexpr uint32_t kTickMs = 1000;   // the loop task's check cadence

// One way the poll task stops: the last step that beat, and what the loop task sees of
// the task while it is stuck (blocked on something, or on the CPU).
struct StallClass {
  const char* name;
  const char* step;
  RunState state;
  bool gone;
};

// file:line of each class is in coverage/TGSTALL-2026-11.md.
const StallClass kClasses[] = {
    // telegram.cpp pollTask: the staged update check or install runs inline on the task.
    {"staged update work that never returns", "staged work", RunState::Blocked, false},
    // A file store write refused and retried, or a store scan that never ends, inside a tool.
    {"file store refusal retried forever", "tool", RunState::Running, false},
    // memory::Lock, TenantLock, the session mutex, the lwIP core lock: portMAX_DELAY takes.
    {"a lock that is never given back", "tool", RunState::Blocked, false},
    // A DNS answer waited on with no bound (v4.9.0: NetworkManager::hostByName).
    {"a name lookup with no deadline", "connect", RunState::Blocked, false},
    // A socket read whose loop never checks a deadline, or a write that never drains.
    {"a TLS read with no deadline", "provider read", RunState::Blocked, false},
    // A queue send that waits for the task's own drain.
    {"a queue that never drains", "replies", RunState::Blocked, false},
    // The Lumi shape: on the CPU, never yielding (an engine busy-wait, a parse loop).
    {"a CPU spin that never yields", "provider read", RunState::Running, false},
    {"a spin preempted by the radio", "turn", RunState::Ready, false},
    // The task ended while the guard was armed (it can only end on a quiesce).
    {"the poll task is gone", "poll loop", RunState::Gone, true},
};

struct Sim {
  PollLiveness live;
  uint32_t now = 1000;
  int fired = 0;
  PollStall last;
  uint32_t firedAtSilentMs = 0;

  void beatFor(uint32_t ms, uint32_t everyMs, const char* step) {
    for (uint32_t t = 0; t < ms; t += kTickMs) {
      if (t % everyMs == 0) live.beat(now, step);
      checkOnce(RunState::Blocked, kPollSilentLimitMs);
      now += kTickMs;
    }
  }
  void stuckFor(uint32_t ms, RunState st, uint32_t limitMs = kPollSilentLimitMs) {
    for (uint32_t t = 0; t < ms; t += kTickMs) {
      checkOnce(st, limitMs);
      now += kTickMs;
    }
  }
  void checkOnce(RunState st, uint32_t limitMs) {
    PollStall s;
    if (live.check(now, st, limitMs, s)) {
      ++fired;
      last = s;
      firedAtSilentMs = s.silentMs;
    }
  }
};

bool printableAscii(const std::string& s) {
  for (unsigned char c : s)
    if (c < 0x20 || c > 0x7e) return false;
  return true;
}

}  // namespace

// Every class: noticed once, between the limit and one check past it, with the step
// and the on-CPU judgement right; never again while the same silence lasts (no thrash).
void test_every_stall_class_is_noticed_once_within_the_limit() {
  for (const StallClass& c : kClasses) {
    Sim sim;
    sim.live.arm(sim.now);
    sim.beatFor(120000, 1000, "poll loop");   // two healthy minutes
    sim.live.beat(sim.now, c.step);
    sim.stuckFor(kPollSilentLimitMs + 30u * 60u * 1000u, c.state);   // stuck 35 min
    TEST_ASSERT_EQUAL_INT_MESSAGE(1, sim.fired, c.name);
    TEST_ASSERT_TRUE_MESSAGE(sim.firedAtSilentMs >= kPollSilentLimitMs, c.name);
    TEST_ASSERT_TRUE_MESSAGE(sim.firedAtSilentMs < kPollSilentLimitMs + 2 * kTickMs, c.name);
    TEST_ASSERT_EQUAL_STRING_MESSAGE(c.step, sim.last.step, c.name);
    TEST_ASSERT_EQUAL_MESSAGE(c.gone, sim.last.gone, c.name);
    const bool onCpu = c.state == RunState::Running || c.state == RunState::Ready;
    if (!c.gone) TEST_ASSERT_EQUAL_MESSAGE(onCpu, sim.last.spinning(), c.name);
    TEST_ASSERT_TRUE_MESSAGE(sim.last.samples > 0 || c.gone, c.name);
  }
}

// The long but healthy work the poll task does: none of it trips the guard.
void test_healthy_long_work_never_trips() {
  struct Healthy {
    const char* name;
    uint32_t durationMs;
    uint32_t beatEveryMs;
  };
  const Healthy kHealthy[] = {
      {"long-polls all day (a beat per socket wait)", 8u * 3600u * 1000u, 1000},
      {"a 12-minute turn, provider reads beat every second", 12u * 60u * 1000u, 1000},
      {"tool calls that do not beat, 120 s each (the provider cap)", 30u * 60u * 1000u, 120000},
      {"the longest silent window the guard tolerates", 60u * 60u * 1000u, kPollSilentLimitMs - kTickMs},
      {"a staged update install, a beat per downloaded chunk", 6u * 60u * 1000u, 2000},
  };
  for (const Healthy& h : kHealthy) {
    Sim sim;
    sim.live.arm(sim.now);
    sim.beatFor(h.durationMs, h.beatEveryMs, "provider read");
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, sim.fired, h.name);
  }
}

// A beat ends the episode: a later silence is a new episode and is noticed again.
void test_a_beat_starts_a_new_episode() {
  Sim sim;
  sim.live.arm(sim.now);
  sim.live.beat(sim.now, "turn");
  sim.stuckFor(kPollSilentLimitMs + 5000, RunState::Running);
  TEST_ASSERT_EQUAL_INT(1, sim.fired);
  sim.beatFor(60000, 1000, "poll loop");
  sim.live.beat(sim.now, "tool");
  sim.stuckFor(kPollSilentLimitMs + 5000, RunState::Blocked);
  TEST_ASSERT_EQUAL_INT(2, sim.fired);
  TEST_ASSERT_EQUAL_STRING("tool", sim.last.step);
  TEST_ASSERT_FALSE(sim.last.spinning());
}

// A quiesce (the factory reset, a mode switch) stops the task on purpose: the guard is
// disarmed and stays quiet however long the task is gone; re-armed, it starts fresh.
void test_disarmed_guard_stays_quiet() {
  Sim sim;
  sim.live.arm(sim.now);
  sim.live.beat(sim.now, "poll loop");
  sim.live.disarm();
  sim.stuckFor(3u * kPollSilentLimitMs, RunState::Gone);
  TEST_ASSERT_EQUAL_INT(0, sim.fired);
  sim.live.arm(sim.now);
  sim.stuckFor(kPollSilentLimitMs - 2 * kTickMs, RunState::Blocked);
  TEST_ASSERT_EQUAL_INT(0, sim.fired);   // the silence counts from arm(), not the old beat
  sim.stuckFor(4 * kTickMs, RunState::Blocked);
  TEST_ASSERT_EQUAL_INT(1, sim.fired);
}

// millis() wraps after 49.7 days: a silence across the wrap is measured right.
void test_silence_across_the_millis_wrap() {
  Sim sim;
  sim.now = 0xFFFFFFFFu - 60000u;
  sim.live.arm(sim.now);
  sim.live.beat(sim.now, "provider read");
  sim.stuckFor(kPollSilentLimitMs - 2 * kTickMs, RunState::Running);
  TEST_ASSERT_EQUAL_INT(0, sim.fired);
  sim.stuckFor(4 * kTickMs, RunState::Running);
  TEST_ASSERT_EQUAL_INT(1, sim.fired);
  TEST_ASSERT_TRUE(sim.firedAtSilentMs < kPollSilentLimitMs + 2 * kTickMs);
}

// The streak record: one guard restart, the boot after it knows, the owner is told on
// the first restart of a streak only; past kPollStallRestartsMax the guard waits an hour;
// a healthy half hour ends the streak; garbage, another build or a power-on clear it.
void test_stall_record_streak_and_boot() {
  PollStallStore store;
  std::memset(&store, 0xA5, sizeof store);   // what a power-on leaves
  PollStallRecord rec(store);
  TEST_ASSERT_FALSE(rec.boot(kBuild));
  TEST_ASSERT_EQUAL_UINT8(0, rec.streak());
  TEST_ASSERT_EQUAL_UINT32(kPollSilentLimitMs, rec.limitMs());

  PollStall s;
  s.silentMs = kPollSilentLimitMs;
  s.step = "provider read";
  for (uint8_t n = 1; n <= kPollStallRestartsMax + 1; ++n) {
    rec.noteStall(s);
    TEST_ASSERT_EQUAL_UINT8(n, rec.streak());
    PollStallRecord next(store);   // the restart
    TEST_ASSERT_TRUE(next.boot(kBuild));
    TEST_ASSERT_EQUAL_UINT8(n, next.streak());
    TEST_ASSERT_FALSE(PollStallRecord(store).boot(kBuild));   // the pending flag is read once
    TEST_ASSERT_EQUAL_UINT32(n >= kPollStallRestartsMax ? kPollSilentLongMs : kPollSilentLimitMs,
                             next.limitMs());
  }
  rec.noteHealthy();
  TEST_ASSERT_EQUAL_UINT8(0, rec.streak());
  TEST_ASSERT_EQUAL_UINT32(kPollSilentLimitMs, rec.limitMs());

  rec.noteStall(s);
  TEST_ASSERT_TRUE(rec.intact(kBuild));
  TEST_ASSERT_FALSE(PollStallRecord(store).boot(kBuild + 1));   // another build's layout
  TEST_ASSERT_EQUAL_UINT8(0, PollStallRecord(store).streak());
  rec.noteStall(s);
  reinterpret_cast<uint8_t*>(&store)[9] ^= 0x40;   // a flipped bit
  PollStallRecord torn(store);
  TEST_ASSERT_FALSE(torn.boot(kBuild));
  TEST_ASSERT_EQUAL_UINT8(0, torn.streak());
}

// The panic reason: the tag, the facts, printable, bounded, and read back as the guard's
// by the crash notice (a watchdog: "part of it stopped responding").
void test_reason_is_bounded_and_names_the_stall() {
  PollStall s;
  s.silentMs = 301234;
  s.step = "provider read";
  s.samples = 290;
  s.onCpu = 289;
  const uint32_t bt[kPollBacktraceMax] = {0x4200a1b2, 0x42011c3d, 0x4201ffff, 0x40377853, 0x420aaaaa, 0x420bbbbb};
  const std::string r = pollStallReason(s, 0x4200a1b2, bt, kPollBacktraceMax);
  TEST_ASSERT_TRUE(r.size() <= kPollStallReasonMax);
  TEST_ASSERT_TRUE(printableAscii(r));
  TEST_ASSERT_TRUE(isPollStallReason(r));
  TEST_ASSERT_TRUE(r.find("silent 301 s") != std::string::npos);
  TEST_ASSERT_TRUE(r.find("provider read") != std::string::npos);
  TEST_ASSERT_TRUE(r.find("spin") != std::string::npos);
  TEST_ASSERT_TRUE(r.find("0x4200a1b2") != std::string::npos);
  TEST_ASSERT_FALSE(isPollStallReason("abort() was called at PC 0x40377853 on core 0"));

  nimbus::crash::Report dump;
  dump.state = nimbus::crash::ImageState::Valid;
  dump.why = r;
  TEST_ASSERT_EQUAL(nimbus::crash::CrashKind::Unresponsive,
                    nimbus::crash::crashKind(&dump, (int)ResetReason::Panic));

  // A step longer than any real one, a huge backtrace: still bounded and printable.
  PollStall w;
  w.silentMs = 4000000000u;
  w.step = "a step name far longer than any step the device has, with \x01 a control byte in it";
  const std::string r2 = pollStallReason(w, 0xFFFFFFFFu, bt, kPollBacktraceMax);
  TEST_ASSERT_TRUE(r2.size() <= kPollStallReasonMax);
  TEST_ASSERT_TRUE(printableAscii(r2));
  TEST_ASSERT_TRUE(isPollStallReason(r2));
  // No PC (the task was gone): no address at all.
  PollStall g;
  g.gone = true;
  g.silentMs = kPollSilentLimitMs;
  g.step = "poll loop";
  const std::string r3 = pollStallReason(g, 0, nullptr, 0);
  TEST_ASSERT_TRUE(r3.find("0x") == std::string::npos);
  TEST_ASSERT_TRUE(r3.find("gone") != std::string::npos);

  const std::string line = pollStallLogLine(s, 1, kPollSilentLimitMs, true);
  TEST_ASSERT_TRUE(printableAscii(line));
  TEST_ASSERT_TRUE(line.find("restart") != std::string::npos);
  TEST_ASSERT_TRUE(line.find("provider read") != std::string::npos);
}

// The guard restarts by a panic, so the poison guard counts a try against the Telegram
// work in flight: a message that stalls the device twice is skipped (tg_dup_guard.h).
void test_guard_restart_counts_a_poison_try() {
  TEST_ASSERT_EQUAL(nimbus::net::BootKind::Crash, nimbus::net::bootKindOf((int)ResetReason::Panic));
  TEST_ASSERT_TRUE(isCrashReset((int)ResetReason::Panic));
}

// The owner notice: plain words, once per streak, at once for an owner request, held for
// a routine, never an em dash, never "I".
void test_owner_notice_plain_and_once_per_streak() {
  nimbus::orch::TurnJournal msg;
  msg.kind = "message";
  nimbus::orch::TurnJournal photo;
  photo.kind = "photo";
  nimbus::orch::TurnJournal routine;
  routine.kind = "routine";

  const nimbus::orch::RestartNoticePlan pm = nimbus::orch::planStallNotice("Lumi", &msg, "20:26", 1);
  TEST_ASSERT_EQUAL(nimbus::orch::NoticeTiming::Now, pm.timing);
  TEST_ASSERT_TRUE(pm.text.find("Lumi") == 0);
  TEST_ASSERT_TRUE(pm.text.find("stopped answering") != std::string::npos);
  TEST_ASSERT_TRUE(pm.text.find("restarted itself") != std::string::npos);
  TEST_ASSERT_TRUE(pm.text.find("message from 20:26") != std::string::npos);
  TEST_ASSERT_TRUE(pm.text.find("saved twice") != std::string::npos);

  const nimbus::orch::RestartNoticePlan pp = nimbus::orch::planStallNotice("Lumi", &photo, "", 1);
  TEST_ASSERT_EQUAL(nimbus::orch::NoticeTiming::Now, pp.timing);
  TEST_ASSERT_TRUE(pp.text.find("photo") != std::string::npos);
  TEST_ASSERT_TRUE(pp.text.find("send it again") != std::string::npos);

  const nimbus::orch::RestartNoticePlan pr = nimbus::orch::planStallNotice("Lumi", &routine, "03:30", 1);
  TEST_ASSERT_EQUAL(nimbus::orch::NoticeTiming::Held, pr.timing);
  TEST_ASSERT_TRUE(pr.text.find("routine") != std::string::npos);

  const nimbus::orch::RestartNoticePlan pn = nimbus::orch::planStallNotice("", nullptr, "", 1);
  TEST_ASSERT_EQUAL(nimbus::orch::NoticeTiming::Now, pn.timing);
  TEST_ASSERT_TRUE(pn.text.find("Nimbus") == 0);

  for (uint8_t streak = 2; streak < 8; ++streak) {
    const nimbus::orch::RestartNoticePlan again = nimbus::orch::planStallNotice("Lumi", &msg, "20:26", streak);
    TEST_ASSERT_EQUAL(nimbus::orch::NoticeTiming::None, again.timing);
    TEST_ASSERT_TRUE(again.text.empty());
  }
  for (const auto* p : {&pm, &pp, &pr, &pn}) {
    TEST_ASSERT_TRUE(p->text.find("\xE2\x80\x94") == std::string::npos);   // no em dash
    TEST_ASSERT_TRUE(p->text.find(" I ") == std::string::npos);
    TEST_ASSERT_TRUE(p->text.find("crash") == std::string::npos);   // it did not crash
  }
  // The queue lets it out once, like every restart notice.
  nimbus::orch::RestartNoticeQueue q;
  TEST_ASSERT_FALSE(q.atBoot(pm).empty());
  TEST_ASSERT_TRUE(q.atBoot(pm).empty());
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_every_stall_class_is_noticed_once_within_the_limit);
  RUN_TEST(test_healthy_long_work_never_trips);
  RUN_TEST(test_a_beat_starts_a_new_episode);
  RUN_TEST(test_disarmed_guard_stays_quiet);
  RUN_TEST(test_silence_across_the_millis_wrap);
  RUN_TEST(test_stall_record_streak_and_boot);
  RUN_TEST(test_reason_is_bounded_and_names_the_stall);
  RUN_TEST(test_guard_restart_counts_a_poison_try);
  RUN_TEST(test_owner_notice_plain_and_once_per_streak);
  return UNITY_END();
}
