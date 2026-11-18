// test_fold_driver - the one fold driver (lane F of the history fix, 16 Nov 2026; owner
// ruling 2: the summary can never stay off for days again).
//
// The class, not the instance: Pending at every rung index for manual and automatic
// cycles, exactly one CycleFailed a cycle, at most one rung a pass, a manual request that
// is never lost (mid-cycle, across a reboot, for another chat, after its slot is evicted),
// a manual request that restarts an automatic cycle with keep 0, the device-wide daily
// cap across chats, a simulated week of failures on five chats (at most 12 paid requests
// and one alert a local day, an alert once a day of failing has passed), the firmware
// reset, the Lumi blob due on the first pass and folding forward with no id gap, a fresh
// device and a device with no clock.
#include <unity.h>

#include <cstdio>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "../support/fake_config.h"
#include "../support/fake_platform.h"
#include "nimbus/harness/engine.h"
#include "nimbus/harness/fold_driver.h"
#include "nimbus/harness/providers.h"
#include "nimbus/orch/compact.h"

using agent::FoldDriver;
using agent::FoldDriverHooks;
using agent::FoldSlice;
using agent::FoldTick;
using agent::TurnEngine;
using harness_test::FakeConfig;
using harness_test::FakePlatform;
using harness_test::LogCapture;
namespace orch = nimbus::orch;

void setUp() { LogCapture::install(); }
void tearDown() { agent::hlog::setSink(nullptr); }

namespace {

const uint32_t kT0 = 1794787200;   // 2026-11-16 00:00 UTC

std::string goodSummary(const std::string& tag) {
  std::string s = "1. Decisions & preferences\n- Daily targets 1,900 kcal; " + tag + "\n";
  s += "2. Pending items & next steps\n- Weigh-in on Monday.\n3. Facts worth remembering long-term\n- 10 km in March.\n";
  return s;
}

struct BlobIO : orch::FoldStoreIO {
  std::string blob;
  std::string load() override { return blob; }
  void save(const std::string& b) override { blob = b; }
};

// One device (or VN): an engine over scripted hosts, its fold store and control file, and
// a driver over them. Rows are kept per chat; the slice is the protocol's: oldest first
// from the chat's thru, about 1 KB a row, keep 0 for a manual cycle, else keep the newest
// two rows.
struct Device {
  FakePlatform plat;
  FakeConfig cfg;
  BlobIO blobIO;
  std::string ctlText;
  orch::FoldStore folds;
  std::unique_ptr<TurnEngine> eng;
  std::unique_ptr<FoldDriver> drv;
  bool failing = true;                 // every rung answers thin (a 2xx: paid)
  std::string thinText = "Noted.";     // what a failing rung answers
  std::set<std::string> answering;     // "host/model" that answer with a summary
  int asked = 0;
  std::vector<std::string> askedKeys;                      // "host/model" per request
  std::vector<std::pair<std::string, std::string>> told;   // (chat, text)
  std::vector<agent::FoldDone> done;
  std::vector<std::string> changedChats;                  // every status change, by chat
  std::map<std::string, std::vector<std::string>> rows;    // chat -> row ids, oldest first
  std::vector<std::pair<std::string, bool>> sliced;        // (chat, manual) per slice read
  bool quiet = true;
  bool online = true;
  std::set<std::string> guests;   // chats that are not the owner's (empty: every chat is)
  bool ownerAlerts = false;       // the binding's alert: the owner's chats only, says if it told
  std::string firmware = "5.1.2";

  Device() {
    TurnEngine::Deps d;
    d.cfg = cfg.contract();
    d.platform = plat.contract();
    for (const char* h : {"openai", "anthropic", "mistral"}) d.hosts.add(h, hostFn(h));
    eng.reset(new TurnEngine(std::move(d)));
  }

  agent::ProviderTurnFn hostFn(const std::string& host) {
    return [this, host](std::string&, const std::string&, const std::string&, std::string& out, std::string&,
                        const agent::HeadTools*, orch::TokenUsage* usage) -> bool {
      asked++;
      if (usage) usage->responses = 1;
      const std::string model = agent::providers::modelOverrideFor(host.c_str());
      const std::string key = host + "/" + (model.empty() ? "model-" + host : model);
      askedKeys.push_back(key);
      out = !failing || answering.count(key) ? goodSummary(key) : thinText;
      return true;
    };
  }

  void boot() {
    folds.begin(&blobIO);
    FoldDriverHooks h;
    h.online = [this] { return online; };
    h.quiet = [this](bool) { return quiet; };
    h.triggerBytes = [] { return 48u * 1024u; };
    h.foldCycleBytes = [] { return (size_t)53900; };
    h.slice = [this](const std::string& chat, size_t cap, bool manual) { return slice(chat, cap, manual); };
    h.tell = [this](const std::string& chat, const std::string& text) { told.push_back({chat, text}); };
    h.folded = [this](const agent::FoldDone& d) { done.push_back(d); };
    h.changed = [this](const std::string& chat) { changedChats.push_back(chat); };
    if (!guests.empty()) h.ownerChat = [this](const std::string& chat) { return !guests.count(chat); };
    if (ownerAlerts)
      h.alert = [this](const std::string& chat, const std::string& text) {
        if (guests.count(chat)) return false;   // only logged
        told.push_back({chat, text});
        return true;
      };
    h.firmware = firmware;
    orch::FoldCtlIO io;
    io.load = [this] { return ctlText; };
    io.save = [this](const std::string& t) {
      ctlText = t;
      return true;
    };
    drv.reset(new FoldDriver(*eng, folds, io, h));
    drv->begin();
  }
  void reboot() {
    drv.reset();
    boot();
  }

