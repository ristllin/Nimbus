#include "nimbus/sys/poll_guard.h"

// FAILING-FIRST STUB (lane TGSTALL step 1): the pre-fix behavior, where nothing watches
// the poll task. Replaced by the implementation in the next commit.
namespace nimbus::sys {
void PollLiveness::arm(uint32_t) {}
void PollLiveness::disarm() {}
void PollLiveness::beat(uint32_t, const char*) {}
bool PollLiveness::check(uint32_t, RunState, uint32_t, PollStall&) { return false; }
bool PollStallRecord::boot(uint32_t) { return false; }
void PollStallRecord::noteStall(const PollStall&) {}
void PollStallRecord::noteHealthy() {}
bool PollStallRecord::intact(uint32_t) const { return false; }
void PollStallRecord::seal() {}
void PollStallRecord::clear(uint32_t) {}
std::string pollStallReason(const PollStall&, uint32_t, const uint32_t*, size_t) { return std::string(); }
bool isPollStallReason(const std::string&) { return false; }
std::string pollStallLogLine(const PollStall&, uint8_t, uint32_t, bool) { return std::string(); }
}  // namespace nimbus::sys
