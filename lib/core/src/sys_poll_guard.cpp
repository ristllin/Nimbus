#include "nimbus/sys/poll_guard.h"

#include <cstddef>
#include <cstdio>
#include <cstring>

#include "nimbus/sys/breadcrumb.h"   // crc32

namespace nimbus::sys {

// ---- the liveness check -----------------------------------------------------

void PollLiveness::arm(uint32_t nowMs) {
  lastMs_.store(nowMs, std::memory_order_relaxed);
  step_.store("start", std::memory_order_relaxed);
  episodeMs_ = nowMs;
  samples_ = 0;
  onCpu_ = 0;
  fired_ = false;
  armed_.store(true, std::memory_order_relaxed);
}

void PollLiveness::disarm() { armed_.store(false, std::memory_order_relaxed); }

void PollLiveness::beat(uint32_t nowMs, const char* step) {
  // The step first: a reader that sees the new time never pairs it with an older step
  // than the one before it (both are relaxed; a stale pair names the previous step).
  step_.store(step ? step : "", std::memory_order_relaxed);
  lastMs_.store(nowMs, std::memory_order_relaxed);
}

bool PollLiveness::check(uint32_t nowMs, RunState state, uint32_t limitMs, PollStall& out) {
  if (!armed()) return false;
  const uint32_t last = lastMs_.load(std::memory_order_relaxed);
  if (last != episodeMs_) {   // a beat since the last check: a new episode
    episodeMs_ = last;
    samples_ = 0;
    onCpu_ = 0;
    fired_ = false;
  }
  const uint32_t silent = nowMs - last;   // wraps with millis()
  if (silent >= kPollSampleAfterMs && samples_ < UINT16_MAX) {
    ++samples_;
    if (state == RunState::Running || state == RunState::Ready) ++onCpu_;
  }
  if (fired_ || silent < limitMs) return false;
  fired_ = true;
  out.silentMs = silent;
  out.step = lastStep();
  out.samples = samples_;
  out.onCpu = onCpu_;
  out.gone = state == RunState::Gone;
  return true;
}

// ---- the streak record ------------------------------------------------------

namespace {
uint32_t storeCrc(const PollStallStore& s) {
  return crc32(reinterpret_cast<const uint8_t*>(&s), offsetof(PollStallStore, crc));
}
}  // namespace

bool PollStallRecord::intact(uint32_t build) const {
  return st_.magic == kPollStallMagic && st_.build == build && st_.step[sizeof st_.step - 1] == 0 &&
         st_.crc == storeCrc(st_);
}

void PollStallRecord::seal() { st_.crc = storeCrc(st_); }

void PollStallRecord::clear(uint32_t build) {
  std::memset(&st_, 0, sizeof st_);
  st_.magic = kPollStallMagic;
  st_.build = build;
  seal();
}

bool PollStallRecord::boot(uint32_t build) {
  build_ = build;
  if (!intact(build)) {
    clear(build);
    return false;
  }
  const bool guardRestart = st_.pending != 0;
  st_.pending = 0;
  seal();
  return guardRestart;
}

void PollStallRecord::noteStall(const PollStall& s) {
  if (st_.streak < UINT8_MAX) ++st_.streak;
  st_.pending = 1;
  st_.silentS = s.silentMs / 1000u;
  std::snprintf(st_.step, sizeof st_.step, "%s", s.step ? s.step : "");
  seal();
}

void PollStallRecord::noteHealthy() {
  if (st_.streak == 0) return;
  st_.streak = 0;
  seal();
}

// ---- the words ----------------------------------------------------------------

namespace {
// Printable ASCII only, at most `cap` bytes: the panic reason and the log line are read
// on a serial console and in the crash lines.
std::string printable(const char* s, size_t cap) {
  std::string out;
  for (const char* p = s ? s : ""; *p && out.size() < cap; ++p)
    out.push_back((*p >= 0x20 && *p <= 0x7e) ? *p : '?');
  return out;
}

std::string judgement(const PollStall& s) {
  if (s.gone) return "the task is gone";
  char b[64];
  std::snprintf(b, sizeof b, "on the CPU %u/%u (%s)", (unsigned)s.onCpu, (unsigned)s.samples,
                s.spinning() ? "a spin" : "waiting");
  return b;
}
}  // namespace

std::string pollStallReason(const PollStall& s, uint32_t pc, const uint32_t* bt, size_t btLen) {
  char head[160];
  std::snprintf(head, sizeof head, "%s tg_poll silent %u s after %s, %s", kPollStallTag,
                (unsigned)(s.silentMs / 1000u), printable(s.step, 24).c_str(), judgement(s).c_str());
  std::string r = head;
  if (pc) {
    char a[24];
    std::snprintf(a, sizeof a, ", pc 0x%08x", (unsigned)pc);
    r += a;
    if (bt && btLen) r += " bt";
    for (size_t i = 0; bt && i < btLen && i < kPollBacktraceMax; ++i) {
      std::snprintf(a, sizeof a, " 0x%08x", (unsigned)bt[i]);
      if (r.size() + std::strlen(a) > kPollStallReasonMax) break;
      r += a;
    }
  }
  if (r.size() > kPollStallReasonMax) r.resize(kPollStallReasonMax);
  return r;
}

bool isPollStallReason(const std::string& why) { return why.rfind(kPollStallTag, 0) == 0; }

std::string pollStallLogLine(const PollStall& s, uint8_t streak, uint32_t limitMs, bool restarting) {
  char b[220];
  std::snprintf(b, sizeof b,
                "telegram: the poll task has been silent %u s (limit %u s) after %s, %s - %s "
                "(stall %u in a row)",
                (unsigned)(s.silentMs / 1000u), (unsigned)(limitMs / 1000u), printable(s.step, 24).c_str(),
                judgement(s).c_str(), restarting ? "restarting the device" : "restart already asked",
                (unsigned)streak);
  return b;
}

}  // namespace nimbus::sys