  uint32_t nothingKept = 0;
  bool unreadAlways = false;   // every slice meets rows the store will not give back
  std::vector<agent::SliceState> stateScript;   // when set: each slice's outcome, in turn
  size_t stateAt = 0;
  std::set<std::string> waitChats;   // chats whose every slice read goes on (Wait)   // > 0: every slice reads to its bound and finds nothing, this much kept
  FoldSlice slice(const std::string& chat, size_t cap, bool manual) {
    sliced.push_back({chat, manual});
    FoldSlice s;
    if (waitChats.count(chat)) {   // this chat's read goes on (a cold walk)
      s.state = agent::SliceState::Wait;
      s.progressed = true;
      return s;
    }
    if (!stateScript.empty()) {   // a scripted outcome per slice, in turn
      s.state = stateScript[stateAt++ % stateScript.size()];
      return s;
    }
    if (unreadAlways) {
      s.state = agent::SliceState::Unread;
      return s;
    }
    if (nothingKept) {
      s.state = agent::SliceState::Nothing;
      s.keptBytes = nothingKept;
      return s;
    }
    const std::string thru = folds.get(chat).thru;
    const std::vector<std::string>& r = rows[chat];
    const size_t keep = manual ? 0 : 2;
    for (size_t i = 0; i + keep < r.size(); i++) {
      if (!thru.empty() && !orch::rowIdAfter(r[i], thru)) continue;
      if (s.digest.size() + 1024 > cap) break;
      s.digest += "row " + r[i] + std::string(1016, '.') + "\n";
      s.thru = r[i];
      s.rows++;
      s.turns++;
    }
    s.firstDate = s.rows ? "2026-11-02" : "";
    s.lastDate = s.rows ? "2026-11-15" : "";
    return s;
  }
  // `n` rows on the chat, a hidden span behind its soft start (a fold is due).
  void backlog(const std::string& chat, int n, int firstId = 0x100) {
    for (int i = 0; i < n; i++) {
      char id[16];
      std::snprintf(id, sizeof(id), "m%08x", firstId + i);
      rows[chat].push_back(id);
    }
    folds.setSoftStart(chat, rows[chat][rows[chat].size() - 3]);
    folds.noteTurn(chat);
  }
  FoldTick tick(uint32_t epoch) {
    plat.ms += 1000;
    return drv->tick(plat.ms, epoch);
  }
  int alerts() const {
    int n = 0;
    for (const auto& t : told) n += t.second.find("hasn't updated since") != std::string::npos;
    return n;
  }
};

}  // namespace

// Automatic and manual: a cycle with every rung failing returns Pending after each of the
// first n-1 rungs and CycleFailed exactly once after the n-th, one rung a pass, and only
// the CycleFailed counts a failure (none on Pending). An automatic cycle tells the owner
// nothing; a manual one gets one failure notice with the next try time.
static void test_pending_at_every_rung_then_one_cycle_failed() {
  for (bool manual : {false, true}) {
    Device d;
    d.boot();
    d.backlog("c", 40);
    if (manual) d.drv->requestManual("c");
    const int n = (int)d.eng->foldRungs().size();
    TEST_ASSERT_EQUAL(4, n);
    int cycleFailed = 0;
    for (int i = 0; i < n; i++) {
      const int before = d.asked;
      const FoldTick t = d.tick(kT0 + (uint32_t)i);
      TEST_ASSERT_EQUAL(1, d.asked - before);   // one rung a pass
      if (i < n - 1) {
        TEST_ASSERT_TRUE_MESSAGE(t == FoldTick::Pending, agent::foldTickName(t));
        TEST_ASSERT_EQUAL_UINT32(0, d.folds.get("c").failedCycles);   // no strike on Pending
        TEST_ASSERT_EQUAL(0, (int)d.told.size());
      } else {
        cycleFailed += t == FoldTick::CycleFailed;
      }
    }
    TEST_ASSERT_EQUAL(1, cycleFailed);
    TEST_ASSERT_EQUAL_UINT32(1, d.folds.get("c").failedCycles);
    TEST_ASSERT_EQUAL_UINT32(kT0 + (uint32_t)(n - 1) + 15 * 60, d.folds.get("c").nextTry);
    TEST_ASSERT_EQUAL(manual ? 1 : 0, (int)d.told.size());
    if (manual) TEST_ASSERT_TRUE(d.told[0].second.find("tries again by itself at 00:15") != std::string::npos);
    TEST_ASSERT_TRUE(LogCapture::contains("fold(c): cycle failed, next try 2026-11-16 00:15"));
    // The wait holds: nothing is asked until nextTry.
    const int before = d.asked;
    for (int i = 0; i < 30; i++) d.tick(kT0 + 10 + (uint32_t)i * 20);
    TEST_ASSERT_EQUAL(before, d.asked);
    TEST_ASSERT_TRUE(d.tick(kT0 + 15 * 60 + 10) == FoldTick::Pending);   // the next cycle starts
  }
}

