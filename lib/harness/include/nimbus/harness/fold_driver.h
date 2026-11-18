#pragma once
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "nimbus/harness/conversation.h"   // FoldSlice - what a cycle freezes
#include "nimbus/harness/fold_ladder.h"    // FoldRung, FoldRungReport
#include "nimbus/orch/compact.h"           // FoldStore, FoldCtl, FoldCtlIO

// fold_driver - the one fold driver the device and the Virtual Nimbus share (lane F of
// the history fix, 16 Nov 2026; owner ruling 2: the summary can never stay off for
// days again). Each pump pass calls tick(), which runs at most ONE rung of a fold
// cycle (fold_ladder.h), so an owner message waits at most one rung:
//
//   start   a cycle starts when a chat is due (over the trigger, hidden rows, a manual
//           request) and the gates pass: no turn in flight, heap, network, the chat's
//           wait, and (automatic only) the owner-first quiet gate and the device-wide
//           daily cap. It freezes the chat, the manual flag, the slice and the rungs
//           (a (host, model) that answered empty or thin last time goes last).
//   rungs   the starting pass and each later passing pass run one rung. A failed rung
//           returns Pending (no failure, no notice); a later automatic rung re-checks
//           the quiet gate, a manual one never waits for quiet.
//   end     a summary lands (Ok), every rung failed (CycleFailed, exactly once: the
//           chat's next try is scheduled 15 min, 1 h, 4 h, then every 12 h out), or
//           the daily cap stops an automatic cycle (Deferred, uncounted).
//
// Manual requests (Summarize now, /compact) wait in the persisted queue (FoldCtl),
// run one at a time with a 2-minute cooldown, skip the daily cap (they are still
// counted) and are always answered: the confirmation, a failure with the next try
// time, or "nothing to summarize". A manual request is a catch-up: while the chat
// still has rows neither the summary nor the history holds, its cycles chain, oldest
// first (at most kCatchUpCycles), and one confirmation names the whole span. A manual request for a chat whose automatic cycle
// is running ends that cycle uncounted and starts a manual one with a keep-0 slice.
// One owner alert per local day, after 24 h of failing. With no clock the waits are
// kept in RAM on the monotonic clock, the cap counts under day 0 and no alert is sent.
// Single task: every call comes from the pump's task (tg_poll on the device, the
// engine thread on the VN).
namespace agent {

class TurnEngine;

enum class FoldTick : uint8_t {
  Idle = 0,      // nothing due
  Deferred,      // something is due, a gate said "not now" (no failure)
  Skipped,       // a due fold had nothing new to summarize (no paid call)
  Pending,       // a rung failed and rungs remain (no failure, no notice)
  Ok,            // a summary landed
  CycleFailed,   // every rung failed: the next try is scheduled
};
const char* foldTickName(FoldTick t);

// A landed summary, for the binding's write order: the store is already written; the
// binding resets the chat's provider chain, then writes the boundary row.
struct FoldDone {
  std::string chat;
  bool manual = false;
  std::string summary;   // as stored
  FoldSlice slice;       // the digest it took (thru, rows, dates)
  size_t producedBytes = 0;
  uint32_t bytesFolded = 0, msgsFolded = 0;   // the counters the fold restarted
  std::string host, model;
  uint32_t elapsedMs = 0;   // the cycle, start to landing
};

// One chat's fold health (GET /api/fold/status, the Memory pane's card).
struct FoldStatus {
  std::string chat;
  uint32_t lastOk = 0;            // epoch of the last summary (0 = never or no clock)
  uint32_t summaryBytes = 0;
  std::string thru, thruDate;     // the newest row the summary covers, and its local date
  std::string uncoveredTo;        // rows after thru up to this one are in neither ("" = none)
  uint32_t failedCycles = 0, nextTry = 0, firstFail = 0;
  uint32_t paidToday = 0, cap = nimbus::orch::kFoldPaidPerDay;
  bool running = false, manual = false;   // a cycle runs for this chat
  bool manualBusy = false;        // a manual cycle runs (any chat): one at a time
  uint32_t rungIndex = 0, rungCount = 0;   // the active rung, 1-based (0 = none)
  std::string rungHost, rungModel;
  bool queued = false;            // a manual request for this chat waits
  uint32_t cooldownS = 0;         // until a new manual cycle may start
};
// The status as JSON (every field present, never null: -1 = no age). `chats`, when
// given, adds "chats": every conversation the page can pick, with its badge's state.
std::string foldStatusJson(const FoldStatus& s, uint32_t nowEpoch, int32_t utcOffsetMin,
                           const std::vector<FoldStatus>* chats = nullptr);

struct FoldDriverHooks {
  std::function<bool()> online;                        // the network is up (unset = yes)
  std::function<bool(bool overflow)> quiet;            // an automatic rung may run now (unset = yes)
  std::function<void()> notDue;                        // nothing was due this pass
  std::function<uint32_t()> triggerBytes;              // the automatic trigger, 0 = off
  std::function<size_t()> foldCycleBytes;              // what one cycle may take (orch::foldCycleBytes)
  // The slice of a chat (default: the engine's historyFoldSlice).
  std::function<FoldSlice(const std::string& chat, size_t capBytes, bool manual)> slice;
  std::function<int32_t(uint32_t epoch)> utcOffsetMin;   // the owner's zone (unset = UTC)
  std::function<void(const FoldDone&)> folded;         // a summary landed (after the store)
  std::function<void(const std::string& chat, const std::string& text)> tell;   // an owner notice
  // The daily alert (unset = tell): the binding sends it to the owner's chats only and says
  // whether it did (lane CTXR1: the day's one alert is spent only when it reached the owner).
  std::function<bool(const std::string& chat, const std::string& text)> alert;
  // Whether `chat` is the owner's (unset = every chat is): the owner's due chats fold first,
  // and the others share at most kFoldGuestPaidPerDay of the daily cap.
  std::function<bool(const std::string& chat)> ownerChat;
  std::function<void(const std::string& chat)> changed;   // the status moved ("" = no one chat)
  std::function<void(bool)> asking;                    // true just before a rung's request, false after
  std::string firmware;                                // this image's version
  // No clock at boot: a chat that carries a persisted wait is held this long for the clock
  // to sync before it counts as due (a boot must not retry every chat in a 12 h backoff).
  static constexpr uint32_t kNoClockGraceMs = 10u * 60u * 1000u;
};

class FoldDriver {
 public:
  static constexpr uint32_t kManualCooldownMs = 120000;
  static constexpr uint32_t kCatchUpCycles = 64;   // one manual catch-up: about 3.4 MB at a 53.9 KB step
  // A slice that could not read its rows looks again 1 minute out, doubling to 1 hour.
  static constexpr uint32_t kReadWaitFirstMs = 60000;
  static constexpr uint32_t kReadWaitMaxMs = 3600000;
  // A manual read that goes on (a scan, a read budget) yields this long between its passes, and
  // is answered after this many of them.
  static constexpr uint32_t kManualPassMs = 1000;
  static constexpr uint32_t kManualWaitPasses = 64;
  static uint32_t readWaitMs(uint32_t waits);

