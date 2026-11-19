#pragma once
#include <mutex>
#include <string>

#include "posix_fs.h"                    // fsutil::readFile, writeFileAtomic
#include "nimbus/harness/log.h"          // agent::hlog
#include "nimbus/orch/budget_command.h"  // limitsViewFromJson - the ceiling's state, the device's shape
#include "nimbus/orch/budget_governor.h"
#include "nimbus/orch/spend_ceiling.h"   // NoticeChannel

// hosted_budget - the owner's history profile and the nightly spending advice on a Virtual
// Nimbus (lane RB-BG, parity with the device's src/agent/budget_profile_store.cpp; the rules are
// lib/core's nimbus/orch/budget_governor.h).
//
// One file on the instance volume, <memDir>/budget.json, written whole through a temporary
// file and a rename; a missing or unreadable file reads as full (the default: each head's own
// marks, as released v5.1.2), and status() says so. The constructor installs the profile every
// head's context is derived under. night() runs from the engine thread's idle tick (the rig's
// spendTick) against the instance's spend ceiling state; it never changes the profile (owner
// ruling 81). When the month is on course past 0.9 of the ceiling it logs the advice and owes
// the owner one line, once a month, on the web chat and on Telegram (take(), from the rig's
// spendNotice, as the ceiling's notices go). A corrupt file keeps the advice off (an owner's
// profile that still reads is kept). The hosted instance has no /budget command: choose() is
// the seam a later settings field uses.
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

  // One night at `epoch` (the instance's day start `startMin`, `offsetMin` east of UTC) against
  // `spendStateJson` (SpendCeilingVn::stateJson). A night whose write fails is undone and runs
  // again on the next tick. Returns the night's advice (Kind::None when nothing ran).
  // `head`: the head the instance runs, so the advice names sizes in tokens (empty host = not known).
  struct Head {
    std::string host, model;
    bool loopOn = true;
  };
  nimbus::orch::BudgetAdvice night(uint64_t epoch, int32_t offsetMin, uint16_t startMin,
                                   const std::string& spendStateJson, const Head& head = Head()) {
    nimbus::orch::BudgetAdvice a;
    nimbus::orch::LimitsView v;
    if (!nimbus::orch::limitsViewFromJson(spendStateJson, v) || !v.loaded) return a;
    nimbus::orch::GovernorFacts g;
    if (!nimbus::orch::governorFactsAt(epoch, offsetMin, startMin, {v.monthKey, v.spentMicro, v.active, v.limitMicro}, g))
      return a;
    for (uint8_t i = 0; !head.host.empty() && i < 5; i++)
      g.dueTokens[i] =
          nimbus::orch::profileMarksFor(head.host, head.model, head.loopOn, (nimbus::orch::HistoryProfile)i).dueTokens;
    std::lock_guard<std::mutex> lk(mu_);
    if (read_ == nimbus::orch::BudgetFileRead::Corrupt) return a;
    const nimbus::orch::BudgetFile before = file_;
    a = nimbus::orch::governorNight(file_, g);
    if (a.kind == nimbus::orch::BudgetAdvice::Kind::None) return a;
    if (!saveLocked()) {
      file_ = before;
      agent::hlog::logf("budget: the spending advice's night did not store - tried again on the next tick");
      return nimbus::orch::BudgetAdvice();
    }
    if (!a.line.empty()) {
      agent::hlog::logf("budget: %s", a.record.c_str());
      owed_ = a.line;
      owedTg_ = owedWeb_ = true;
    }
    return a;
  }

  // The owner's line the advice owes on `ch` (Telegram or the web chat); true once, then it is
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
  // `now`: the head's marks under the profile in force (profileMarksFor), for tokens first.
  std::string status(const nimbus::orch::ProfileMarks& now = nimbus::orch::ProfileMarks()) {
    std::lock_guard<std::mutex> lk(mu_);
    return nimbus::orch::profileStatusText(file_, read_, now);
  }
  nimbus::orch::BudgetFileRead readState() {
    std::lock_guard<std::mutex> lk(mu_);
    return read_;
  }
  // The owner's choice; false when it did not store (nothing changed).
  bool choose(nimbus::orch::HistoryProfile p, int64_t day) {
    std::lock_guard<std::mutex> lk(mu_);
    if (file_.profile == p && read_ != nimbus::orch::BudgetFileRead::Corrupt) return true;
    const nimbus::orch::BudgetFile before = file_;
    nimbus::orch::chooseProfile(file_, p, day);
    if (!saveLocked()) {
      file_ = before;
      return false;
    }
    nimbus::orch::installHistoryProfile(file_.effective());
    return true;
  }

 private:
  bool saveLocked() {
    const bool ok = fsutil::writeFileAtomic(path_, nimbus::orch::budgetFileJson(file_));
    if (ok) read_ = nimbus::orch::BudgetFileRead::Ok;
    return ok;
  }

  std::string path_;
  std::mutex mu_;
  nimbus::orch::BudgetFile file_;
  nimbus::orch::BudgetFileRead read_ = nimbus::orch::BudgetFileRead::Missing;
  std::string owed_;
  bool owedTg_ = false, owedWeb_ = false;
};

}  // namespace nimbusd
