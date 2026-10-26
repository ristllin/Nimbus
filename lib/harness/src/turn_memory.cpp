#include "nimbus/harness/turn_memory.h"

#include <cstdio>
#include <utility>

// A turn never spends the device's last internal SRAM (see turn_memory.h).
namespace agent {

const char kShortOfMemoryReply[] = "I'm short of memory right now. Please try again in a minute.";

void TurnMemoryGate::begin(uint32_t freeB, uint32_t largest, uint32_t floorFree, uint32_t floorLargest) {
  floorFree_ = floorFree;
  floorLargest_ = floorLargest;
  n_ = 0;
  dropped_ = 0;
  refusedAt_ = nullptr;
  firstUnder_ = nullptr;
  phase("admit", freeB, largest);
}

void TurnMemoryGate::phase(const char* name, uint32_t freeB, uint32_t largest) {
  if (n_ < kMaxMarks) marks_[n_++] = Mark{name, freeB, largest};
  else ++dropped_;
  if (!firstUnder_ && !holds(freeB, largest)) firstUnder_ = name;
}

bool TurnMemoryGate::dispatch(const char* name, uint32_t freeB, uint32_t largest) {
  phase(name, freeB, largest);
  refusedAt_ = holds(freeB, largest) ? nullptr : name;
  return refusedAt_ == nullptr;
}

uint32_t TurnMemoryGate::lowFree() const {
  uint32_t low = UINT32_MAX;
  for (int i = 0; i < n_; ++i)
    if (marks_[i].freeB < low) low = marks_[i].freeB;
  return n_ ? low : 0;
}

uint32_t TurnMemoryGate::setupCost() const {
  if (!n_) return 0;
  const uint32_t low = lowFree();
  return marks_[0].freeB > low ? marks_[0].freeB - low : 0;
}

std::string TurnMemoryGate::line(bool full) const {
  if (!n_) return "turn memory: no samples";
  char b[64];
  std::string out = "turn memory:";
  if (full) {
    for (int i = 0; i < n_; ++i) {
      std::snprintf(b, sizeof b, " %s=%u/%u", marks_[i].name, (unsigned)marks_[i].freeB,
                    (unsigned)marks_[i].largest);
      out += b;
    }
    if (dropped_) {
      std::snprintf(b, sizeof b, " (+%d more)", dropped_);
      out += b;
    }
  } else {
    int lowAt = 0;
    for (int i = 1; i < n_; ++i)
      if (marks_[i].freeB < marks_[lowAt].freeB) lowAt = i;
    std::snprintf(b, sizeof b, " admit=%u/%u", (unsigned)marks_[0].freeB, (unsigned)marks_[0].largest);
    out += b;
    std::snprintf(b, sizeof b, " low=%u/%u at %s", (unsigned)marks_[lowAt].freeB,
                  (unsigned)marks_[lowAt].largest, marks_[lowAt].name);
    out += b;
  }
  std::snprintf(b, sizeof b, " cost=%u floor=%u/%u", (unsigned)setupCost(), (unsigned)floorFree_,
                (unsigned)floorLargest_);
  out += b;
  if (refusedAt_) out += std::string(" UNDER at ") + refusedAt_;
  else out += firstUnder_ ? std::string(" ok (dipped at ") + firstUnder_ + ")" : std::string(" ok");
  return out;
}

namespace {
std::function<void(bool)>& psramSwitch() {
  static std::function<void(bool)> sw;
  return sw;
}
int s_psramDepth = 0;
}  // namespace

void setPsramFirstSwitch(std::function<void(bool on)> sw) { psramSwitch() = std::move(sw); }

PsramFirstScope::PsramFirstScope() {
  if (s_psramDepth++ == 0 && psramSwitch()) psramSwitch()(true);
}

PsramFirstScope::~PsramFirstScope() {
  if (--s_psramDepth == 0 && psramSwitch()) psramSwitch()(false);
}

int PsramFirstScope::depth() { return s_psramDepth; }

}  // namespace agent