  FoldDriver(TurnEngine& eng, nimbus::orch::FoldStore& folds, nimbus::orch::FoldCtlIO ctlIO, FoldDriverHooks hooks);

  // Load the control file; a new firmware restarts every chat's wait. Once, after the
  // fold store began.
  void begin();
  // The engine was rebuilt (the VN rebuilds it on a key or model change): later rungs
  // run on the new one; a running cycle keeps its frozen slice and rungs.
  void rebind(TurnEngine& eng) { eng_ = &eng; }
  // One pump pass: at most one rung.
  FoldTick tick(uint32_t nowMs, uint32_t nowEpoch);
  // The chat the last turn ran in: looked at first.
  void noteTurnChat(const std::string& chat) { lastTurnChat_ = chat; }

  enum class Manual : uint8_t { Queued = 0, AlreadyRunning, Full };
  Manual requestManual(const std::string& chat);
  // A /clear: a cycle or catch-up for the chat ends with its slice, uncounted.
  void noteCleared(const std::string& chat);

  FoldStatus status(const std::string& chat, uint32_t nowMs, uint32_t nowEpoch) const;
  bool cycleActive() const { return !cycle_.chat.empty(); }
  const nimbus::orch::FoldCtl& ctl() const { return ctl_; }
  // NIMBUS_TEST bench op (FOLDRESET): today's paid count restarts at 0.
  void resetDailyCount();

  // The owner-facing copy (Telegram, the web chat).
  static std::string confirmation(const FoldSlice& s);
  static std::string failureNotice(uint32_t nextTry, uint32_t waitS, int32_t utcOffsetMin);
  static std::string alertNotice(uint32_t sinceEpoch, int32_t utcOffsetMin);
  static std::string unreadNotice();   // rows the store refuses for a day: the daily alert

 private:
  struct Cycle {
    std::string chat;
    bool manual = false;
    bool overflow = false;
    FoldSlice slice;
    std::string prevSummary;
    uint32_t bytesAtStart = 0, msgsAtStart = 0;
    std::vector<FoldRung> rungs;
    size_t idx = 0;
    uint32_t startedMs = 0;
    FoldRungReport bestThin;   // the longest answer that only named too few sections
  };