// A rung that answers ends the cycle: Ok after k Pendings, the store written first, one
// confirmation for a manual request that names what it covered.
static void test_answer_at_rung_k_lands_and_confirms_once() {
  for (bool manual : {false, true}) {
    for (int k = 0; k < 4; k++) {
      Device d;
      d.boot();
      d.backlog("c", 40);
      const std::vector<agent::FoldRung> rungs = d.eng->foldRungs();
      d.answering.insert(rungs[(size_t)k].host + "/" + rungs[(size_t)k].model);
      if (manual) d.drv->requestManual("c");
      for (int i = 0; i < k; i++) TEST_ASSERT_TRUE(d.tick(kT0) == FoldTick::Pending);
      TEST_ASSERT_TRUE(d.tick(kT0) == FoldTick::Ok);
      TEST_ASSERT_EQUAL(1, (int)d.done.size());
      TEST_ASSERT_EQUAL_STRING(d.folds.get("c").thru.c_str(), d.done[0].slice.thru.c_str());
      TEST_ASSERT_TRUE(d.folds.get("c").summary.find(rungs[(size_t)k].host) != std::string::npos);
      TEST_ASSERT_EQUAL(manual ? 1 : 0, (int)d.told.size());
      if (manual)
        TEST_ASSERT_EQUAL_STRING(
            "\xE2\x9C\x93 Compacted. Summary updated: covered 40 messages from 2026-11-02 to 2026-11-15.",
            d.told[0].second.c_str());
      TEST_ASSERT_TRUE(d.ctlText.find("manualPendingChat=\n") != std::string::npos);
      // The last status change names the chat, after its request was dropped (bench B08: the
      // CTX echo showed queued=1 after the landing).
      TEST_ASSERT_EQUAL_STRING("c", d.changedChats.back().c_str());
      TEST_ASSERT_FALSE(d.drv->status("c", d.plat.ms, kT0).queued);
    }
  }
}

// A manual request is never lost: queued behind a running cycle for another chat, kept
// across a reboot, kept after its chat's slot is evicted from the fold blob, and every
// one is answered. Manual cycles run one at a time, two minutes apart.
static void test_manual_request_is_never_lost() {
  Device d;
  d.boot();
  d.backlog("a", 40);
  d.backlog("b", 40, 0x400);
  d.failing = false;
  TEST_ASSERT_TRUE(d.tick(kT0) == FoldTick::Ok);   // an automatic fold of "a"
  d.failing = true;
  d.backlog("a", 40, 0x800);
  TEST_ASSERT_TRUE(d.tick(kT0 + 1) == FoldTick::Pending);   // an automatic cycle runs on "a" (or "b")
  const std::string running = d.drv->status("a", d.plat.ms, kT0).running ? "a" : "b";
  const std::string other = running == "a" ? "b" : "a";
  TEST_ASSERT_TRUE(d.drv->requestManual(other) == FoldDriver::Manual::Queued);
  d.folds.noteMessage("web", 10);   // the web chat has a fold slot
  TEST_ASSERT_TRUE(d.drv->requestManual("web") == FoldDriver::Manual::Queued);
  for (int i = 0; i < 30; i++) d.rows["web"].push_back("m0000" + std::to_string(5000 + i));
  // A reboot mid-cycle: the queue is in the control file, the cycle starts over.
  d.reboot();
  TEST_ASSERT_EQUAL(2, (int)d.drv->ctl().manualPendingChat.size());
  // The web chat's slot evicted from the blob by sixteen other chats: still queued.
  for (int i = 0; i < 16; i++) d.folds.noteMessage("guest" + std::to_string(i), 10);
  TEST_ASSERT_NULL(d.folds.peek("web"));
  d.failing = false;
  std::vector<uint32_t> landedMs;
  for (int i = 0; i < 600 && !d.drv->ctl().manualPendingChat.empty(); i++) {
    d.plat.ms += 999;   // about two seconds a pass with tick's own step
    const size_t before = d.done.size();
    d.tick(kT0 + 10 + (uint32_t)i * 2);
    if (d.done.size() > before && d.done.back().manual) landedMs.push_back(d.plat.ms);
  }
  int confirmations = 0;
  for (const auto& t : d.told) confirmations += t.second.rfind("\xE2\x9C\x93 Compacted.", 0) == 0;
  TEST_ASSERT_EQUAL(2, confirmations);
  TEST_ASSERT_TRUE(d.drv->ctl().manualPendingChat.empty());
  // One at a time, two minutes apart: the second manual cycle started at least 120 s
  // after the first one ended.
  TEST_ASSERT_EQUAL(2, (int)landedMs.size());
  const agent::FoldDone& second = d.done.back();
  TEST_ASSERT_TRUE(landedMs[1] - second.elapsedMs - landedMs[0] >= FoldDriver::kManualCooldownMs);
  // A full queue refuses, and says so.
  Device q;
  q.boot();
  for (int i = 0; i < 4; i++) TEST_ASSERT_TRUE(q.drv->requestManual("q" + std::to_string(i)) == FoldDriver::Manual::Queued);
  TEST_ASSERT_TRUE(q.drv->requestManual("late") == FoldDriver::Manual::Full);
}

// A manual request for a chat whose automatic cycle runs ends that cycle uncounted and
// starts a manual one with a keep-0 slice; the rungs that failed go last.
static void test_manual_restarts_an_automatic_cycle_with_keep_zero() {
  Device d;
  d.boot();
  d.backlog("c", 40);
  TEST_ASSERT_TRUE(d.tick(kT0) == FoldTick::Pending);
  TEST_ASSERT_TRUE(d.tick(kT0) == FoldTick::Pending);   // two rungs failed
  TEST_ASSERT_FALSE(d.sliced.back().second);
  TEST_ASSERT_TRUE(d.drv->requestManual("c") == FoldDriver::Manual::Queued);
  TEST_ASSERT_FALSE(d.drv->cycleActive());
  TEST_ASSERT_EQUAL_UINT32(0, d.folds.get("c").failedCycles);   // uncounted
  TEST_ASSERT_TRUE(d.tick(kT0 + 1) == FoldTick::Pending);
  TEST_ASSERT_TRUE(d.sliced.back().second);                     // a manual slice: keep 0
  const agent::FoldStatus st = d.drv->status("c", d.plat.ms, kT0 + 1);
  TEST_ASSERT_TRUE(st.running && st.manual);
  // The failed rungs went last: the manual cycle asked Mistral first, then the Sub model.
  TEST_ASSERT_EQUAL_STRING("mistral/model-mistral", d.askedKeys[2].c_str());
  TEST_ASSERT_EQUAL_STRING("anthropic/sub-anthropic", (st.rungHost + "/" + st.rungModel).c_str());
  TEST_ASSERT_TRUE(d.drv->requestManual("c") == FoldDriver::Manual::AlreadyRunning);
  // A /clear ends the cycle with its slice, uncounted; the queued request is answered.
  d.drv->noteCleared("c");
  TEST_ASSERT_FALSE(d.drv->cycleActive());
  TEST_ASSERT_EQUAL_UINT32(0, d.folds.get("c").failedCycles);
  d.rows["c"].clear();
  d.plat.ms += FoldDriver::kManualCooldownMs;
  TEST_ASSERT_TRUE(d.tick(kT0 + 2) == FoldTick::Skipped);
  TEST_ASSERT_EQUAL_STRING("Nothing to compact yet.", d.told.back().second.c_str());
}

