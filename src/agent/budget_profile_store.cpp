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
std::string g_owed;          // the owner's line a step owes
bool g_owedTg = false, g_owedWeb = false;

// Caller holds g_mx.
void installLocked() { nimbus::orch::installHistoryProfile(g_file.effective()); }

// Caller holds g_mx.
bool saveLocked() {
  LittleFS.mkdir("/data");
  const bool ok = agent::writeFileAtomic(kPath, nimbus::orch::budgetFileJson(g_file), "budget_json");
  if (ok) g_read = BudgetFileRead::Ok;
  return ok;
}

// The file's text: "" when there is none, "?" when it is there but does not read (three tries,
// as the spend store reads its file: a transient open failure is not a missing file).
std::string readText() {
  for (int attempt = 0; attempt < 3; attempt++) {
    if (!LittleFS.exists(kPath)) {
      if (attempt == 2) return std::string();
      delay(20);
      continue;
    }
    File f = LittleFS.open(kPath, "r");
    if (f && f.size() <= kMax) {
      const String body = f.readString();
      f.close();
      if (body.length()) return body.c_str();
    } else if (f) {
      f.close();
      return "?";   // larger than any file this code writes: not ours
    }
    delay(20);
  }
  return "?";
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
  g_read = nimbus::orch::parseBudgetFile(readText(), g_file);
  installLocked();
  if (g_read == BudgetFileRead::Corrupt)
    alogf("budget: /data/budget.json could not be read - history profile %s (%s), the governor waits for "
          "/budget profile", nimbus::orch::historyProfileSlug(g_file.effective()),
          g_file.pinned ? "the owner's pin" : "the default");
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
    if (!g_loaded || g_read == BudgetFileRead::Corrupt) return;   // the owner's next command writes it
    if (g_file.night && g.runningDay <= g_file.night && g.runningDay + nimbus::orch::kGovernorObsNights >= g_file.night)
      return;   // tonight's run is done
    const BudgetFile before = g_file;
    st = nimbus::orch::governorNight(g_file, g);
    if (st.kind == GovernorStep::Kind::None) return;
    if (!saveLocked()) {   // undone: the next minute runs the night again
      g_file = before;
      alogf("budget: the governor's night did not store - tried again in a minute");
      return;
    }
    installLocked();
    if (!st.line.empty()) {
      g_owed = st.line;
      g_owedTg = g_owedWeb = true;
    }
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
  nimbus::orch::HistoryProfile p = nimbus::orch::kDefaultHistoryProfile;
  if (arg != "auto" && !nimbus::orch::parseHistoryProfile(arg, p)) return nimbus::orch::profileReplyText(g_file, arg, false);
  // Nothing to change (and a corrupt file still gets written: the owner's command is what
  // brings the governor back).
  if (!nimbus::orch::profileCommandChanges(g_file, arg) && g_read != BudgetFileRead::Corrupt)
    return nimbus::orch::profileStatusText(g_file, g_read);
  const BudgetFile before = g_file;
  const bool wasPinned = g_file.pinned;
  if (arg == "auto") nimbus::orch::unpinProfile(g_file, today());
  else nimbus::orch::pinProfile(g_file, p, today());
  if (!saveLocked()) {
    g_file = before;   // nothing changed: the reply says so
    return "The profile could not be stored, so nothing changed. Try again in a moment.";
  }
  installLocked();
  changed = true;
  record = std::string("History profile ") + nimbus::orch::historyProfileSlug(g_file.effective()) + " (was " +
           nimbus::orch::historyProfileSlug(before.effective()) + (wasPinned ? ", chosen" : "") + "), " +
           (g_file.pinned ? "chosen by the owner" : "the device chooses again");
  return nimbus::orch::profileReplyText(g_file, arg, true);
}

std::string nextNotice(bool telegram) {
  std::lock_guard<std::mutex> lk(g_mx);
  return (telegram ? g_owedTg : g_owedWeb) ? g_owed : std::string();
}

void noticeDelivered(bool telegram) {
  std::lock_guard<std::mutex> lk(g_mx);
  (telegram ? g_owedTg : g_owedWeb) = false;
}

}  // namespace budgetprofile
}  // namespace agent