  FoldTick startCycle(uint32_t nowMs, uint32_t nowEpoch);
  FoldTick pickChat(uint32_t nowMs, uint32_t nowEpoch, std::string& chat, bool& manual, bool& overflow);
  FoldTick nothingToFold(const std::string& chat, bool manual, uint32_t nowMs, const FoldSlice& s);
  bool pickFrom(uint32_t nowMs, uint32_t nowEpoch, bool owners, std::string& chat);
  FoldTick sliceWaits(const std::string& chat, bool manual, uint32_t nowMs, uint32_t nowEpoch, const FoldSlice& s);
  void noteUnread(const std::string& chat, uint32_t nowEpoch, bool unread);
  void endManualWait(const std::string& chat, uint32_t nowMs, SliceState state, bool tooLong);
  uint32_t manualWaitPasses_ = 0;   // passes the running manual request's read went on without landing
  uint32_t manualPassUntilMs_ = 0;  // the next pass of that read (the automatic folds go meanwhile)
  FoldTick runRung(uint32_t nowMs, uint32_t nowEpoch);
  FoldTick landed(const FoldRungReport& rep, const std::string& summary, uint32_t nowMs, uint32_t nowEpoch);
  FoldTick cycleFailed(uint32_t nowMs, uint32_t nowEpoch);
  void maybeAlert(const std::string& chat, const nimbus::orch::ChatFold* f, uint32_t nowEpoch, int32_t off);
  bool gatesOpen() const;
  bool automaticRungWaits() const;
  FoldTick stopAtCap();
  bool pickManual(uint32_t nowMs, std::string& chat) const;
  bool pickAutomatic(uint32_t nowMs, uint32_t nowEpoch, std::string& chat);
  bool dueNow(const std::string& chat, uint32_t nowMs, uint32_t nowEpoch);
  std::vector<FoldRung> orderedRungs() const;
  bool capReached(uint32_t nowEpoch, const std::string& chat = std::string());
  bool isOwnerChat(const std::string& chat) const { return !hooks_.ownerChat || hooks_.ownerChat(chat); }
  void endManual(const std::string& chat, uint32_t nowMs);
  void saveCtl();
  void changed(const std::string& chat = std::string()) const;
  uint32_t today(uint32_t nowEpoch) const;
  int32_t offsetAt(uint32_t epoch) const;

  TurnEngine* eng_;
  nimbus::orch::FoldStore& folds_;
  nimbus::orch::FoldCtlIO ctlIO_;
  FoldDriverHooks hooks_;
  nimbus::orch::FoldCtl ctl_;
  Cycle cycle_;
  std::string lastTurnChat_;
  std::set<std::string> degraded_;             // "host/model" that answered empty or thin
  std::map<std::string, uint32_t> ramWaitMs_;  // no clock: chat -> monotonic ms of its next try
  // A chat whose slice could not read its rows (SliceState Wait/Unread): looks in a row that
  // read nothing new, and the monotonic ms of the next look.
  std::map<std::string, uint32_t> readWaitCount_, readWaitUntilMs_;
  void clearReadWait(const std::string& chat);
  std::map<std::string, std::string> thruDate_;   // chat -> the local date its summary reaches
  // The manual catch-up in progress: what its landed cycles covered so far.
  struct CatchUp {
    std::string chat;
    FoldSlice span;   // rows summed, first and last date
    uint32_t cycles = 0;
  };
  CatchUp catchUp_;
  bool catchUpContinues(const std::string& chat, const FoldSlice& landed);
  uint32_t lastManualEndMs_ = 0;
  bool manualEnded_ = false;
  uint32_t capLoggedDay_ = 0xFFFFFFFFu;
  // Paid requests made with no clock: counted here, never under a real day's persisted
  // count (a boot before the clock syncs must not reset day D's cap). The count rolls over
  // 24 h (monotonic) after its first request: with no clock the day never ends otherwise.
  uint32_t clocklessPaid_ = 0, clocklessGuest_ = 0;
  uint32_t clocklessSinceMs_ = 0;
  uint32_t lastTickMs_ = 0, firstTickMs_ = 0;
  bool ticked_ = false;
  void notePaid(uint32_t nowEpoch, uint32_t n, const std::string& chat);
  uint32_t paidToday(uint32_t nowEpoch) const;
  uint32_t guestPaidToday(uint32_t nowEpoch) const;
  bool endAutomaticForManual();
};

}  // namespace agent