// The daily cap is the device's: five chats due and failing share 12 paid requests a
// local day, and an automatic cycle past it is Deferred, uncounted.
static void test_daily_cap_is_device_wide_across_five_chats() {
  Device d;
  d.boot();
  for (int c = 0; c < 5; c++) d.backlog("chat" + std::to_string(c), 40, 0x100 + c * 0x100);
  int deferred = 0;
  for (int i = 0; i < 3000; i++) deferred += d.tick(kT0 + (uint32_t)i * 20) == FoldTick::Deferred;
  TEST_ASSERT_EQUAL_UINT32(12, d.drv->ctl().paidOn(orch::foldLocalDay(kT0, 0)));
  TEST_ASSERT_EQUAL(12, d.asked);
  TEST_ASSERT_TRUE(deferred > 0);
  TEST_ASSERT_TRUE(LogCapture::contains("the daily cap"));
  // A manual request is exempt (and still counted).
  d.drv->requestManual("chat0");
  d.tick(kT0 + 60000);
  TEST_ASSERT_EQUAL(13, d.asked);
  TEST_ASSERT_EQUAL_UINT32(13, d.drv->ctl().paidOn(orch::foldLocalDay(kT0 + 60000, 0)));
  // The bench op restarts the count.
  d.drv->resetDailyCount();
  TEST_ASSERT_EQUAL_UINT32(0, d.drv->ctl().paidOn(orch::foldLocalDay(kT0 + 60000, 0)));
}

// The DoD week: five chats, every rung failing, a pass a minute for 7 days in a UTC+1
// zone. At most 12 paid requests and at most one alert each local day; at least one alert
// once a day of failing has passed; never a cycle back to back on a chat.
static void test_a_failing_week_on_five_chats_stays_bounded() {
  Device d;
  d.boot();
  for (int c = 0; c < 5; c++) d.backlog("chat" + std::to_string(c), 60, 0x1000 + c * 0x1000);
  std::map<uint32_t, int> paidByDay, alertsByDay;
  int alertsAfterADay = 0;
  for (uint32_t m = 0; m < 7u * 24u * 60u; m++) {
    const uint32_t now = kT0 + m * 60;
    const int asked = d.asked;
    const int alerts = d.alerts();
    d.tick(now);
    const uint32_t day = orch::foldLocalDay(now, 60);
    paidByDay[day] += d.asked - asked;
    alertsByDay[day] += d.alerts() - alerts;
    if (now >= kT0 + 24 * 3600) alertsAfterADay += d.alerts() - alerts;
  }
  for (const auto& p : paidByDay) TEST_ASSERT_TRUE_MESSAGE(p.second <= 12, "over 12 paid requests in a local day");
  for (const auto& a : alertsByDay) TEST_ASSERT_TRUE_MESSAGE(a.second <= 1, "two alerts in a local day");
  TEST_ASSERT_TRUE_MESSAGE(alertsAfterADay >= 1, "no alert after a day of failing");
  TEST_ASSERT_EQUAL(0, d.alerts() - alertsAfterADay);   // none in the first day
  for (const auto& t : d.told) TEST_ASSERT_TRUE(t.second.find("paused") == std::string::npos);
}

// No clock: the wait lives in RAM on the monotonic clock, the count sits under day 0, no
// alert is sent, nothing is persisted as nextTry.
static void test_no_clock_uses_ram_backoff() {
  Device d;
  d.boot();
  d.backlog("c", 40);
  for (int i = 0; i < 4; i++) d.tick(0);
  TEST_ASSERT_EQUAL_UINT32(1, d.folds.get("c").failedCycles);
  TEST_ASSERT_EQUAL_UINT32(0, d.folds.get("c").nextTry);
  TEST_ASSERT_EQUAL_UINT32(4, d.drv->status("c", d.plat.ms, 0).paidToday);   // day 0, in RAM
  TEST_ASSERT_EQUAL_UINT32(0, d.drv->ctl().paidCount);                        // nothing persisted
  const int asked = d.asked;
  for (int i = 0; i < 100; i++) d.tick(0);   // 100 s: inside the 15 min RAM wait
  TEST_ASSERT_EQUAL(asked, d.asked);
  d.plat.ms += 15 * 60 * 1000;
  d.tick(0);
  TEST_ASSERT_EQUAL(asked + 1, d.asked);
  TEST_ASSERT_EQUAL(0, d.alerts());
}

// Lane CTXR1 (low items). No clock for a whole uptime: the RAM count restarts 24 h after its
// first paid request, so automatic summaries never stop for good waiting for a "next local
// day" that never comes.
static void test_no_clock_cap_rolls_over_after_a_day() {
  Device d;
  d.boot();
  d.backlog("c", 400);
  int firstDay = -1;
  for (int m = 0; m < 30 * 60; m++) {   // 30 hours, a pass a minute, no clock
    d.plat.ms += 60000;
    d.drv->tick(d.plat.ms, 0);
    if (m == 23 * 60) firstDay = d.asked;
  }
  TEST_ASSERT_TRUE_MESSAGE(firstDay <= 12, "the cap holds inside the first day");
  TEST_ASSERT_TRUE_MESSAGE(d.asked > firstDay, "the count rolls over: summaries go on");
  TEST_ASSERT_TRUE(LogCapture::contains("daily count restarts"));
}

