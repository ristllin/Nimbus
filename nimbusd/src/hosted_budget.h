#pragma once
#include <mutex>
#include <string>

#include "posix_fs.h"                    // fsutil::readFile, writeFileAtomic
#include "nimbus/orch/budget_command.h"  // limitsViewFromJson - the ceiling's state, the device's shape
#include "nimbus/orch/budget_governor.h"
#include "nimbus/orch/spend_ceiling.h"   // NoticeChannel
#include "nimbus/harness/log.h"          // agent::hlog

// hosted_budget - the history profile and its governor on a Virtual Nimbus (lane RB-BG,
// parity with the device's src/agent/budget_profile_store.cpp; the rules are lib/core's
// nimbus/orch/budget_governor.h).
//
// One file on the instance volume, <memDir>/budget.json, written whole through a temporary
// file and a rename; a missing or unreadable file reads as the default profile (standard),
// and status() says so. The constructor installs the profile every head's context is derived
// under. night() runs from the engine thread's idle tick (the rig's spendTick) against the
// instance's spend ceiling state: on the first tick after the instance's day start it runs
// the governor's night, stores the file, installs a new profile, logs it, and owes the owner
// one line on the web chat and on Telegram (take(), from the rig's spendNotice, as the
// ceiling's notices go). A corrupt file keeps the governor off (a pin that still reads is kept). The hosted instance
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
  // UTC) against `spendStateJson` (SpendCeilingVn::stateJson). A step that stores is installed,
  // logged and owed to the owner on both channels (take()); a night whose write fails is undone
  // and runs again on the next tick. Nothing runs while the file reads as corrupt. Returns the
  // step (Kind::None when nothing ran).
  nimbus::orch::GovernorStep night(uint64_t epoch, int32_t offsetMin, uint16_t startMin,
                                   const std::string& spendStateJson) {
    nimbus::orch::GovernorStep st;
    nimbus::orch::LimitsView v;
    if (!nimbus::orch::limitsViewFromJson(spendStateJson, v) || !v.loaded) return st;
    nimbus::orch::GovernorFacts g;
    if (!nimbus::orch::governorFactsAt(epoch, offsetMin, startMin, {v.monthKey, v.spentMicro, v.active, v.limitMicro}, g))
      return st;
    std::lock_guard<std::mutex> lk(mu_);
    if (read_ == nimbus::orch::BudgetFileRead::Corrupt) return st;
    const nimbus::orch::BudgetFile before = file_;
    st = nimbus::orch::governorNight(file_, g);
    if (st.kind == nimbus::orch::GovernorStep::Kind::None) return st;
    if (!saveLocked()) {
      file_ = before;
      agent::hlog::logf("budget: the governor's night did not store - tried again on the next tick");
      return nimbus::orch::GovernorStep();
    }
    nimbus::orch::installHistoryProfile(file_.effective());
    if (st.changed()) agent::hlog::logf("budget: %s", st.record.c_str());
    if (!st.line.empty()) {
      owed_ = st.line;
      owedTg_ = owedWeb_ = true;
    }
    return st;
  }

  // The owner's line a step owes on `ch` (Telegram or the web chat); true once, then it is
  // marked delivered (the rig records it in that chat as it does the ceiling's notices).
  bool take(nimbus::orch::NoticeChannel ch, std::string& text) {
    std::lock_guard<std::mutex> lk(mu_);
    bool& owed = ch == nimbus::orch::NoticeChannel::Telegram ? owedTg_ : owedWeb_;
    if (!owed) return false;
    owed = false;
    text = owed_;
    return true;
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
    if (file_.pinned && file_.pinnedProfile == p && read_ != nimbus::orch::BudgetFileRead::Corrupt) return true;
    const nimbus::orch::BudgetFile before = file_;
    nimbus::orch::pinProfile(file_, p, day);
    return commitLocked(before);
  }
  bool unpin(int64_t day) {
    std::lock_guard<std::mutex> lk(mu_);
    if (!file_.pinned && read_ != nimbus::orch::BudgetFileRead::Corrupt) return true;
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
  std::string owed_;
  bool owedTg_ = false, owedWeb_ = false;
};

}  // namespace nimbusd
