#pragma once
#include <mutex>
#include <string>

#include "posix_fs.h"                    // fsutil::readFile, writeFileAtomic
#include "nimbus/orch/budget_command.h"  // limitsViewFromJson - the ceiling's state, the device's shape
#include "nimbus/orch/budget_governor.h"

// hosted_budget - the history profile and its governor on a Virtual Nimbus (lane RB-BG,
// parity with the device's src/agent/budget_profile_store.cpp; the rules are lib/core's
// nimbus/orch/budget_governor.h).
//
// One file on the instance volume, <memDir>/budget.json, written whole through a temporary
// file and a rename; a missing or unreadable file reads as the default profile (standard),
// and status() says so. The constructor installs the profile every head's context is derived
// under. night() runs from the engine thread's idle tick (the rig's spendTick) against the
// instance's spend ceiling state: on the first tick after the instance's day start it runs
// the governor's night, installs a new profile, stores the file and returns the owner's line
// (the rig records it in the web chat as it does the ceiling's notices). The hosted instance
// has no /budget command: pin() and unpin() are the seam a later settings field uses.
//
// Thread-safe: the engine thread ticks, the HTTP thread may read the status.
namespace nimbusd {

class HostedBudget {
 public:
  explicit HostedBudget(std::string path) : path_(std::move(path)) {
    std::lock_guard<std::mutex> lk(mu_);
    std::string text;
    if (!fsutil::readFile(path_, text)) text.clear();
    read_ = nimbus::orch::parseBudgetFile(text, file_);
    nimbus::orch::installHistoryProfile(file_.effective());
  }

  // One governor night at `epoch` (the instance's day start `startMin`, `offsetMin` east of
  // UTC) against `spendStateJson` (SpendCeilingVn::stateJson). Returns the owner's line ("" =
  // none); `step` gets the whole step (the change record's line among it).
  std::string night(uint64_t epoch, int32_t offsetMin, uint16_t startMin, const std::string& spendStateJson,
                    nimbus::orch::GovernorStep* step = nullptr) {
    nimbus::orch::LimitsView v;
    if (!nimbus::orch::limitsViewFromJson(spendStateJson, v) || !v.loaded) return std::string();
    nimbus::orch::GovernorFacts g;
    if (!nimbus::orch::governorFactsAt(epoch, offsetMin, startMin, v.monthKey, v.spentMicro, v.active, v.limitMicro, g))
      return std::string();
    std::lock_guard<std::mutex> lk(mu_);
    if (file_.night && g.runningDay <= file_.night) return std::string();
    const nimbus::orch::GovernorStep st = nimbus::orch::governorNight(file_, g);
    nimbus::orch::installHistoryProfile(file_.effective());
    saveLocked();
    if (step) *step = st;
    return st.line;
  }

  nimbus::orch::HistoryProfile effective() {
    std::lock_guard<std::mutex> lk(mu_);
    return file_.effective();
  }
  std::string status() {
    std::lock_guard<std::mutex> lk(mu_);
    return nimbus::orch::profileStatusText(file_, read_);
  }
  nimbus::orch::BudgetFileRead readState() {
    std::lock_guard<std::mutex> lk(mu_);
    return read_;
  }
  // The owner's choice (pinned), and back to the governor; false when it did not store
  // (nothing changed).
  bool pin(nimbus::orch::HistoryProfile p, int64_t day) {
    std::lock_guard<std::mutex> lk(mu_);
    const nimbus::orch::BudgetFile before = file_;
    nimbus::orch::pinProfile(file_, p, day);
    return commitLocked(before);
  }
  bool unpin(int64_t day) {
    std::lock_guard<std::mutex> lk(mu_);
    const nimbus::orch::BudgetFile before = file_;
    nimbus::orch::unpinProfile(file_, day);
    return commitLocked(before);
  }

 private:
  bool saveLocked() {
    const bool ok = fsutil::writeFileAtomic(path_, nimbus::orch::budgetFileJson(file_));
    if (ok) read_ = nimbus::orch::BudgetFileRead::Ok;
    return ok;
  }
  bool commitLocked(const nimbus::orch::BudgetFile& before) {
    if (!saveLocked()) {
      file_ = before;
      return false;
    }
    nimbus::orch::installHistoryProfile(file_.effective());
    return true;
  }

  std::string path_;
  std::mutex mu_;
  nimbus::orch::BudgetFile file_;
  nimbus::orch::BudgetFileRead read_ = nimbus::orch::BudgetFileRead::Missing;
};

}  // namespace nimbusd
