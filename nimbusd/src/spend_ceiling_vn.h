#pragma once
#include <sys/stat.h>

#include <cstdio>
#include <ctime>
#include <mutex>
#include <string>

#include "posix_fs.h"   // fsutil::readFile, writeFileAtomic - fsync'd tmp + rename
#include "nimbus/orch/seam_meter.h"   // the meter every paid call outside a turn reports to
#include "nimbus/orch/spend_ceiling.h"
#include "nimbus/orch/usage_ledger.h"

// spend_ceiling_vn - the monthly spend ceiling on a Virtual Nimbus (lane DEF; ruling
// R7, 21 Oct 2026: the 20 USD ceiling applies to hosted instances too).
//
// The same pure rules as the device (lib/core nimbus/orch/spend_ceiling.h): one count
// per calendar month in the instance's time zone, every provider combined, a freeze
// that every paid request consults (the process-wide gate the engine, the wire and
// the tools read), one owner notice per channel per pause. The state lives in one
// file on the instance volume (<dataDir>/spend_ceiling.txt), written whole through a
// temporary file, an fsync and a rename (a full volume never leaves a cut file in its
// place). A file that is there but cannot be read back is never taken for a zero count:
// the instance starts paused at the default ceiling until the owner raises it or the
// month ends (lane QW4, fail-closed money).
//
// Pricing: a hosted instance keeps no owner prices, so a record is priced with the
// built-in table through a UsageLedger (a model with no price of its own at its
// provider's highest price: fail-closed). A router (Cumulo) key reads its upstream's
// rows; the cloud's own credit metering is a second, separate limit.
//
// Thread-safe: the engine thread commits spend and pumps notices, the HTTP thread
// reads the state and edits the ceiling.
namespace nimbusd {

class SpendCeilingVn {
 public:
  // `hadState`: an earlier release ran here; `telegram`: a bot is set (its owner chat
  // owes the first-start notice too).
  SpendCeilingVn(std::string path, bool hadState, bool telegram)
      : path_(std::move(path)), telegram_(telegram) {
    std::lock_guard<std::mutex> lk(mu_);
    load(hadState);
  }

  // The process-wide gate and the in-flight pricer answer from this instance (one per
  // process: the daemon runs one engine).
  void install() {
    self() = this;
    nimbus::orch::installSpendFreeze(+[] { return self() && self()->frozen(); });
    nimbus::orch::installPendingSpend(
        +[](const nimbus::orch::TurnSpend& s) { return self() ? self()->price(s) : 0; },
        +[](const char*, uint64_t micro) { if (self()) self()->addPending(micro); });
    // Every paid call outside a turn (a sub-agent, an embedding) counts too.
    nimbus::orch::installSeamMeter(+[](nimbus::orch::SpendSeam, const char*,
                                       const nimbus::orch::TurnSpend& s, uint64_t est) {
      if (self()) self()->commitMicro(nimbus::orch::addSatMicro(self()->price(s), est));
    });
  }
  ~SpendCeilingVn() {
    if (self() == this) {
      nimbus::orch::installSpendFreeze(nullptr);
      nimbus::orch::installPendingSpend(nullptr, nullptr);
      nimbus::orch::installSeamMeter(nullptr);
      self() = nullptr;
    }
  }

  bool frozen() {
    std::lock_guard<std::mutex> lk(mu_);
    return c_.frozenWith(pending_);
  }

  uint64_t price(const nimbus::orch::TurnSpend& s) const { return ledger_.priceTurnMicro(s); }

  // A turn's (or fold's) whole spend; `turn` ends the turn in flight (its pending
  // rounds are replaced by this committed total).
  void commit(const nimbus::orch::TurnSpend& s, bool turn) {
    const uint64_t micro = price(s);
    std::lock_guard<std::mutex> lk(mu_);
    if (turn) pending_ = 0;
    if (micro && c_.add(micro, monthNow())) save();
  }
  // A paid call outside a turn (never ends the turn in flight).
  void commitMicro(uint64_t micro) {
    std::lock_guard<std::mutex> lk(mu_);
    if (micro && c_.add(micro, monthNow())) save();
  }
  void addPending(uint64_t micro) {
    std::lock_guard<std::mutex> lk(mu_);
    pending_ = nimbus::orch::addSatMicro(pending_, micro);
  }

  // The owner's ceiling; false when the file could not be written (nothing changed).
  bool setLimitMicro(uint64_t micro) {
    std::lock_guard<std::mutex> lk(mu_);
    const nimbus::orch::SpendCeiling before = c_;
    if (!c_.setLimit(micro)) return true;
    if (save()) return true;
    c_ = before;
    return false;
  }

  // The month rollover (the engine thread's idle tick).
  void tick() {
    std::lock_guard<std::mutex> lk(mu_);
    if (c_.roll(monthNow()) || dirty_) save();
  }

