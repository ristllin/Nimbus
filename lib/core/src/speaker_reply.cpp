#include "nimbus/speaker_reply.h"

#include <cstring>

namespace nimbus {
namespace sfx {

uint32_t ReplyTicket::queue() {
  if (++seq_ == 0) seq_ = 1;   // 0 means "not queued" to callers
  state_ = ReplyState::Queued;
  why_[0] = '\0';
  return seq_;
}

void ReplyTicket::playing(uint32_t ticket) {
  if (ticket == seq_ && state_ == ReplyState::Queued) state_ = ReplyState::Playing;
}

void ReplyTicket::done(uint32_t ticket) {
  if (ticket == seq_ && state_ != ReplyState::Failed) state_ = ReplyState::Done;
}

void ReplyTicket::failed(uint32_t ticket, const char* why) {
  if (ticket != seq_) return;
  state_ = ReplyState::Failed;
  std::strncpy(why_, why && why[0] ? why : "the clip did not play", sizeof(why_) - 1);
  why_[sizeof(why_) - 1] = '\0';
}

ReplyState ReplyTicket::state(uint32_t ticket) const {
  if (ticket == 0 || ticket != seq_) return ReplyState::Failed;
  return state_;
}

std::string ReplyTicket::why(uint32_t ticket) const {
  if (ticket == 0) return "the clip was not queued";
  if (ticket != seq_) return "a newer spoken reply replaced it";
  return state_ == ReplyState::Failed ? std::string(why_) : std::string();
}

std::string speakerFailText(const std::string& why) {
  return "could not play on the speaker: " + why;
}

std::string speakerOpenFailReason(size_t largestDmaBlock) {
  return "the audio driver could not start (largest free DMA memory block " +
         std::to_string((unsigned long long)largestDmaBlock) + " B)";
}

}  // namespace sfx
}  // namespace nimbus
