#include "nimbus/harness/fold_driver.h"

#include <ArduinoJson.h>

#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

#include "nimbus/harness/engine.h"
#include "nimbus/harness/log.h"
#include "nimbus/orch/loop_delivery.h" // deliveryChannelOf - where the daily alert can go
#include "nimbus/orch/loops.h"         // civilFromDays - the owner's local dates
#include "nimbus/orch/wire_limits.h"   // foldDigestCap - the slice beside the summary

// fold_driver - see fold_driver.h. One fold cycle at a time, one rung per pass.
namespace agent {

namespace orch = nimbus::orch;

namespace {
constexpr size_t kDefaultFoldStep = 65536;
constexpr uint32_t kDefaultTriggerBytes = 48u * 1024u;

// "YYYY-MM-DD" of `epoch` in the owner's zone ("" with no clock).
std::string localDate(uint32_t epoch, int32_t offMin) {
  if (epoch == 0) return std::string();
  const orch::CivilTime c = orch::civilFromDays(orch::foldLocalDay(epoch, offMin));
  char buf[16];
  std::snprintf(buf, sizeof(buf), "%04d-%02d-%02d", c.year, c.month, c.day);
  return buf;
}

// "HH:MM" of `epoch` in the owner's zone.
std::string localClock(uint32_t epoch, int32_t offMin) {
  const int64_t local = (int64_t)epoch + (int64_t)offMin * 60;
  const int64_t m = ((local / 60) % 1440 + 1440) % 1440;
  char buf[8];
  std::snprintf(buf, sizeof(buf), "%02d:%02d", (int)(m / 60), (int)(m % 60));
  return buf;
}

// "15 minutes", "1 hour", "4 hours".
std::string waitWords(uint32_t waitS) {
  if (waitS < 3600) {
    const uint32_t m = (waitS + 59) / 60;
    return std::to_string(m) + (m == 1 ? " minute" : " minutes");
  }
  const uint32_t h = (waitS + 1799) / 3600;
  return std::to_string(h) + (h == 1 ? " hour" : " hours");
}

std::string rungKey(const std::string& host, const std::string& model) { return host + "/" + model; }

const char* stateOf(const FoldStatus& s, uint32_t nowEpoch) {
  const bool uncovered = !s.uncoveredTo.empty();
  if (s.lastOk == 0) return uncovered ? "behind" : "none";
  const uint32_t age = nowEpoch > s.lastOk ? nowEpoch - s.lastOk : 0;
  if (uncovered || age > 72u * 3600u) return "behind";
  return age >= 24u * 3600u ? "stale" : "fresh";
}

// Whether Summarize now can start for this chat, and why not (the button's hint).
const char* blockedWhy(const FoldStatus& s) {
  if (s.running && s.manual) return "Summarizing now.";
  if (s.queued) return "Queued. It starts in a moment.";
  if (s.manualBusy) return "Another summary is running.";
  if (s.cooldownS) return "Ready again in a moment.";
  return "";
}
}  // namespace

const char* foldTickName(FoldTick t) {
  switch (t) {
    case FoldTick::Idle: return "idle";
    case FoldTick::Deferred: return "deferred";
    case FoldTick::Skipped: return "skipped";
    case FoldTick::Pending: return "pending";
    case FoldTick::Ok: return "ok";
    case FoldTick::CycleFailed: return "cycle failed";
  }
  return "unknown";
}

std::string foldStatusJson(const FoldStatus& s, uint32_t nowEpoch, int32_t utcOffsetMin,
                           const std::vector<FoldStatus>* chats) {
  JsonDocument d;
  d["chat"] = s.chat;
  const bool aged = s.lastOk != 0 && nowEpoch != 0;
  d["summaryAgeH"] = aged ? (double)((nowEpoch > s.lastOk ? nowEpoch - s.lastOk : 0) / 360) / 10.0 : -1.0;
  d["lastOk"] = s.lastOk;
  d["lastOkDate"] = localDate(s.lastOk, utcOffsetMin);
  d["summaryBytes"] = s.summaryBytes;
  d["thru"] = s.thru;
  d["thruDate"] = s.thruDate;
  d["uncovered"] = !s.uncoveredTo.empty();
  d["uncoveredFrom"] = s.uncoveredTo.empty() ? std::string() : s.thru;
  d["uncoveredTo"] = s.uncoveredTo;
  d["failedCycles"] = s.failedCycles;
  d["firstFail"] = s.firstFail;
  d["nextTry"] = s.nextTry;
  d["nextTryInS"] = s.nextTry > nowEpoch && nowEpoch ? s.nextTry - nowEpoch : 0;
  d["paidToday"] = s.paidToday;
  d["cap"] = s.cap;
  d["running"] = s.running;
  d["manual"] = s.manual;
  d["manualBusy"] = s.manualBusy;
  d["rung"] = s.rungIndex ? std::to_string(s.rungIndex) + "/" + std::to_string(s.rungCount) : std::string();
  d["rungHost"] = s.rungHost;
  d["rungModel"] = s.rungModel;
  d["queued"] = s.queued;
  d["cooldownS"] = s.cooldownS;
  d["state"] = stateOf(s, nowEpoch);
  const char* why = blockedWhy(s);
  d["canRun"] = *why == 0;
  d["why"] = why;
  if (chats) {
    JsonArray list = d["chats"].to<JsonArray>();
    for (const FoldStatus& c : *chats) {
      JsonObject o = list.add<JsonObject>();
      o["chat"] = c.chat;
      o["state"] = stateOf(c, nowEpoch);
    }
  }
  std::string out;
  serializeJson(d, out);
  return out;
}

FoldDriver::FoldDriver(TurnEngine& eng, orch::FoldStore& folds, orch::FoldCtlIO ctlIO, FoldDriverHooks hooks)
    : eng_(&eng), folds_(folds), ctlIO_(std::move(ctlIO)), hooks_(std::move(hooks)) {}

void FoldDriver::begin() {
  ctl_ = orch::FoldCtl::decode(ctlIO_.load ? ctlIO_.load() : std::string());
  if (ctl_.fwAtLastFail.empty() || ctl_.fwAtLastFail == hooks_.firmware) return;
  // A new firmware may have fixed what failed: every chat's wait restarts, so a chat
  // with a reason to fold folds on the next pass instead of sitting out a 12 h wait.
  hlog::logf("fold: firmware %s follows %s - every chat's wait restarts", hooks_.firmware.c_str(),
             ctl_.fwAtLastFail.c_str());
  folds_.resetWaits();
  ctl_.fwAtLastFail.clear();
  saveCtl();
}

void FoldDriver::saveCtl() {
  if (ctlIO_.save && !ctlIO_.save(ctl_.encode())) hlog::log("fold: the fold control file was not saved");
}

void FoldDriver::changed(const std::string& chat) const {
  if (hooks_.changed) hooks_.changed(chat);
}

int32_t FoldDriver::offsetAt(uint32_t epoch) const { return hooks_.utcOffsetMin ? hooks_.utcOffsetMin(epoch) : 0; }

uint32_t FoldDriver::today(uint32_t nowEpoch) const { return orch::foldLocalDay(nowEpoch, offsetAt(nowEpoch)); }

void FoldDriver::resetDailyCount() {
  ctl_.paidCount = 0;
  ctl_.guestPaid = 0;
  clocklessPaid_ = clocklessGuest_ = 0;
  saveCtl();
  changed();
}

// The network, then no turn in flight, the heap floor and a rung to ask: nothing visible
// or paid happens before these pass (prism v3.6.0 HIGH), and a cycle keeps its place meanwhile.
bool FoldDriver::gatesOpen() const {
  if (hooks_.online && !hooks_.online()) return false;
  return eng_->canFoldNow();
}

// A later automatic rung waits for the owner again (an overflow fold does not wait).
bool FoldDriver::automaticRungWaits() const {
  if (cycle_.manual || cycle_.overflow || !hooks_.quiet) return false;
  return !hooks_.quiet(false);
}

FoldTick FoldDriver::stopAtCap() {
  hlog::logf("fold(%s): the cycle stops at rung %u/%u for the daily cap, uncounted", cycle_.chat.c_str(),
             (unsigned)(cycle_.idx + 1), (unsigned)cycle_.rungs.size());
  cycle_ = Cycle();
  changed();
  return FoldTick::Deferred;
}

FoldTick FoldDriver::tick(uint32_t nowMs, uint32_t nowEpoch) {
  if (!ticked_) {
    ticked_ = true;
    firstTickMs_ = nowMs;
  }
  lastTickMs_ = nowMs;
  if (clocklessPaid_ && nowMs - clocklessSinceMs_ >= 24u * 3600u * 1000u) {   // a clockless day ends
    hlog::log("fold: 24 hours with no clock since the first paid request - the daily count restarts");
    clocklessPaid_ = clocklessGuest_ = 0;
  }
  if (!gatesOpen()) {
    if (!cycleActive()) noteGatesClosed(nowMs);
    return cycleActive() || !ctl_.manualPendingChat.empty() ? FoldTick::Deferred : FoldTick::Idle;
  }
  std::string ready;
  if (pickManual(nowMs, ready)) endAutomaticForManual();
  if (!cycleActive()) {
    const FoldTick started = startCycle(nowMs, nowEpoch);
    if (!cycleActive()) return started;
  } else if (automaticRungWaits()) {
    return FoldTick::Deferred;
  }
  if (!cycle_.manual && capReached(nowEpoch, cycle_.chat)) return stopAtCap();
  return runRung(nowMs, nowEpoch);
}

void FoldDriver::notePaid(uint32_t nowEpoch, uint32_t n, const std::string& chat) {
  const bool guest = !isOwnerChat(chat);
  if (nowEpoch == 0) {   // no clock: in RAM, the day counted from its first request
    if (!clocklessPaid_) clocklessSinceMs_ = lastTickMs_;
    clocklessPaid_ += n;
    if (guest) clocklessGuest_ += n;
    return;
  }
  ctl_.notePaid(today(nowEpoch), n, guest);
  saveCtl();
}

uint32_t FoldDriver::paidToday(uint32_t nowEpoch) const {
  return nowEpoch == 0 ? clocklessPaid_ : ctl_.paidOn(today(nowEpoch));
}

uint32_t FoldDriver::guestPaidToday(uint32_t nowEpoch) const {
  return nowEpoch == 0 ? clocklessGuest_ : ctl_.guestPaidOn(today(nowEpoch));
}

bool FoldDriver::capReached(uint32_t nowEpoch, const std::string& chat) {
  const uint32_t paid = paidToday(nowEpoch);
  if (!chat.empty() && !isOwnerChat(chat) && guestPaidToday(nowEpoch) >= orch::kFoldGuestPaidPerDay) return true;
  if (paid < orch::kFoldPaidPerDay) return false;
  const uint32_t day = today(nowEpoch);
  if (capLoggedDay_ != day) {
    capLoggedDay_ = day;
    hlog::logf("fold: %u paid fold requests today, the daily cap - automatic folds wait for the next local day",
               (unsigned)paid);
  }
  return true;
}

bool FoldDriver::pickManual(uint32_t nowMs, std::string& chat) const {
  if (ctl_.manualPendingChat.empty()) return false;
  if (manualEnded_ && nowMs - lastManualEndMs_ < kManualCooldownMs) return false;   // one at a time, 2 min apart
  // Its read waits (fix review, candidate 2): between two passes of a manual read that goes
  // on, the other chats' automatic folds get their turn.
  if (manualWaitPasses_ && (int32_t)(manualPassUntilMs_ - nowMs) > 0) return false;
  chat = ctl_.manualPendingChat.front();
  return true;
}

// The chat has a reason to fold (over the trigger, or rows a shift hid), waits aside.
bool FoldDriver::wantsFold(const std::string& chat) const {
  const orch::ChatFold* f = folds_.peek(chat);
  if (!f) return false;
  const uint32_t trigger = hooks_.triggerBytes ? hooks_.triggerBytes() : kDefaultTriggerBytes;
  const bool hidden = !f->softStart.empty() && (f->thru.empty() || rowIdAfter(f->softStart, f->thru));
  return hidden || (trigger && orch::bytesAfterThru(*f) >= trigger) || f->msgsSinceFold >= orch::kFoldMsgsThreshold;
}

// A chat with a reason to fold that this pass does not start, and why (bench B11, candidate 2:
// the bench saw silence). At most one line per chat and cause per hour (candidate 4, F1): the
// line is durable, and one key in total let two held chats write a line each on every pass.
void FoldDriver::noteNotStarted(const std::string& chat, const char* why, uint32_t nowMs) {
  const std::string key = chat + "|" + why;
  NotStartedLine* slot = &notStarted_[0];
  for (NotStartedLine& l : notStarted_) {
    if (l.key == key) {
      if (nowMs - l.ms < kNotStartedEveryMs) return;
      slot = &l;
      break;
    }
    if (!slot->key.empty() && (l.key.empty() || l.ms - slot->ms > 0x80000000u)) slot = &l;   // empty, else oldest
  }
  slot->key = key;
  slot->ms = nowMs;
  hlog::logf("fold(%s): due, not started - %s", chat.c_str(), why);
}

void FoldDriver::noteGatesClosed(uint32_t nowMs) {
  for (const std::string& c : folds_.chatIds()) {
    if (c == "system" || !wantsFold(c)) continue;
    const bool offline = hooks_.online && !hooks_.online();
    noteNotStarted(c, offline ? "the network is down" : "a turn in flight, the heap floor or no provider to ask", nowMs);
    return;
  }
}

bool FoldDriver::dueNow(const std::string& chat, uint32_t nowMs, uint32_t nowEpoch) {
  const char* why = waitOf(chat, nowMs, nowEpoch);
  if (why) {
    if (wantsFold(chat)) noteNotStarted(chat, why, nowMs);
    return false;
  }
  bool thrash = false;
  const uint32_t trigger = hooks_.triggerBytes ? hooks_.triggerBytes() : kDefaultTriggerBytes;
  if (folds_.evaluateDue(chat, trigger, orch::kFoldMsgsThreshold, nowEpoch, &thrash) != orch::FoldDue::Yes) {
    if (wantsFold(chat)) noteNotStarted(chat, "its retry wait (next try scheduled)", nowMs);
    return false;
  }
  if (thrash && nowEpoch == 0) ramWaitMs_[chat] = nowMs + orch::kFoldThrashRetryS * 1000u;
  return true;
}

// The wait that holds the chat this pass (nullptr = none).
const char* FoldDriver::waitOf(const std::string& chat, uint32_t nowMs, uint32_t nowEpoch) const {
  const auto w = ramWaitMs_.find(chat);
  if (w != ramWaitMs_.end() && (int32_t)(w->second - nowMs) > 0) return "its wait (no clock)";
  const auto r = readWaitUntilMs_.find(chat);
  if (r != readWaitUntilMs_.end() && (int32_t)(r->second - nowMs) > 0) return "its rows' read wait";
  // No clock yet: a chat that carries a persisted wait is held a few minutes for the clock,
  // not retried at once on every boot (its wait cannot be read without one).
  if (nowEpoch == 0 && nowMs - firstTickMs_ < FoldDriverHooks::kNoClockGraceMs) {
    const orch::ChatFold* f = folds_.peek(chat);
    if (f && f->nextTry) return "a persisted wait, no clock yet";
  }
  return nullptr;
}

bool FoldDriver::pickFrom(uint32_t nowMs, uint32_t nowEpoch, bool owners, std::string& chat) {
  if (!lastTurnChat_.empty() && isOwnerChat(lastTurnChat_) == owners && dueNow(lastTurnChat_, nowMs, nowEpoch)) {
    chat = lastTurnChat_;
    return true;
  }
  for (const std::string& c : folds_.chatIds()) {
    if (c == lastTurnChat_ || c == "system" || isOwnerChat(c) != owners) continue;
    if (!owners && guestPaidToday(nowEpoch) >= orch::kFoldGuestPaidPerDay) {   // their share is spent
      if (wantsFold(c)) noteNotStarted(c, "the daily cap's share for chats that are not the owner's", nowMs);
      continue;
    }
    if (!dueNow(c, nowMs, nowEpoch)) continue;
    chat = c;
    return true;
  }
  return false;
}

bool FoldDriver::pickAutomatic(uint32_t nowMs, uint32_t nowEpoch, std::string& chat) {
  // The owner's chats first (the chat the last turn ran in, then the others, least recently
  // used first), then everyone else's: a chat left due before a restart (the legacy pause
  // Lumi held) folds on the first pass, before anyone writes in it.
  return pickFrom(nowMs, nowEpoch, true, chat) || pickFrom(nowMs, nowEpoch, false, chat);
}

std::vector<FoldRung> FoldDriver::orderedRungs() const {
  std::vector<FoldRung> rungs = eng_->foldRungs();
  // A (host, model) that answered empty or thin goes last (stable among the rest).
  std::stable_partition(rungs.begin(), rungs.end(),
                        [this](const FoldRung& r) { return !degraded_.count(rungKey(r.host, r.model)); });
  return rungs;
}

// The chat a new cycle takes: the next manual request (one at a time, 2 min apart),
// else the first due chat whose automatic fold the quiet gate and the daily cap allow.
FoldTick FoldDriver::pickChat(uint32_t nowMs, uint32_t nowEpoch, std::string& chat, bool& manual, bool& overflow) {
  manual = pickManual(nowMs, chat);
  overflow = false;
  if (manual) return FoldTick::Pending;
  if (!pickAutomatic(nowMs, nowEpoch, chat)) {
    if (hooks_.notDue) hooks_.notDue();
    return ctl_.manualPendingChat.empty() ? FoldTick::Idle : FoldTick::Deferred;
  }
  overflow = folds_.overflowQueued(chat);
  // A due automatic fold waits for the owner (owner_first.h); an overflow fold is
  // needed by the next turn. Then the device-wide daily cap.
  if (hooks_.quiet && !hooks_.quiet(overflow)) {
    noteNotStarted(chat, "the quiet gate (waits for a quiet moment)", nowMs);
    return FoldTick::Deferred;
  }
  if (!capReached(nowEpoch, chat)) return FoldTick::Pending;
  noteNotStarted(chat, "the daily cap", nowMs);
  return FoldTick::Deferred;
}

// A due fold whose slice read to its bound and found nothing: a manual request is answered
// (what an unfinished catch-up covered, else "nothing yet"), an automatic one restarts its
// counters (and gives up hidden rows only up to where the read reached).
FoldTick FoldDriver::nothingToFold(const std::string& chat, bool manual, uint32_t nowMs, const FoldSlice& s) {
  clearReadWait(chat);
  if (manual) {
    const bool caughtUp = catchUp_.chat == chat && catchUp_.span.rows;
    if (hooks_.tell) hooks_.tell(chat, caughtUp ? confirmation(catchUp_.span) : "Nothing to compact yet.");
    catchUp_ = CatchUp();
    endManual(chat, nowMs);
  } else {
    // Never restart the count at or over the trigger: a kept tail that large (one huge turn)
    // would make the chat due again on every pass with nothing to fold.
    const uint32_t trigger = hooks_.triggerBytes ? hooks_.triggerBytes() : kDefaultTriggerBytes;
    folds_.noteFoldSkipped(chat, s.readTo, std::min(s.keptBytes, trigger - trigger / 4));
    hlog::logf("fold(%s): nothing new beyond the kept history - skipped", chat.c_str());
  }
  changed(chat);
  return FoldTick::Skipped;
}

uint32_t FoldDriver::readWaitMs(uint32_t waits) {
  if (waits == 0) return 0;
  const uint32_t ms = kReadWaitFirstMs << std::min<uint32_t>(waits - 1, 6);
  return std::min(ms, kReadWaitMaxMs);
}

// A due fold whose rows cannot be read yet (SliceState Wait or Unread): nothing in the store
// changes, the chat's anchor and counters stay, and the hidden rows stay named. A read that
// moved forward (a scan, a read budget it resumes from) goes on next pass; otherwise the chat
// is tried again later, 1 minute out and doubling to 1 hour, so it never stops folding for
// good and never reads the card every pass. A manual request is answered: a catch-up says
// what it covered, else the owner is told the rows could not be read.
// Rows the store keeps refusing (Unread) stay unread, fail-closed: the anchor never passes them.
// After a day of it the owner hears once a local day, through the alert (fix review E).
// A read got past the chat's rows (Ready, Nothing, a landed fold) or the chat was cleared: its
// read waits end, and so does its unread clock (persisted, fold_ctl.txt).
void FoldDriver::clearReadWait(const std::string& chat) {
  readWaitCount_.erase(chat);
  readWaitUntilMs_.erase(chat);
  if (ctl_.dropUnreadSince(chat)) saveCtl();
}

// The unread clock survives a Wait pass and a restart (fix review, candidate 2): only a read
// that gets past the rows ends it. A clock that stepped back before it restarts it.
void FoldDriver::noteUnread(const std::string& chat, uint32_t nowEpoch, bool unread) {
  if (!unread || !nowEpoch) return;   // no clock: no day to count, no alert (as for failed cycles)
  uint32_t since = ctl_.unreadSinceOf(chat);
  if (!since || nowEpoch < since) {
    since = nowEpoch;
    if (ctl_.setUnreadSince(chat, since)) saveCtl();
  }
  if (nowEpoch - since < orch::kFoldAlertAfterS || ctl_.lastAlertDay == today(nowEpoch)) return;
  const std::string text = unreadNotice();
  bool told = false;
  if (hooks_.alert) told = hooks_.alert(chat, text);
  else if (hooks_.tell) {
    hooks_.tell(chat, text);
    told = true;
  }
  if (!told) return;
  ctl_.lastAlertDay = today(nowEpoch);
  saveCtl();
}

namespace {
// What a manual request that cannot read its rows is told: the truth for each case (rows the
// store refuses are not a matter of minutes).
const char* manualWaitAnswer(SliceState state, bool tooLong) {
  if (state == SliceState::Unread)
    return "\xE2\x9A\xA0 Some older messages in this chat can't be read from storage right now, so the summary "
           "can't include them yet. It keeps trying by itself; if this lasts, restart the device.";
  if (tooLong)
    return "\xE2\x9A\xA0 Reading this chat's older messages is taking too long right now, so the summary isn't "
           "finished. Try again later.";
  return "\xE2\x9A\xA0 Couldn't read this chat's older messages just now. Try again in a few minutes.";
}
}  // namespace

// The answer and the end of a manual request whose read stopped.
void FoldDriver::endManualWait(const std::string& chat, uint32_t nowMs, SliceState state, bool tooLong) {
  const bool caughtUp = catchUp_.chat == chat && catchUp_.span.rows;
  if (hooks_.tell) hooks_.tell(chat, caughtUp ? confirmation(catchUp_.span) : manualWaitAnswer(state, tooLong));
  catchUp_ = CatchUp();
  endManual(chat, nowMs);
}

FoldTick FoldDriver::sliceWaits(const std::string& chat, bool manual, uint32_t nowMs, uint32_t nowEpoch,
                                const FoldSlice& s) {
  noteUnread(chat, nowEpoch, s.state == SliceState::Unread);
  uint32_t& waits = readWaitCount_[chat];
  if (!s.progressed) waits++;
  // A manual read that goes on yields one pass to the automatic folds, and is answered after
  // kManualWaitPasses of them instead of being left pending for good.
  const bool manualGoesOn = manual && s.progressed && s.state != SliceState::Unread;
  if (manualGoesOn) {
    manualWaitPasses_++;
    manualPassUntilMs_ = nowMs + kManualPassMs;
  }
  const uint32_t waitMs = manualGoesOn ? kManualPassMs : readWaitMs(s.progressed ? 0 : waits);
  readWaitUntilMs_[chat] = nowMs + waitMs;
  hlog::logf("fold(%s): the slice %s (%s), the anchor stays - next look in %u s", chat.c_str(),
             s.state == SliceState::Unread ? "meets rows the card will not give back" : "waits for its rows",
             s.progressed ? "progress" : std::to_string((unsigned)waits).c_str(), (unsigned)(waitMs / 1000));
  const bool tooLong = manualGoesOn && manualWaitPasses_ >= kManualWaitPasses;
  if (manual && (!manualGoesOn || tooLong)) endManualWait(chat, nowMs, s.state, tooLong);
  changed(chat);
  return FoldTick::Deferred;
}

// An automatic cycle starting between two passes of a manual read that goes on.
bool FoldDriver::automaticInYield(bool manual) const {
  return !manual && manualWaitPasses_ && !ctl_.manualPendingChat.empty();
}

FoldTick FoldDriver::startCycle(uint32_t nowMs, uint32_t nowEpoch) {
  std::string chat;
  bool manual = false, overflow = false;
  const FoldTick picked = pickChat(nowMs, nowEpoch, chat, manual, overflow);
  if (picked != FoldTick::Pending) return picked;
  const orch::ChatFold* f = folds_.peek(chat);
  Cycle c;
  c.prevSummary = f ? f->summary : std::string();
  const size_t step = hooks_.foldCycleBytes ? hooks_.foldCycleBytes() : kDefaultFoldStep;
  const size_t cap =
      std::min(orch::foldDigestAfterFailures(step, f ? f->failedCycles : 0), orch::foldDigestCap(c.prevSummary.size()));
  c.slice = hooks_.slice ? hooks_.slice(chat, cap, manual) : eng_->historyFoldSlice(chat, cap, manual);
  if (c.slice.digest.empty()) {
    if (c.slice.state == SliceState::Wait || c.slice.state == SliceState::Unread)
      return sliceWaits(chat, manual, nowMs, nowEpoch, c.slice);
    return nothingToFold(chat, manual, nowMs, c.slice);
  }
  clearReadWait(chat);
  c.chat = chat;
  c.manual = manual;
  c.overflow = overflow;
  c.bytesAtStart = f ? f->bytesSinceFold : 0;
  c.msgsAtStart = f ? f->msgsSinceFold : 0;
  c.rungs = orderedRungs();
  c.startedMs = nowMs;
  c.inYield = automaticInYield(manual);
  cycle_ = std::move(c);
  hlog::logf("fold(%s): cycle start (%s, %u rungs, %u B digest, %u rows)", chat.c_str(),
             manual ? "manual" : "automatic", (unsigned)cycle_.rungs.size(), (unsigned)cycle_.slice.digest.size(),
             (unsigned)cycle_.slice.rows);
  // The slice's range (bench B11 checks that each fold starts where the last one ended).
  hlog::logf("fold(%s): slice of the rows after %s through %s (first %s)", chat.c_str(),
             f && !f->thru.empty() ? f->thru.c_str() : "the chat's start", cycle_.slice.thru.c_str(),
             cycle_.slice.firstId.empty() ? "-" : cycle_.slice.firstId.c_str());
  changed(chat);
  return FoldTick::Pending;
}

FoldTick FoldDriver::runRung(uint32_t nowMs, uint32_t nowEpoch) {
  const size_t n = cycle_.rungs.size();
  if (cycle_.idx >= n) return cycleFailed(nowMs, nowEpoch);
  const FoldRung rung = cycle_.rungs[cycle_.idx];
  // A rung whose host left the ladder since the cycle froze it (parked, capped, its key
  // removed) is passed over, not asked.
  const std::vector<FoldRung> now = eng_->foldRungs();
  if (std::none_of(now.begin(), now.end(), [&](const FoldRung& r) { return r.host == rung.host; })) {
    hlog::logf("fold(%s): rung %u/%u %s/%s passed over - not usable now", cycle_.chat.c_str(),
               (unsigned)(cycle_.idx + 1), (unsigned)n, rung.host.c_str(), rung.model.c_str());
    return ++cycle_.idx < n ? FoldTick::Pending : cycleFailed(nowMs, nowEpoch);
  }
  FoldRungReport rep;
  std::string summary;
  if (hooks_.asking) hooks_.asking(true);
  const FoldResult r = eng_->runFoldRung(cycle_.chat, cycle_.prevSummary, cycle_.slice.digest, rung, summary, &rep);
  if (hooks_.asking) hooks_.asking(false);
  if (rep.paidRequests) notePaid(nowEpoch, rep.paidRequests, cycle_.chat);
  if (r == FoldResult::Deferred) return FoldTick::Deferred;   // a gate closed: the rung waits
  if (r == FoldResult::Ok) return landed(rep, summary, nowMs, nowEpoch);
  hlog::log(foldRungFailLine(cycle_.chat, cycle_.idx, n, rep).c_str());
  // A rung that answered empty or thin, or was cut twice (a model that thinks past its cap),
  // goes last next cycle: the ladder does not pay for it first again.
  if (rep.degraded || rep.fail == FoldFailKind::Cut) degraded_.insert(rungKey(rep.host, rep.model));
  if (rep.thinSummary.size() > cycle_.bestThin.thinSummary.size()) cycle_.bestThin = rep;
  if (++cycle_.idx < n) {
    changed(cycle_.chat);
    return FoldTick::Pending;
  }
  return cycleFailed(nowMs, nowEpoch);
}

FoldTick FoldDriver::landed(const FoldRungReport& rep, const std::string& summary, uint32_t nowMs,
                            uint32_t nowEpoch) {
  const Cycle c = cycle_;
  cycle_ = Cycle();
  folds_.applyFold(c.chat, summary, nowEpoch, c.slice.thru, orch::kChatSummaryMax, c.slice.keptBytes);   // the summary FIRST
  degraded_.erase(rungKey(rep.host, rep.model));
  ramWaitMs_.erase(c.chat);
  if (!c.slice.lastDate.empty()) thruDate_[c.chat] = c.slice.lastDate;
  FoldDone done;
  done.chat = c.chat;
  done.manual = c.manual;
  done.summary = summary;
  done.slice = c.slice;
  done.producedBytes = rep.producedBytes;
  done.bytesFolded = c.bytesAtStart;
  done.msgsFolded = c.msgsAtStart;
  done.host = rep.host;
  done.model = rep.model;
  done.elapsedMs = nowMs - c.startedMs;
  if (hooks_.folded) hooks_.folded(done);   // then the chain reset and the boundary row
  if (c.manual && !catchUpContinues(c.chat, c.slice)) {
    if (hooks_.tell) hooks_.tell(c.chat, confirmation(catchUp_.span));
    catchUp_ = CatchUp();
    endManual(c.chat, nowMs);
  }
  changed(c.chat);
  return FoldTick::Ok;
}

bool FoldDriver::catchUpContinues(const std::string& chat, const FoldSlice& landed) {
  if (catchUp_.chat != chat) catchUp_ = CatchUp{chat, FoldSlice(), 0};
  FoldSlice& span = catchUp_.span;
  span.rows += landed.rows;
  if (span.firstDate.empty()) span.firstDate = landed.firstDate;
  if (!landed.lastDate.empty()) span.lastDate = landed.lastDate;
  span.thru = landed.thru;
  // More to catch up: rows the history still hides behind the anchor, or a read that stopped
  // short of the newest row (lane CTXR1: one press summarizes everything, so a rollback to
  // v5.1.1 skips nothing). The request stays queued (a restart resumes it), the next cycle
  // starts on a later pass.
  const orch::ChatFold* f = folds_.peek(chat);
  const bool hidden = f && !f->softStart.empty() && (f->thru.empty() || rowIdAfter(f->softStart, f->thru));
  return (hidden || landed.more) && ++catchUp_.cycles < kCatchUpCycles;
}

FoldTick FoldDriver::cycleFailed(uint32_t nowMs, uint32_t nowEpoch) {
  if (!cycle_.bestThin.thinSummary.empty()) {
    // Every rung answered, none with the sections the gate looks for: a summary in
    // another language or with renamed headings is better than none, and a backoff that
    // never ends. The longest is kept.
    hlog::logf("fold(%s): every rung answered without the expected sections - keeping the longest (%s/%s)",
               cycle_.chat.c_str(), cycle_.bestThin.host.c_str(), cycle_.bestThin.model.c_str());
    const FoldRungReport best = cycle_.bestThin;
    return landed(best, best.thinSummary, nowMs, nowEpoch);
  }
  const Cycle c = cycle_;
  cycle_ = Cycle();
  const uint32_t next = folds_.noteCycleFailed(c.chat, nowEpoch);
  const orch::ChatFold* f = folds_.peek(c.chat);
  const uint32_t waitS = orch::foldBackoffS(f ? f->failedCycles : 1);
  if (nowEpoch == 0) ramWaitMs_[c.chat] = nowMs + waitS * 1000u;   // no clock: the wait lives in RAM
  ctl_.fwAtLastFail = hooks_.firmware;
  saveCtl();
  const int32_t off = offsetAt(nowEpoch);
  const std::string when = next ? localDate(next, off) + " " + localClock(next, off) : "in " + waitWords(waitS);
  hlog::logf("fold(%s): cycle failed, next try %s (%u failed cycles in a row)", c.chat.c_str(), when.c_str(),
             (unsigned)(f ? f->failedCycles : 1));
  if (c.manual) {
    if (hooks_.tell) hooks_.tell(c.chat, failureNotice(next, waitS, off));
    catchUp_ = CatchUp();
    endManual(c.chat, nowMs);
  } else {
    maybeAlert(c.chat, f, nowEpoch, off);
  }
  changed(c.chat);
  return FoldTick::CycleFailed;
}

// One owner alert a local day on the whole device, and only after a day of failing (a
// clock that stepped back before firstFail never alerts).
void FoldDriver::maybeAlert(const std::string& chat, const orch::ChatFold* f, uint32_t nowEpoch, int32_t off) {
  if (!f || !nowEpoch || !f->firstFail || nowEpoch <= f->firstFail) return;
  if (nowEpoch - f->firstFail < orch::kFoldAlertAfterS || ctl_.lastAlertDay == today(nowEpoch)) return;
  const std::string text = alertNotice(f->lastFoldEpoch ? f->lastFoldEpoch : f->firstFail, off);
  bool told = false;
  if (hooks_.alert) told = hooks_.alert(chat, text);
  else if (hooks_.tell) {
    hooks_.tell(chat, text);
    told = true;
  }
  if (!told) return;   // only logged (not the owner's chat): the day's alert is still the owner's
  ctl_.lastAlertDay = today(nowEpoch);
  saveCtl();
}

void FoldDriver::endManual(const std::string& chat, uint32_t nowMs) {
  manualWaitPasses_ = 0;
  ctl_.dropManual(chat);
  saveCtl();
  lastManualEndMs_ = nowMs;
  manualEnded_ = true;
}

FoldDriver::Manual FoldDriver::requestManual(const std::string& chat) {
  if (cycle_.chat == chat && cycle_.manual) return Manual::AlreadyRunning;
  if (!ctl_.queueManual(chat)) return Manual::Full;   // before anything is ended for it
  saveCtl();
  if (cycle_.chat == chat) {
    // The owner asked for this chat while its automatic cycle runs: that cycle ends
    // uncounted, its failed rungs go last, and a manual cycle with a fresh keep-0
    // slice starts on a later pass.
    for (size_t i = 0; i < cycle_.idx && i < cycle_.rungs.size(); i++)
      degraded_.insert(rungKey(cycle_.rungs[i].host, cycle_.rungs[i].model));
    hlog::logf("fold(%s): a manual request ends the automatic cycle at rung %u/%u, uncounted", chat.c_str(),
               (unsigned)(cycle_.idx + 1), (unsigned)cycle_.rungs.size());
    cycle_ = Cycle();
  }
  changed(chat);
  return Manual::Queued;
}

// A manual request waits for no automatic cycle (fold_driver.h): one that is between
// rungs when a request is ready to start ends uncounted, its failed rungs going last. One
// that started in a manual read's yield is the read's own gift of time: it finishes its
// ladder (lands or fails as any cycle does) and the manual starts on the pass after
// (candidate 5, G2), so no paid answer is thrown away and none lands early.
void FoldDriver::endAutomaticForManual() {
  if (!cycleActive() || cycle_.manual || cycle_.inYield || ctl_.manualPendingChat.empty()) return;
  for (size_t i = 0; i < cycle_.idx && i < cycle_.rungs.size(); i++)
    degraded_.insert(rungKey(cycle_.rungs[i].host, cycle_.rungs[i].model));
  hlog::logf("fold(%s): the automatic cycle yields to a manual request at rung %u/%u, uncounted",
             cycle_.chat.c_str(), (unsigned)(cycle_.idx + 1), (unsigned)cycle_.rungs.size());
  cycle_ = Cycle();
}

void FoldDriver::noteCleared(const std::string& chat) {
  if (cycle_.chat == chat) {
    hlog::logf("fold(%s): /clear ends the cycle at rung %u/%u, uncounted", chat.c_str(), (unsigned)(cycle_.idx + 1),
               (unsigned)cycle_.rungs.size());
    cycle_ = Cycle();
  }
  if (catchUp_.chat == chat) catchUp_ = CatchUp();
  ramWaitMs_.erase(chat);
  clearReadWait(chat);
  thruDate_.erase(chat);
  changed(chat);
}

FoldStatus FoldDriver::status(const std::string& chat, uint32_t nowMs, uint32_t nowEpoch) const {
  FoldStatus s;
  s.chat = chat;
  if (const orch::ChatFold* f = folds_.peek(chat)) {
    s.lastOk = f->lastFoldEpoch;
    s.summaryBytes = (uint32_t)f->summary.size();
    s.thru = f->thru;
    if (!f->softStart.empty() && (f->thru.empty() || rowIdAfter(f->softStart, f->thru))) s.uncoveredTo = f->softStart;
    s.failedCycles = f->failedCycles;
    s.nextTry = f->nextTry;
    s.firstFail = f->firstFail;
  }
  const auto d = thruDate_.find(chat);
  if (d != thruDate_.end()) s.thruDate = d->second;
  s.paidToday = paidToday(nowEpoch);
  s.running = cycle_.chat == chat && cycleActive();
  s.manual = s.running && cycle_.manual;
  s.manualBusy = cycleActive() && cycle_.manual;
  if (s.running && cycle_.idx < cycle_.rungs.size()) {
    s.rungIndex = (uint32_t)cycle_.idx + 1;
    s.rungCount = (uint32_t)cycle_.rungs.size();
    s.rungHost = cycle_.rungs[cycle_.idx].host;
    s.rungModel = cycle_.rungs[cycle_.idx].model;
  }
  s.queued = std::find(ctl_.manualPendingChat.begin(), ctl_.manualPendingChat.end(), chat) !=
             ctl_.manualPendingChat.end();
  const uint32_t since = nowMs - lastManualEndMs_;
  if (manualEnded_ && since < kManualCooldownMs) s.cooldownS = (kManualCooldownMs - since + 999) / 1000;
  return s;
}

std::string FoldDriver::confirmation(const FoldSlice& s) {
  std::string text = "\xE2\x9C\x93 Compacted. Summary updated: covered " + std::to_string(s.rows) +
                     (s.rows == 1 ? " message" : " messages");
  if (s.firstDate.empty()) return text + ".";
  if (s.firstDate == s.lastDate || s.lastDate.empty()) return text + " on " + s.firstDate + ".";
  return text + " from " + s.firstDate + " to " + s.lastDate + ".";
}

std::string FoldDriver::failureNotice(uint32_t nextTry, uint32_t waitS, int32_t utcOffsetMin) {
  std::string when = "in " + waitWords(waitS);
  if (nextTry) when = "at " + localClock(nextTry, utcOffsetMin) + " (" + localDate(nextTry, utcOffsetMin) + ")";
  return "\xE2\x9A\xA0 Couldn't update the summary just now. It tries again by itself " + when + ".";
}

std::string FoldDriver::unreadNotice() {
  return "\xE2\x9A\xA0 Some older messages in this chat can't be read from storage, so its summary can't move "
         "past them. Restart the device; if this keeps happening, check the memory card.";
}

std::string FoldDriver::alertNotice(uint32_t sinceEpoch, int32_t utcOffsetMin) {
  return "\xE2\x9A\xA0 This chat's summary hasn't updated since " + localDate(sinceEpoch, utcOffsetMin) +
         ". It keeps retrying on its own; send /compact to try now.";
}

FoldAlertRoute foldAlertRoute(const std::string& chat, bool chatIsOwners, const std::string& ownerTelegram,
                              const std::string& webChat, const std::string& text) {
  FoldAlertRoute r;
  if (!chatIsOwners) return r;
  const orch::DeliveryChannel ch = orch::deliveryChannelOf(chat);
  if (ch != orch::DeliveryChannel::Serial && ch != orch::DeliveryChannel::Voice) {
    r.target = chat;   // the owner's Telegram chat, or the web chat
    r.text = text;
    return r;
  }
  r.target = !ownerTelegram.empty() ? ownerTelegram : webChat;
  if (!r.target.empty()) r.text = std::string("About ") + orch::deliveryChannelLabel(ch) + ": " + text;
  return r;
}

bool foldAlertSend(const std::string& chat, const std::string& text, bool chatIsOwners,
                   const std::string& ownerTelegram, const std::string& webChat,
                   bool (*send)(const std::string&, const std::string&)) {
  const FoldAlertRoute r = foldAlertRoute(chat, chatIsOwners, ownerTelegram, webChat, text);
  if (r.target.empty()) {
    hlog::logf("fold(%s): the daily alert is logged only (%s), the day's alert not spent", chat.c_str(),
               chatIsOwners ? "no chat a person reads" : "not the owner's chat");
    return false;
  }
  if (send && send(r.target, r.text)) return true;
  hlog::logf("fold(%s): the daily alert to %s was not delivered, the day's alert not spent", chat.c_str(),
             r.target.c_str());
  return false;
}

}  // namespace agent