  // The notice owed on `ch` (the rules in lib/core) and its text; delivered() marks it.
  using Kind = nimbus::orch::SpendCeiling::Notice;
  Kind nextNotice(nimbus::orch::NoticeChannel ch, std::string& text) {
    std::lock_guard<std::mutex> lk(mu_);
    const Kind k = c_.nextNotice(ch);
    if (k != Kind::None) text = nimbus::orch::noticeText(k, factsLocked());
    return k;
  }
  void delivered(nimbus::orch::NoticeChannel ch, Kind k) {
    std::lock_guard<std::mutex> lk(mu_);
    if (c_.markDelivered(ch, k)) save();
  }

  // {"limitCents":..,"spentCents":..,"frozen":..,"ownerSet":..,"month":..,
  //  "resetsOn":"..","saved":..,"loaded":true} (the device's shape).
  std::string stateJson() {
    std::lock_guard<std::mutex> lk(mu_);
    char buf[256];
    std::snprintf(buf, sizeof(buf),
                  "{\"limitCents\":%llu,\"spentCents\":%llu,\"frozen\":%s,\"ownerSet\":%s,"
                  "\"active\":%s,\"month\":%lu,\"resetsOn\":\"%s\",\"saved\":%s,\"loaded\":true}",
                  (unsigned long long)(c_.limitMicro / 10000ULL),
                  (unsigned long long)(c_.spentMicro / 10000ULL),
                  c_.frozenWith(pending_) ? "true" : "false", c_.ownerSet ? "true" : "false",
                  c_.active() ? "true" : "false",
                  (unsigned long)c_.monthKey, nimbus::orch::resetDateText(c_.monthKey).c_str(),
                  dirty_ ? "false" : "true");
    return buf;
  }
  std::string setupText() {
    std::lock_guard<std::mutex> lk(mu_);
    return nimbus::orch::setupDefaultsText(c_.limitMicro);
  }
  void setHeadModel(std::string m) {
    std::lock_guard<std::mutex> lk(mu_);
    head_ = std::move(m);
  }

 private:
  static SpendCeilingVn*& self() {
    static SpendCeilingVn* s = nullptr;
    return s;
  }
  static uint32_t monthNow() { return nimbus::orch::ceilingMonthKey(time(nullptr)); }

  // Caller holds mu_. No file: a new instance starts at zero for this month; one that
  // already had state (an earlier release ran here) owes its web chat one notice. A
  // file that is there but does not read back (unreadable, torn, foreign) is the one
  // case with no count to trust and no ledger to recount from: the instance starts
  // paused, at the default ceiling over a count at that ceiling (one freeze notice
  // says how to raise it), never at zero.
  void load(bool hadState) {
    struct stat st;
    const bool exists = ::stat(path_.c_str(), &st) == 0;
    std::string text;
    bool read = false;
    for (int attempt = 0; exists && !read && attempt < 3; ++attempt)   // a transient read error
      read = fsutil::readFile(path_, text);
    if (read && wholeLine(text) && c_.deserialize(text)) {
      c_.roll(monthNow());
      return;
    }
    if (exists) {
      c_ = nimbus::orch::SpendCeiling();
      c_.monthKey = monthNow();
      c_.add(c_.limitMicro, c_.monthKey);
      std::fprintf(stderr, "[nimbusd] spend: %s did not read back - paid calls paused until the ceiling is raised\n",
                   path_.c_str());
      save();
      return;
    }
    c_ = nimbus::orch::SpendCeiling::firstBoot(monthNow(), hadState, 0, telegram_);
    save();
  }

  // Every field the instance has ever written (nine, the last one present): a file cut
  // short (the old unsynced save on a full volume) parses with its tail read as 0,
  // which would un-freeze a frozen instance, so it does not count as read back.
  static bool wholeLine(const std::string& text) {
    const size_t semi = text.find(';');
    if (semi == std::string::npos) return false;
    size_t bars = 0;
    for (size_t i = semi + 1; i < text.size(); ++i) bars += text[i] == '|';
    return bars >= 8 && text.back() != '|';
  }

  // Caller holds mu_. Whole-file write: a temporary file, fsync'd, renamed over the
  // state only once every byte landed (a full volume leaves the previous good file).
  bool save() {
    dirty_ = !fsutil::writeFileAtomic(path_, c_.serialize());
    return !dirty_;
  }

  nimbus::orch::CeilingFacts factsLocked() const {
    nimbus::orch::CeilingFacts f;
    f.limitMicro = c_.limitMicro;
    f.spentMicro = c_.spentMicro;
    f.monthKey = c_.monthKey;
    const time_t now = time(nullptr);
    struct tm tmv;
    if (now >= nimbus::orch::kCeilingClockValid && localtime_r(&now, &tmv)) f.dayOfMonth = tmv.tm_mday;
    f.headModel = head_;
    return f;
  }

  std::string path_;
  bool telegram_ = false;
  std::mutex mu_;
  nimbus::orch::SpendCeiling c_;
  nimbus::orch::UsageLedger ledger_;   // built-in prices only (read-only after construction)
  uint64_t pending_ = 0;
  bool dirty_ = false;
  std::string head_;
};

}  // namespace nimbusd
