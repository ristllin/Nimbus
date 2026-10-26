#include "nimbus/heap_watch.h"

#include <cstdio>

namespace nimbus::heapwatch {

size_t ringSince(const FailRing& r, uint32_t since, FailRecord* out, uint32_t& lost) {
  lost = 0;
  if (r.total <= since) return 0;   // nothing new (or the ring was reset under us)
  uint32_t first = since;
  if (r.total - since > kRingSize) {
    lost = r.total - since - (uint32_t)kRingSize;
    first = r.total - (uint32_t)kRingSize;
  }
  size_t n = 0;
  for (uint32_t i = first; i < r.total; ++i) out[n++] = r.rec[i % kRingSize];
  return n;
}

std::string capsString(uint32_t caps) {
  // The bits that name a memory kind, in the order a reader looks for them. The
  // values mirror esp_heap_caps.h (the device glue static_asserts each one).
  static const struct { uint32_t bit; const char* name; } kBits[] = {
      {1u << 3, "DMA"},      {1u << 11, "INTERNAL"}, {1u << 10, "SPIRAM"},
      {1u << 12, "DEFAULT"}, {1u << 2, "8BIT"},      {1u << 1, "32BIT"},
      {1u << 0, "EXEC"},     {1u << 13, "IRAM_8BIT"}, {1u << 15, "RTCRAM"},
  };
  std::string s;
  for (const auto& b : kBits) {
    if (!(caps & b.bit)) continue;
    if (!s.empty()) s += '|';
    s += b.name;
  }
  return s.empty() ? std::string("0") : s;
}

std::string failLine(const FailRecord& e) {
  char task[kTaskNameLen];
  size_t i = 0;
  for (; i < kTaskNameLen - 1 && e.task[i]; ++i) {
    const char c = e.task[i];
    task[i] = (c >= 0x21 && c <= 0x7e) ? c : '?';   // printable, no spaces
  }
  task[i] = 0;
  char b[200];
  snprintf(b, sizeof(b), "heap: alloc FAILED size=%u caps=%s task=%s free=%u largest=%u up=%us",
           (unsigned)e.size, capsString(e.caps).c_str(), i ? task : "?", (unsigned)e.intFree,
           (unsigned)e.intLargest, (unsigned)(e.ms / 1000));
  return b;
}

// ---- activity tags ----------------------------------------------------------

const char* tagName(Tag t) {
  static const char* const kNames[kTagCount] = {
      "turn", "relay.dial", "relay.bulk", "tunnel", "tg.dl", "vision",
      "stt",  "ota",        "dream",      "verify", "worker", "lan.big",
  };
  return t < kTagCount ? kNames[t] : "?";
}

std::string tagsString(uint32_t mask) {
  std::string s;
  for (uint8_t t = 0; t < kTagCount; ++t) {
    if (!(mask & (1u << t))) continue;
    if (!s.empty()) s += '|';
    s += tagName(Tag(t));
  }
  return s.empty() ? std::string("-") : s;
}

void Activity::enter(Tag t) {
  if (t >= kTagCount) return;
  cnt_[t].fetch_add(1, std::memory_order_relaxed);
  sticky_.fetch_or(1u << t, std::memory_order_relaxed);
}

void Activity::leave(Tag t) {
  if (t >= kTagCount) return;
  uint16_t v = cnt_[t].load(std::memory_order_relaxed);
  // Never wrap below zero (an unmatched leave is a caller bug, not a reason to
  // report a tag as live forever).
  while (v && !cnt_[t].compare_exchange_weak(v, uint16_t(v - 1), std::memory_order_relaxed)) {
  }
}

void Activity::touch(Tag t) {
  if (t < kTagCount) sticky_.fetch_or(1u << t, std::memory_order_relaxed);
}

uint32_t Activity::active() const {
  uint32_t m = 0;
  for (uint8_t t = 0; t < kTagCount; ++t)
    if (cnt_[t].load(std::memory_order_relaxed)) m |= 1u << t;
  return m;
}

uint32_t Activity::sample() {
  const uint32_t live = active();
  const uint32_t seen = sticky_.exchange(live, std::memory_order_relaxed);
  return seen | live;
}

// ---- low-water ladder -------------------------------------------------------

LadderStep ladderNote(Ladder& l, uint32_t minFree, uint32_t tags, uint32_t nowMs) {
  LadderStep st;
  if (minFree >= l.low) return st;   // not a new low (a monitor window can read higher)
  st.newLow = true;
  l.low = minFree;
  l.lowTags = tags;
  l.lowUpMs = nowMs;
  if (minFree >= kLadderLogBelow) return st;
  if (l.logged != UINT32_MAX && minFree + kLadderStepBytes > l.logged) return st;
  st.log = true;
  st.was = l.logged;
  l.logged = minFree;
  return st;
}

std::string ladderLine(const Ladder& l, uint32_t was, uint32_t freeNow, uint32_t largestNow) {
  char wasBuf[16];
  if (was == UINT32_MAX) snprintf(wasBuf, sizeof(wasBuf), "none");
  else snprintf(wasBuf, sizeof(wasBuf), "%u", (unsigned)was);
  char b[200];
  snprintf(b, sizeof(b), "heap: new low-water %u (was %s) free=%u largest=%u tags=%s up=%us",
           (unsigned)l.low, wasBuf, (unsigned)freeNow, (unsigned)largestNow,
           tagsString(l.lowTags).c_str(), (unsigned)(l.lowUpMs / 1000));
  return b;
}

// ---- rate limit -------------------------------------------------------------

bool LineBudget::take(uint32_t nowMs) {
  if (!started || (uint32_t)(nowMs - windowStart) >= kWindowMs) {
    started = true;
    windowStart = nowMs;
    used = 0;
  }
  if (used < kPerWindow) {
    ++used;
    return true;
  }
  ++dropped;
  return false;
}

uint32_t LineBudget::takeDropped() {
  const uint32_t d = dropped;
  dropped = 0;
  return d;
}

// ---- resting largest --------------------------------------------------------

void RestingLargest::note(uint32_t largest) {
  s[next] = largest;
  next = uint8_t((next + 1) % kSamples);
  if (n < kSamples) ++n;
}

uint32_t RestingLargest::resting(uint32_t fallback) const {
  if (!n) return fallback;
  uint32_t m = UINT32_MAX;
  for (uint8_t i = 0; i < n; ++i)
    if (s[i] < m) m = s[i];
  return m;
}

// ---- spawn gate -------------------------------------------------------------

SpawnVerdict spawnVerdict(uint32_t stackBytes, uint32_t intFree, bool turnInFlight) {
  if (turnInFlight) return SpawnVerdict::TurnInFlight;
  // 64-bit sum: a hostile or garbage stack size can never wrap the need below free.
  const uint64_t need = (uint64_t)stackBytes + kSpawnMarginBytes;
  return (uint64_t)intFree >= need ? SpawnVerdict::Ok : SpawnVerdict::LowMemory;
}

SpawnVerdict spawnVerdictAtBoot(uint32_t stackBytes, uint32_t intFree) {
  const uint64_t need = (uint64_t)stackBytes + kBootSpawnMarginBytes;   // no wrap
  return (uint64_t)intFree >= need ? SpawnVerdict::Ok : SpawnVerdict::LowMemory;
}

const char* spawnVerdictName(SpawnVerdict v) {
  switch (v) {
    case SpawnVerdict::Ok: return "ok";
    case SpawnVerdict::TurnInFlight: return "turn in flight";
    case SpawnVerdict::LowMemory: return "low memory";
  }
  return "?";
}

// ---- the boot's loopTask stack floor -----------------------------------------

std::string loopStackFloorLine(uint32_t minBytes, bool taskPresent) {
  char b[192];
  if (taskPresent && !loopStackUnderFloor(minBytes)) {
    std::snprintf(b, sizeof b, "boot stack: loopTask min free %u B, floor %u B - ok",
                  (unsigned)minBytes, (unsigned)kLoopStackFloorBytes);
    return b;
  }
  std::snprintf(b, sizeof b,
                "boot stack: BOOT FAULT - loopTask min free %s%u B is under the %u B floor: "
                "the boot path grew past the loopTask stack (see docs/memory.md)",
                taskPresent ? "" : "?? (no task handle) ", (unsigned)minBytes,
                (unsigned)kLoopStackFloorBytes);
  return b;
}

// ---- the orchestrator boot task's stack floor ---------------------------------

std::string orchBootStackFloorLine(uint32_t minBytes, uint32_t stackBytes, bool measured) {
  char b[208];
  if (!measured) {
    std::snprintf(b, sizeof b,
                  "boot stack: BOOT FAULT - orchboot left no stack watermark (the orchestrator "
                  "boot task did not report; see docs/memory.md)");
    return b;
  }
  if (!orchBootStackUnderFloor(minBytes)) {
    std::snprintf(b, sizeof b, "boot stack: orchboot min free %u B of %u B, floor %u B - ok",
                  (unsigned)minBytes, (unsigned)stackBytes, (unsigned)kOrchBootStackFloorBytes);
    return b;
  }
  std::snprintf(b, sizeof b,
                "boot stack: BOOT FAULT - orchboot min free %u B of %u B is under the %u B floor: "
                "the orchestrator boot chain grew past its task stack (see docs/memory.md)",
                (unsigned)minBytes, (unsigned)stackBytes, (unsigned)kOrchBootStackFloorBytes);
  return b;
}

}  // namespace nimbus::heapwatch
