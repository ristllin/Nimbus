#pragma once
#include <cstdint>
#include <string>

#include "nimbus/orch/turn_journal.h"

// restart_notice - when the owner is told that the device restarted.
//
// The crash notice used to be queued at boot whatever the interrupted turn was,
// so a crash inside the 03:30 nightly memory pass pinged the owner at 03:33,
// outside the quiet gate the turn engine already has.
//
// The rule: a crash during an owner request (a message, photo, voice message,
// file or console request - the owner is waiting on that reply) is still told
// at boot. A crash during a background turn (a routine or a background task)
// is held and sent once, at the owner's next turn. A crash with nothing in
// progress is told at boot, as before. A restart that was not a crash gets no
// notice yet (a later change adds that line). Never more than one notice per
// boot.
namespace nimbus::orch {

enum class NoticeTiming : uint8_t { None, Now, Held };

struct RestartNoticePlan {
  NoticeTiming timing = NoticeTiming::None;
  std::string text;
};

// A turn kind whose reply the owner is waiting on (photo, file, voice,
// message, console). The remaining kinds (routine, background) are the
// device's own work.
bool isOwnerRequestKind(const std::string& kind);

// What to tell the owner about this boot, and when. `resetReason` is an
// esp_reset_reason value: a restart that was not a crash gets no notice yet.
// `lost` is the turn the restart cut short (nullptr when none); `crashCause`
// is the crash's plain cause.
RestartNoticePlan planRestartNotice(const std::string& deviceName, int resetReason, const TurnJournal* lost,
                                    const std::string& hhmm, const std::string& crashCause);

// Holds at most one notice and lets it out once. Each call returns the text to
// send now, or "" (and never the same text twice). Releasing moves the text
// out, so the device can guard a call with a spinlock (no allocation inside
// it).
class RestartNoticeQueue {
 public:
  // At boot, once: a Now plan is returned at once; a Held plan is kept.
  std::string atBoot(RestartNoticePlan plan);
  // A turn started: the held notice, if this is an owner turn.
  std::string atTurnStart(const std::string& turnKind);
  bool holding() const { return !held_.empty(); }

 private:
  std::string release();
  std::string held_;
  bool booted_ = false;
};

}  // namespace nimbus::orch