// No clock at boot: a chat with a persisted wait (a 12 h backoff from before the restart) is
// not retried at once; it waits a few minutes for the clock, then goes.
static void test_no_clock_boot_holds_a_persisted_wait() {
  Device d;
  d.boot();
  d.backlog("c", 40);
  d.folds.noteCycleFailed("c", kT0);
  d.reboot();
  TEST_ASSERT_TRUE(d.folds.get("c").nextTry != 0);
  for (int i = 0; i < 60; i++) d.tick(0);   // a minute after boot
  TEST_ASSERT_EQUAL(0, d.asked);
  d.plat.ms += FoldDriverHooks::kNoClockGraceMs;
  d.tick(0);
  TEST_ASSERT_TRUE(d.asked > 0);
}

// Two chats failing for over a day, a guest's first: the day's one alert still reaches the
// owner (it used to be spent on the guest's chat, where it was only logged).
static void test_the_daily_alert_is_spent_only_when_it_reaches_the_owner() {
  Device d;
  d.guests = {"guest"};
  d.ownerAlerts = true;
  d.thinText = "";   // every rung answers empty
  d.boot();
  d.backlog("guest", 40);
  d.backlog("owner", 40, 0x400);
  // Both have been failing for 30 hours; the guest is due now, the owner an hour later.
  d.folds.noteCycleFailed("guest", kT0 - 30 * 3600);
  d.folds.noteCycleFailed("owner", kT0 - 30 * 3600);
  d.folds.noteCycleFailed("owner", kT0 - 60);
  for (uint32_t m = 0; m < 3 * 60; m++) d.tick(kT0 + m * 60);
  int ownerAlerts = 0;
  for (const auto& t : d.told) ownerAlerts += t.first == "owner" && t.second.find("hasn't updated since") != std::string::npos;
  TEST_ASSERT_TRUE(LogCapture::contains("fold(guest): cycle failed"));
  TEST_ASSERT_EQUAL_MESSAGE(1, ownerAlerts, "the owner hears about the failing summary the same day");
  for (const auto& t : d.told) TEST_ASSERT_TRUE(t.first != "guest");
}

// A guest chat that refills all day spends at most half the daily cap: the owner's chat,
// due late in the day, still gets its automatic summary.
static void test_a_busy_guest_chat_leaves_the_owner_room() {
  Device d;
  d.failing = false;
  d.guests = {"g"};
  d.boot();
  d.backlog("g", 3000, 0x1000);
  for (uint32_t m = 0; m < 20u * 60u; m += 10) {   // 20 hours of the guest folding
    d.tick(kT0 + m * 60);
    if (!d.folds.get("g").softStart.empty()) continue;
    d.backlog("g", 20, 0x1000 + (int)d.rows["g"].size());   // it refills
  }
  int guestFolds = 0;
  for (const auto& done : d.done) guestFolds += done.chat == "g";
  TEST_ASSERT_TRUE_MESSAGE(guestFolds <= (int)orch::kFoldGuestPaidPerDay, "the guest's share of the cap");
  d.backlog("owner", 40, 0x100);
  for (uint32_t m = 20u * 60u; m < 21u * 60u; m++) d.tick(kT0 + m * 60);
  bool ownerFolded = false;
  for (const auto& done : d.done) ownerFolded = ownerFolded || done.chat == "owner";
  TEST_ASSERT_TRUE_MESSAGE(ownerFolded, "the owner's chat folds the same day");
}

// Review (lane CTXR1): a slice that reads to its bound and keeps a tail at or over the trigger
// (one huge turn) restarts the count under the trigger: the chat is not due again on every
// pass with nothing to fold.
static void test_a_huge_kept_tail_does_not_skip_every_pass() {
  Device d;
  d.boot();
  d.folds.noteMessage("c", 60 * 1024);
  d.folds.noteTurn("c");
  d.nothingKept = 200 * 1024;
  TEST_ASSERT_TRUE(d.tick(kT0) == FoldTick::Skipped);
  for (int i = 1; i < 20; i++) TEST_ASSERT_TRUE(d.tick(kT0 + (uint32_t)i * 60) == FoldTick::Idle);
  TEST_ASSERT_TRUE(orch::bytesAfterThru(d.folds.get("c")) < 48u * 1024u);
}

// v5.1.2 fix review E: rows the store keeps refusing stay unread (fail-closed: the anchor never
// passes them), but not silently. After a day of it the owner hears once a local day, and a
// Summarize now says the truth, not "try again in a few minutes".
static void test_rows_unread_for_a_day_alert_the_owner_once_a_day() {
  Device d;
  d.boot();
  d.backlog("c", 40);
  d.unreadAlways = true;
  for (uint32_t m = 0; m < 3u * 24u * 60u; m += 5) d.tick(kT0 + m * 60);
  int alerts = 0;
  for (const auto& t : d.told) alerts += t.second.find("can't move past them") != std::string::npos;
  TEST_ASSERT_TRUE_MESSAGE(alerts >= 1 && alerts <= 3, std::to_string(alerts).c_str());
  TEST_ASSERT_TRUE_MESSAGE(d.folds.get("c").thru.empty(), "the anchor never passes rows it could not read");
  d.told.clear();
  d.drv->requestManual("c");
  for (int i = 0; i < 3; i++) d.tick(kT0 + 4u * 86400u + (uint32_t)i);
  int answers = 0, minutes = 0;
  for (const auto& t : d.told) {
    answers += t.second.find("can't be read from storage right now") != std::string::npos;
    minutes += t.second.find("Try again in a few minutes") != std::string::npos;
  }
  TEST_ASSERT_EQUAL_MESSAGE(1, answers, "the manual request is answered with the truth");
  TEST_ASSERT_EQUAL(0, minutes);
}

