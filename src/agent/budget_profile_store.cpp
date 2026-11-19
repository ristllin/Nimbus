#include "budget_profile_store.h"

#include <Arduino.h>
#include <LittleFS.h>

#include <ctime>
#include <mutex>

#include "budget_command.h"   // recordDeviceNote - the Usage page's record
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
using nimbus::orch::BudgetAdvice;

constexpr const char* kPath = "/data/budget.json";
constexpr size_t kMax = 2048;   // a longer file is not ours

std::mutex g_mx;   // the poll task writes; the web task reads the status line
BudgetFile g_file;
BudgetFileRead g_read = BudgetFileRead::Missing;
bool g_loaded = false;
std::string g_owed;          // the owner's line the advice owes
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

// The head's marks under each profile, tiny to full (the owner reads tokens: ruling 82).
void headMarks(nimbus::orch::ProfileMarks out[5]) {
  const String h = agent::store::resolvedOrchHost();
  const std::string host = h.c_str(), model = agent::store::orchModel(h).c_str();
  const bool loop = agent::store::orchToolLoop();
  for (uint8_t i = 0; i < 5; i++)
    out[i] = nimbus::orch::profileMarksFor(host, model, loop, (nimbus::orch::HistoryProfile)i);
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
  const bool chosen = g_file.profile != nimbus::orch::kDefaultHistoryProfile;
  if (g_read == BudgetFileRead::Corrupt)
    alogf("budget: /data/budget.json could not be read - history profile %s (%s), the advice waits for "
          "/budget profile", nimbus::orch::historyProfileSlug(g_file.effective()),
          chosen ? "the owner's choice" : "the default");
  else
    alogf("budget: history profile %s (%s)", nimbus::orch::historyProfileSlug(g_file.effective()),
          chosen ? "chosen by the owner" : "the default");
}

namespace {

// The facts of tonight from the clock and the spend store; false while either is not ready.
bool factsNow(nimbus::orch::GovernorFacts& g) {
  const time_t now = time(nullptr);
  if (now < nimbus::orch::kCeilingClockValid) return false;
  const std::string state = agent::spend::stateJson();
  if (state.empty()) return false;   // a commit holds the spend state: next minute
  nimbus::orch::LimitsView v;
  if (!nimbus::orch::limitsViewFromJson(state, v) || !v.loaded) return false;
  const nimbus::orch::SpendFacts spend{v.monthKey, v.spentMicro, v.active, v.limitMicro};
  if (!nimbus::orch::governorFactsAt((uint64_t)now, agent::utcOffsetAtUnix((uint32_t)now),
                                     agent::store::dayStartMin(), spend, g))
    return false;
  nimbus::orch::ProfileMarks m[5];
  headMarks(m);
  for (uint8_t i = 0; i < 5; i++) g.dueTokens[i] = m[i].dueTokens;
  return true;
}

// The night under the lock: stored and owed, or undone when the write fails. It never changes
// the profile (owner ruling 81).
BudgetAdvice runNight(const nimbus::orch::GovernorFacts& g) {
  std::lock_guard<std::mutex> lk(g_mx);
  if (!g_loaded || g_read == BudgetFileRead::Corrupt) return BudgetAdvice();   // the owner's next command writes it
  const BudgetFile before = g_file;
  const BudgetAdvice a = nimbus::orch::governorNight(g_file, g);
  if (a.kind == BudgetAdvice::Kind::None) return a;   // tonight's run is done
  if (!saveLocked()) {   // undone: the next minute runs the night again
    g_file = before;
    alogf("budget: the spending advice's night did not store - tried again in a minute");
    return BudgetAdvice();
  }
  if (!a.line.empty()) {
    g_owed = a.line;
    g_owedTg = g_owedWeb = true;
  }
  return a;
}

}  // namespace

void tick() {
  load();
  nimbus::orch::GovernorFacts g;
  if (!factsNow(g)) return;
  const BudgetAdvice a = runNight(g);
  if (a.kind != BudgetAdvice::Kind::Advice) return;
  alogf("budget: the owner is told the month is on course past the ceiling (%s; %s)",
        nimbus::orch::usdText(a.projectedMicro).c_str(), a.fits ? nimbus::orch::historyProfileSlug(a.fit) : "no fit");
  agent::budget::recordDeviceNote(a.record);
}

std::string statusLine() {
  nimbus::orch::ProfileMarks m[5];
  headMarks(m);
  std::lock_guard<std::mutex> lk(g_mx);
  return nimbus::orch::profileStatusText(g_file, g_read, m[(uint8_t)g_file.profile]);
}

std::string command(const std::string& arg, bool& changed, std::string& record) {
  changed = false;
  load();
  nimbus::orch::ProfileMarks m[5];
  headMarks(m);
  std::lock_guard<std::mutex> lk(g_mx);
  if (arg.empty()) return nimbus::orch::profileReplyText(g_file, arg, true, m);
  nimbus::orch::HistoryProfile p = nimbus::orch::kDefaultHistoryProfile;
  if (!nimbus::orch::parseHistoryProfile(arg, p)) return nimbus::orch::profileReplyText(g_file, arg, false);
  // Nothing to change (a corrupt file still gets written: the owner's command brings the
  // advice back).
  if (!nimbus::orch::profileCommandChanges(g_file, arg) && g_read != BudgetFileRead::Corrupt)
    return nimbus::orch::profileStatusText(g_file, g_read, m[(uint8_t)g_file.profile]);
  const BudgetFile before = g_file;
  nimbus::orch::chooseProfile(g_file, p, today());
  if (!saveLocked()) {
    g_file = before;   // nothing changed: the reply says so
    return "The profile could not be stored, so nothing changed. Try again in a moment.";
  }
  installLocked();
  changed = true;
  record = std::string("History profile ") + nimbus::orch::historyProfileSlug(g_file.profile) + ", " +
           nimbus::orch::tokensText(m[(uint8_t)g_file.profile].dueTokens) + " (was " +
           nimbus::orch::historyProfileSlug(before.profile) + "), chosen by the owner";
  return nimbus::orch::profileReplyText(g_file, arg, true, m);
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
