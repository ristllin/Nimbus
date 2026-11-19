#include "budget_profile_store.h"

#include <Arduino.h>
#include <LittleFS.h>

#include <ctime>
#include <mutex>

#include "budget_command.h"   // recordDeviceChange - the Usage page's change record
#include "fs_atomic.h"
#include "spend_ceiling_store.h"
#include "store.h"
#include "nimbus/harness/conversation.h"   // utcOffsetAtUnix
#include "nimbus/orch/budget_command.h"    // limitsViewFromJson
#include "nimbus/orch/budget_governor.h"
#include "nimbus/orch/day_start.h"       // calendarDay
#include "nimbus/orch/spend_ceiling.h"     // kCeilingClockValid
#include "../sys/agent_log.h"

namespace agent {
namespace budgetprofile {

namespace {

using nimbus::orch::BudgetFile;
using nimbus::orch::BudgetFileRead;
using nimbus::orch::GovernorStep;

constexpr const char* kPath = "/data/budget.json";
constexpr size_t kMax = 2048;   // a longer file is not ours

std::mutex g_mx;   // the poll task writes; the web task reads the status line
BudgetFile g_file;
BudgetFileRead g_read = BudgetFileRead::Missing;
bool g_loaded = false;
std::string g_owed;   // the owner's line a step owes

// Caller holds g_mx.
void installLocked() { nimbus::orch::installHistoryProfile(g_file.effective()); }

// Caller holds g_mx. A failed write keeps the RAM state (the profile in force) and says so.
bool saveLocked() {
  LittleFS.mkdir("/data");
  const bool ok = agent::writeFileAtomic(kPath, nimbus::orch::budgetFileJson(g_file), "budget_json");
  if (ok) g_read = BudgetFileRead::Ok;
  return ok;
}

int64_t today() {
  const time_t now = time(nullptr);
  if (now < nimbus::orch::kCeilingClockValid) return 0;
  return nimbus::orch::day::calendarDay((uint64_t)now, agent::utcOffsetAtUnix((uint32_t)now));
}

}  // namespace

void load() {
  std::lock_guard<std::mutex> lk(g_mx);
  if (g_loaded || !agent::flashMounted()) return;
  g_loaded = true;
  std::string text;
  if (LittleFS.exists(kPath)) {
    File f = LittleFS.open(kPath, "r");
    if (f && f.size() <= kMax) text = f.readString().c_str();
    else text = "?";   // unreadable or not ours: corrupt
    if (f) f.close();
  }
  g_read = nimbus::orch::parseBudgetFile(text, g_file);
  installLocked();
  if (g_read == BudgetFileRead::Corrupt)
    alogf("budget: /data/budget.json could not be read - the default history profile (%s) applies",
          nimbus::orch::historyProfileSlug(g_file.effective()));
  else
    alogf("budget: history profile %s (%s)", nimbus::orch::historyProfileSlug(g_file.effective()),
          g_file.pinned ? "chosen by the owner" : g_read == BudgetFileRead::Missing ? "the default" : "the device's");
}

void tick() {
  load();
  const time_t now = time(nullptr);
  if (now < nimbus::orch::kCeilingClockValid) return;
  const std::string state = agent::spend::stateJson();
  if (state.empty()) return;   // a commit holds the spend state: next minute
  nimbus::orch::LimitsView v;
  if (!nimbus::orch::limitsViewFromJson(state, v) || !v.loaded) return;
  nimbus::orch::GovernorFacts g;
  if (!nimbus::orch::governorFactsAt((uint64_t)now, agent::utcOffsetAtUnix((uint32_t)now), agent::store::dayStartMin(),
                                     v.monthKey, v.spentMicro, v.active, v.limitMicro, g))
    return;
  GovernorStep st;
  {
    std::lock_guard<std::mutex> lk(g_mx);
    if (!g_loaded) return;
    if (g.runningDay <= g_file.night && g_file.night) return;   // tonight's run is done
    st = nimbus::orch::governorNight(g_file, g);
    installLocked();
    if (!saveLocked()) alogf("budget: the governor's night did not store (the profile in force stays)");
    if (!st.line.empty()) g_owed = st.line;
  }
  if (st.changed()) {
    alogf("budget: history profile %s (was %s), by the device", nimbus::orch::historyProfileSlug(st.to),
          nimbus::orch::historyProfileSlug(st.from));
    agent::budget::recordDeviceChange(st.record);
  } else if (!st.line.empty()) {
    alogf("budget: the owner is told the month is on course past the ceiling (%s)",
          nimbus::orch::usdText(st.projectedMicro).c_str());
  }
}

std::string statusLine() {
  std::lock_guard<std::mutex> lk(g_mx);
  return nimbus::orch::profileStatusText(g_file, g_read);
}

std::string command(const std::string& arg, bool& changed, std::string& record) {
  changed = false;
  load();
  std::lock_guard<std::mutex> lk(g_mx);
  if (arg.empty()) return nimbus::orch::profileReplyText(g_file, arg, true);
  const nimbus::orch::HistoryProfile was = g_file.effective();
  const bool wasPinned = g_file.pinned;
  const BudgetFile before = g_file;
  if (arg == "auto") {
    nimbus::orch::unpinProfile(g_file, today());
  } else {
    nimbus::orch::HistoryProfile p;
    if (!nimbus::orch::parseHistoryProfile(arg, p)) return nimbus::orch::profileReplyText(g_file, arg, false);
    nimbus::orch::pinProfile(g_file, p, today());
  }
  if (!saveLocked()) {
    g_file = before;   // nothing changed: the reply says so
    return "The profile could not be stored, so nothing changed. Try again in a moment.";
  }
  installLocked();
  changed = true;
  record = std::string("History profile ") + nimbus::orch::historyProfileSlug(g_file.effective()) + " (was " +
           nimbus::orch::historyProfileSlug(was) + (wasPinned ? ", chosen" : "") + "), " +
           (g_file.pinned ? "chosen by the owner" : "the device chooses again");
  return nimbus::orch::profileReplyText(g_file, arg, true);
}

std::string nextNotice() {
  std::lock_guard<std::mutex> lk(g_mx);
  return g_owed;
}

void noticeDelivered() {
  std::lock_guard<std::mutex> lk(g_mx);
  g_owed.clear();
}

}  // namespace budgetprofile
}  // namespace agent