// Fix review, candidate 2: the unread clock survives a Wait pass and a restart (it is kept in the
// fold control file), so on a flaky card that also gives Wait passes and restarts, the alert
// still comes once a day has passed, and once.
static void test_the_unread_clock_survives_waits_and_restarts() {
  Device d;
  d.boot();
  d.backlog("c", 40);
  d.stateScript = {agent::SliceState::Unread, agent::SliceState::Wait, agent::SliceState::Unread};
  for (uint32_t m = 0; m <= 25u * 60u; m += 10) {
    if (m == 8u * 60u || m == 16u * 60u) d.reboot();
    d.plat.ms += 600000;
    d.drv->tick(d.plat.ms, kT0 + m * 60);
  }
  int alerts = 0;
  for (const auto& t : d.told) alerts += t.second.find("can't move past them") != std::string::npos;
  TEST_ASSERT_EQUAL_MESSAGE(1, alerts, "one alert after a day of unread rows");
  TEST_ASSERT_TRUE(d.ctlText.find("unreadSince=c:") != std::string::npos);
}

// Candidate 4, F1: the "due, not started" line comes at most once an hour for each chat and
// cause, however many chats are held in the same pass (one key in total let two held chats
// write a durable line each on every pass).
static void test_due_not_started_lines_are_bounded_per_chat_and_cause() {
  Device d;
  d.failing = false;
  d.guests = {"g1", "g2"};
  d.boot();
  d.backlog("g1", 40);
  d.backlog("g2", 40, 0x400);
  d.ctlText = "";
  // The guests' share of the day is spent.
  orch::FoldCtl c;
  c.notePaid(orch::foldLocalDay(kT0, 0), 6, true);
  d.ctlText = c.encode();
  d.reboot();
  LogCapture::lines().clear();
  for (uint32_t s = 0; s < 2u * 3600u; s += 20) d.tick(kT0 + s);   // two hours, a pass every 20 s
  int lines = 0;
  for (const auto& l : LogCapture::lines()) lines += l.find("due, not started") != std::string::npos;
  TEST_ASSERT_TRUE_MESSAGE(lines >= 2 && lines <= 6, std::to_string(lines).c_str());
}

// Candidate 4, F3: an automatic cycle that starts in a manual read's yield and is ended when
// the read's next pass comes has paid for its rungs. It counts as a failed cycle (backoff, the
// alert clock): it is never discarded uncounted to start again on the next yield.
static void test_an_automatic_cycle_ended_for_a_manual_read_counts_as_failed() {
  Device d;
  d.waitChats = {"a"};
  d.boot();
  d.backlog("a", 40);
  d.backlog("b", 40, 0x400);
  d.drv->requestManual("a");
  for (int i = 0; i < 160; i++) {   // half-second passes: the manual read's yields between
    d.plat.ms += 500;
    d.drv->tick(d.plat.ms, kT0 + (uint32_t)i / 2);
  }
  const uint32_t failed = d.folds.get("b").failedCycles;
  TEST_ASSERT_TRUE_MESSAGE(failed >= 1, "the ended cycle counts as failed");
  TEST_ASSERT_TRUE_MESSAGE(d.asked <= 4, std::to_string(d.asked).c_str());   // then the backoff holds
  TEST_ASSERT_TRUE(LogCapture::contains("fold(b): cycle failed, next try"));
}

// A fresh device (no blob, no control file): nothing due, no alert, no wait, no file.
static void test_fresh_device_is_idle_and_quiet() {
  Device d;
  d.boot();
  for (int i = 0; i < 50; i++) TEST_ASSERT_TRUE(d.tick(kT0 + (uint32_t)i) == FoldTick::Idle);
  TEST_ASSERT_EQUAL(0, d.asked);
  TEST_ASSERT_EQUAL(0, (int)d.told.size());
  TEST_ASSERT_EQUAL_STRING("", d.ctlText.c_str());
}

// A new firmware restarts every wait: a chat 12 h into its wait under the old image folds
// on the first pass of the new one.
static void test_new_firmware_restarts_the_wait() {
  Device d;
  d.firmware = "5.1.1";
  d.boot();
  d.backlog("c", 40);
  for (int i = 0; i < 4; i++) d.tick(kT0);
  TEST_ASSERT_TRUE(d.ctlText.find("fwAtLastFail=5.1.1") != std::string::npos);
  TEST_ASSERT_TRUE(d.tick(kT0 + 60) == FoldTick::Idle);   // waiting 15 min
  d.firmware = "5.1.2";
  d.reboot();
  TEST_ASSERT_TRUE(d.tick(kT0 + 61) == FoldTick::Pending);
  TEST_ASSERT_TRUE(LogCapture::contains("fold: firmware 5.1.2 follows 5.1.1"));
}

