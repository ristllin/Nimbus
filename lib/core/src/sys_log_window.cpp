#include "nimbus/sys/log_window.h"

namespace nimbus::sys {

namespace {

// The unsigned decimal after `key` (e.g. " heap="), or false.
bool fieldU32(const std::string& line, const char* key, uint32_t& out) {
  const size_t at = line.find(key);
  if (at == std::string::npos) return false;
  size_t i = at;
  while (key[i - at]) ++i;
  uint64_t v = 0;
  size_t digits = 0;
  for (; i < line.size() && line[i] >= '0' && line[i] <= '9'; ++i) {
    v = v * 10 + (uint64_t)(line[i] - '0');
    if (++digits > 10) return false;
  }
  if (!digits || v > 0xFFFFFFFFull) return false;
  out = (uint32_t)v;
  return true;
}

constexpr char kHeadloopPrefix[] = "headloop round=";

}  // namespace

bool traceAbnormal(const TraceFacts& t) {
  return t.fail || t.allocFails > 0 || t.minFree < kForensicMinFree ||
         t.minLargest < kForensicMinLargest;
}

void noteTraceSample(TraceFacts& t, uint32_t free, uint32_t largest) {
  if (free == 0 && largest == 0) return;
  if (free < t.minFree) t.minFree = free;
  if (largest < t.minLargest) t.minLargest = largest;
}

bool parseHeadloopRound(const std::string& line, int& round) {
  if (line.compare(0, sizeof(kHeadloopPrefix) - 1, kHeadloopPrefix) != 0) return false;
  uint32_t r = 0;
  if (!fieldU32(line, kHeadloopPrefix, r) || r > 1000) return false;
  round = (int)r;
  return true;
}

bool headloopRoutine(const std::string& line) {
  int round = 0;
  if (!parseHeadloopRound(line, round)) return false;
  if (line.find(" cap=") != std::string::npos) return false;   // a capped round is evidence
  uint32_t heap = 0, largest = 0;
  // A line without the samples cannot prove memory was fine, so it stays durable.
  if (!fieldU32(line, " heap=", heap) || !fieldU32(line, " intLargest=", largest)) return false;
  return heap >= kForensicMinFree && largest >= kForensicMinLargest;
}

bool RelayFold::abnormal(const Serve& s) {
  return s.status <= 0 || s.status >= 500 || s.intMin < kForensicMinFree;
}

void RelayFold::reset(uint32_t nowMs) {
  *this = RelayFold();
  open_ = true;
  startMs_ = nowMs;
}

std::string RelayFold::note(const Serve& s, uint32_t nowMs, uint32_t heapMin) {
  if (!open_) reset(nowMs);
  ++serves_;
  if (s.status <= 0) ++err_;
  else if (s.status < 300) ++ok2xx_;
  else if (s.status < 400) ++r3xx_;
  else if (s.status < 500) ++r4xx_;
  else ++r5xx_;
  bytes_ += s.bytes;
  if (s.intMin < intMin_) intMin_ = s.intMin;
  if (heapMin < heapMin_) heapMin_ = heapMin;
  if ((uint32_t)(nowMs - startMs_) < kWindowMs) return {};
  const uint32_t mins = (uint32_t)(nowMs - startMs_) / 60000UL;
  std::string line = "relay: " + std::to_string(serves_) + " serve(s) in " + std::to_string(mins) +
                     " min (2xx=" + std::to_string(ok2xx_) + " 3xx=" + std::to_string(r3xx_) +
                     " 4xx=" + std::to_string(r4xx_) + " 5xx=" + std::to_string(r5xx_) +
                     " none=" + std::to_string(err_) + ") bytes=" + std::to_string(bytes_);
  if (intMin_ != UINT32_MAX) line += " intMin=" + std::to_string(intMin_);
  if (heapMin_ != UINT32_MAX) line += " heapMin=" + std::to_string(heapMin_);
  open_ = false;   // the next serve opens a new window
  return line;
}

}  // namespace nimbus::sys
