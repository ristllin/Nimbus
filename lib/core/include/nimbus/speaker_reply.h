#pragma once
// speaker_reply - the outcome of one spoken reply on the device speaker, as the task
// that plays it reports it, so the reply tool answers with what happened instead of
// "spoken" the moment the clip was queued.
//
// The speaker plays on the sound task, after the turn's tool has returned. On 22 Oct
// the speaker driver could not start (a 1,024 B DMA allocation failed), the failure
// was swallowed, and the turn still counted the clip as played. Now the tool queues
// the clip, waits a bounded time for the sound task to open the speaker, and reports
// "could not play on the speaker: <reason>" when it did not.
//
// One reply is tracked at a time: a newer clip replaces the older one's file, so an
// older ticket reads as replaced. The caller guards the object (one writer, the sound
// task; one reader, the turn). Plain data: zero-initialized (a static, or `{}`) it
// tracks no reply, so the device keeps it in PSRAM. Portable and host-tested
// (test_speaker_reply).

#include <cstddef>
#include <cstdint>
#include <string>

namespace nimbus {
namespace sfx {

enum class ReplyState : uint8_t {
  Queued,    // waiting for the sound task (also the zero state, read only for ticket 0)
  Playing,   // the speaker opened and the clip is playing
  Done,      // the clip played to its end
  Failed,    // it did not play; why() says why
};

class ReplyTicket {
 public:
  // A new clip was queued: its ticket (never 0).
  uint32_t queue();
  void playing(uint32_t ticket);
  void done(uint32_t ticket);
  void failed(uint32_t ticket, const char* why);
  // The state of `ticket`; an older ticket than the newest reads Failed (replaced).
  ReplyState state(uint32_t ticket) const;
  // Why `ticket` failed ("" unless state() is Failed).
  std::string why(uint32_t ticket) const;

 private:
  uint32_t   seq_;       // the newest ticket (0: none yet)
  ReplyState state_;
  char       why_[80];
};

// True once the outcome of a ticket is known (anything but Queued).
inline bool settled(ReplyState s) { return s != ReplyState::Queued; }
// True when the clip is on the speaker or played (Playing or Done).
inline bool started(ReplyState s) { return s == ReplyState::Playing || s == ReplyState::Done; }

// The tool's failure text: "could not play on the speaker: <why>".
std::string speakerFailText(const std::string& why);
// Why the speaker driver would not start, from what the heap reports.
std::string speakerOpenFailReason(size_t largestDmaBlock);

}  // namespace sfx
}  // namespace nimbus