// The Lumi blob (thru m000017d9, softStart m00001c96, paused=1 fails=3, a 13-day-old
// summary): due on the first pass after the update with no /compact, and the catch-up
// chains cycles oldest first, each starting at the row after the last anchor (no id gap):
// automatically within the daily cap (two days for its 1,213 rows), or with one Summarize
// now (exempt from the cap) and one confirmation for the whole span.
struct LumiRig {
  Device d;
  std::string chat;
  LumiRig() {
    std::ifstream f("test/support/fixtures/fold_blob_v5_1_1_lumi.txt", std::ios::binary);
    std::stringstream ss;
    ss << f.rdbuf();
    d.blobIO.blob = ss.str();
    d.boot();
    for (const std::string& c : d.folds.chatIds())
      if (d.folds.get(c).thru == "m000017d9") chat = c;
    for (int id = 0x17d0; id <= 0x1ca0; id++) {
      char buf[16];
      std::snprintf(buf, sizeof(buf), "m%08x", id);
      d.rows[chat].push_back(buf);
    }
    d.failing = false;
  }
  bool caughtUp() const { return !(d.folds.get(chat).thru < "m00001c96"); }
  void assertNoGap() const {
    std::string prevThru = "m000017d9";
    for (const agent::FoldDone& done : d.done) {
      char next[16];
      std::snprintf(next, sizeof(next), "m%08x", (unsigned)std::strtoul(prevThru.c_str() + 1, nullptr, 16) + 1);
      TEST_ASSERT_TRUE_MESSAGE(done.slice.digest.rfind(std::string("row ") + next, 0) == 0, "an id gap");
      prevThru = done.slice.thru;
    }
  }
};

static void test_lumi_blob_folds_forward_with_no_id_gap() {
  LumiRig a;
  TEST_ASSERT_FALSE(a.chat.empty());
  TEST_ASSERT_TRUE(a.d.tick(kT0) == FoldTick::Ok);   // the first pass, no request
  for (uint32_t m = 1; m < 3u * 24u * 60u && !a.caughtUp(); m++) a.d.tick(kT0 + m * 60);
  TEST_ASSERT_TRUE(a.caughtUp());
  TEST_ASSERT_TRUE(a.d.done.size() > 12);   // more than one day of the cap
  a.assertNoGap();
  for (const auto& t : a.d.told) TEST_ASSERT_TRUE(t.second.find("/compact") == std::string::npos);
  // One Summarize now: the whole backlog in one catch-up, past the cap, one confirmation.
  LumiRig m;
  m.d.quiet = false;   // the owner is chatting: a manual catch-up does not wait for quiet
  m.d.drv->requestManual(m.chat);
  for (int i = 0; i < 200 && !m.d.drv->ctl().manualPendingChat.empty(); i++) m.d.tick(kT0 + (uint32_t)i);
  TEST_ASSERT_TRUE(m.caughtUp());
  TEST_ASSERT_TRUE(m.d.done.size() > 12);
  m.assertNoGap();
  TEST_ASSERT_EQUAL(1, (int)m.d.told.size());
  TEST_ASSERT_TRUE(m.d.told[0].second.find("Summary updated: covered 12") != std::string::npos);
}

// The status the web reads: every field, never null; the badge and the button's reason.
static void test_status_json_fields_and_badge() {
  Device d;
  d.boot();
  d.backlog("c", 40);
  agent::FoldStatus st = d.drv->status("c", d.plat.ms, kT0);
  std::string j = agent::foldStatusJson(st, kT0, 0);
  for (const char* k : {"\"summaryAgeH\":-1", "\"thru\":", "\"lastOk\":0", "\"failedCycles\":0", "\"nextTry\":0",
                        "\"paidToday\":0", "\"cap\":12", "\"rung\":\"\"", "\"state\":\"behind\"", "\"canRun\":true",
                        "\"uncovered\":true"})
    TEST_ASSERT_TRUE_MESSAGE(j.find(k) != std::string::npos, k);
  TEST_ASSERT_TRUE(j.find("null") == std::string::npos);
  d.drv->requestManual("c");
  d.tick(kT0);
  st = d.drv->status("c", d.plat.ms, kT0);
  j = agent::foldStatusJson(st, kT0, 0);
  TEST_ASSERT_TRUE(j.find("\"rung\":\"2/4\"") != std::string::npos);
  TEST_ASSERT_TRUE(j.find("\"canRun\":false") != std::string::npos);
  TEST_ASSERT_TRUE(j.find("\"why\":\"Summarizing now.\"") != std::string::npos);
  d.failing = false;
  d.tick(kT0 + 1);
  st = d.drv->status("c", d.plat.ms, kT0 + 1 + 3600);
  j = agent::foldStatusJson(st, kT0 + 1 + 3600, 0);
  TEST_ASSERT_TRUE_MESSAGE(j.find("\"summaryAgeH\":1") != std::string::npos, j.c_str());
  TEST_ASSERT_TRUE(j.find("\"state\":\"fresh\"") != std::string::npos);
  TEST_ASSERT_TRUE(j.find("\"cooldownS\":120") != std::string::npos);   // the 2-minute cooldown
  TEST_ASSERT_TRUE(j.find("\"why\":\"Ready again in a moment.\"") != std::string::npos);
  st.cooldownS = 0;
  TEST_ASSERT_TRUE(agent::foldStatusJson(st, kT0 + 30 * 3600, 0).find("\"state\":\"stale\"") != std::string::npos);
  TEST_ASSERT_TRUE(agent::foldStatusJson(st, kT0 + 80 * 3600, 0).find("\"state\":\"behind\"") != std::string::npos);
}

// Review fixes (prism, 16 Nov). (1) No clock: paid requests count under day 0 in RAM and
// never overwrite a real day's persisted count. (2) A manual request for another chat does
// not wait behind an automatic cycle the quiet gate holds. (3) Every rung answering a long
// summary with other headings (another language) lands the longest instead of a backoff;
// "Noted." from every rung still fails the cycle.
static void test_review_fixes_cap_preemption_and_thin_fallback() {
  {
    Device d;
    d.boot();
    for (int c = 0; c < 3; c++) d.backlog("chat" + std::to_string(c), 40, 0x100 + c * 0x100);
    for (int i = 0; i < 400; i++) d.tick(kT0 + (uint32_t)i * 30);   // day D: the cap
    TEST_ASSERT_EQUAL_UINT32(12, d.drv->ctl().paidOn(orch::foldLocalDay(kT0, 0)));
    d.reboot();
    d.plat.ms += 10 * 3600 * 1000;   // the RAM waits are long over
    for (int i = 0; i < 400; i++) d.tick(0);   // before the clock syncs
    TEST_ASSERT_EQUAL_UINT32(12, d.drv->ctl().paidOn(orch::foldLocalDay(kT0, 0)));   // day D's count kept
    const int asked = d.asked;
    for (int i = 0; i < 50; i++) d.tick(kT0 + 13000 + (uint32_t)i);   // synced, still day D
    TEST_ASSERT_EQUAL(asked, d.asked);
  }
  {
    Device d;
    d.boot();
    d.backlog("a", 40);
    d.backlog("web", 30, 0x500);
    TEST_ASSERT_TRUE(d.tick(kT0) == FoldTick::Pending);   // an automatic cycle on "a"
    d.quiet = false;                                       // the owner keeps writing
    TEST_ASSERT_TRUE(d.tick(kT0 + 1) == FoldTick::Deferred);
    d.drv->requestManual("web");
    TEST_ASSERT_TRUE(d.tick(kT0 + 2) == FoldTick::Pending);   // the manual one starts at once
    TEST_ASSERT_TRUE(d.drv->status("web", d.plat.ms, kT0 + 2).running);
    TEST_ASSERT_EQUAL_UINT32(0, d.folds.get("a").failedCycles);   // the automatic one yielded, uncounted
  }
  {
    Device d;
    d.boot();
    d.backlog("c", 40);
    d.thinText = "Resume de la conversation.\n";
    while (d.thinText.size() < 1200) d.thinText += "- Le proprietaire note ses repas et ses courses au marche.\n";
    for (int i = 0; i < 3; i++) TEST_ASSERT_TRUE(d.tick(kT0) == FoldTick::Pending);
    TEST_ASSERT_TRUE(d.tick(kT0) == FoldTick::Ok);
    TEST_ASSERT_TRUE(d.folds.get("c").summary.find("Le proprietaire") != std::string::npos);
    TEST_ASSERT_TRUE(LogCapture::contains("keeping the longest"));
    Device n;
    n.boot();
    n.backlog("c", 40);
    for (int i = 0; i < 3; i++) n.tick(kT0);
    TEST_ASSERT_TRUE(n.tick(kT0) == FoldTick::CycleFailed);
    TEST_ASSERT_TRUE(n.folds.get("c").summary.empty());
  }
}

// The copy: the confirmation, the failure notice and the daily alert; never "paused".
static void test_owner_copy() {
  FoldSlice s;
  s.rows = 1;
  TEST_ASSERT_EQUAL_STRING("\xE2\x9C\x93 Compacted. Summary updated: covered 1 message.",
                           FoldDriver::confirmation(s).c_str());
  s.rows = 12;
  s.firstDate = s.lastDate = "2026-11-16";
  TEST_ASSERT_EQUAL_STRING("\xE2\x9C\x93 Compacted. Summary updated: covered 12 messages on 2026-11-16.",
                           FoldDriver::confirmation(s).c_str());
  TEST_ASSERT_EQUAL_STRING("\xE2\x9A\xA0 Couldn't update the summary just now. It tries again by itself in 15 minutes.",
                           FoldDriver::failureNotice(0, 900, 0).c_str());
  TEST_ASSERT_EQUAL_STRING(
      "\xE2\x9A\xA0 Couldn't update the summary just now. It tries again by itself at 13:15 (2026-11-16).",
      FoldDriver::failureNotice(kT0 + 12 * 3600 + 900, 900, 60).c_str());
  const std::string alert = FoldDriver::alertNotice(kT0 - 13 * 86400, 0);
  TEST_ASSERT_TRUE(alert.find("since 2026-11-03") != std::string::npos);
  for (const std::string& t : {alert, FoldDriver::failureNotice(0, 900, 0)})
    TEST_ASSERT_TRUE(t.find("paused") == std::string::npos && t.find("\xE2\x80\x94") == std::string::npos);
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_pending_at_every_rung_then_one_cycle_failed);
  RUN_TEST(test_answer_at_rung_k_lands_and_confirms_once);
  RUN_TEST(test_manual_request_is_never_lost);
  RUN_TEST(test_manual_restarts_an_automatic_cycle_with_keep_zero);
  RUN_TEST(test_daily_cap_is_device_wide_across_five_chats);
  RUN_TEST(test_a_failing_week_on_five_chats_stays_bounded);
  RUN_TEST(test_no_clock_uses_ram_backoff);
  RUN_TEST(test_no_clock_cap_rolls_over_after_a_day);
  RUN_TEST(test_no_clock_boot_holds_a_persisted_wait);
  RUN_TEST(test_the_daily_alert_is_spent_only_when_it_reaches_the_owner);
  RUN_TEST(test_a_busy_guest_chat_leaves_the_owner_room);
  RUN_TEST(test_a_huge_kept_tail_does_not_skip_every_pass);
  RUN_TEST(test_rows_unread_for_a_day_alert_the_owner_once_a_day);
  RUN_TEST(test_the_unread_clock_survives_waits_and_restarts);
  RUN_TEST(test_due_not_started_lines_are_bounded_per_chat_and_cause);
  RUN_TEST(test_an_automatic_cycle_ended_for_a_manual_read_counts_as_failed);
  RUN_TEST(test_fresh_device_is_idle_and_quiet);
  RUN_TEST(test_new_firmware_restarts_the_wait);
  RUN_TEST(test_lumi_blob_folds_forward_with_no_id_gap);
  RUN_TEST(test_status_json_fields_and_badge);
  RUN_TEST(test_owner_copy);
  RUN_TEST(test_review_fixes_cap_preemption_and_thin_fallback);
  return UNITY_END();
}
