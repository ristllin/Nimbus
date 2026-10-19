#pragma once
#include <cstdint>
#include <string>

// log_window - keeps the durable log's forensic window long (rows 44 and 57).
//
// On Lumi the per-serve relay lines were 83-98% of every rotated SD log file, and the
// per-exchange heaptrace and per-round headloop lines most of the rest, so a 256 KB
// file covered 0.7-5.2 h: whatever led up to a crash had aged out within hours. The
// routine copies of those lines now stay in the RAM ring and on serial (GET /api/log
// still shows every one); the durable log keeps the abnormal ones, and one folded relay
// summary per window. Abnormal means the thing a crash investigation needs: a failed
// exchange or serve, a failed allocation, or internal memory near its floor.
namespace nimbus::sys {

// Internal-SRAM floors below which a sample is forensic evidence (heap F1.5).
inline constexpr uint32_t kForensicMinFree = 12 * 1024;
inline constexpr uint32_t kForensicMinLargest = 8 * 1024;

// One transport exchange (the heaptrace line). Unknown samples stay at UINT32_MAX.
struct TraceFacts {
  bool fail = false;               // the exchange returned no response
  uint32_t allocFails = 0;         // failed internal allocations during it (0 until counted)
  uint32_t minFree = UINT32_MAX;   // lowest internal free sampled
  uint32_t minLargest = UINT32_MAX;// lowest largest internal block sampled
};
bool traceAbnormal(const TraceFacts& t);
// Fold one heap sample into the minimums. A (0, 0) sample is "not taken" (the
// transport leaves the post-write sample unset when there was no body), never a real
// reading: counting it would mark every body-less exchange abnormal.
void noteTraceSample(TraceFacts& t, uint32_t free, uint32_t largest);

// The per-round "headloop round=N tools=T heap=H intLargest=L [cap=...]" line from the
// head loop. Routine (RAM ring only) unless the round was capped or memory was low.
// Any other line, and a headloop line that does not parse, is not routine.
bool headloopRoutine(const std::string& line);
// The round number of a "headloop round=N" line; false for any other line.
bool parseHeadloopRound(const std::string& line, int& round);

// Folds the per-serve relay lines into one durable summary per window.
class RelayFold {
 public:
  static constexpr uint32_t kWindowMs = 15UL * 60UL * 1000UL;

  struct Serve {
    int status = 0;          // loopback HTTP status (0 = no response)
    uint32_t bytes = 0;      // response body bytes
    uint32_t intMin = UINT32_MAX;  // internal free low-water during the serve
  };

  // An abnormal serve keeps its own durable line: no response, a 5xx, or internal
  // memory under the floor during the serve.
  static bool abnormal(const Serve& s);

  // Count one serve. Returns the durable summary line when the window that started at
  // the first serve after the last summary has run its course, else "".
  std::string note(const Serve& s, uint32_t nowMs, uint32_t heapMin);

  uint32_t serves() const { return serves_; }

 private:
  void reset(uint32_t nowMs);
  bool open_ = false;
  uint32_t startMs_ = 0;
  uint32_t serves_ = 0, ok2xx_ = 0, r3xx_ = 0, r4xx_ = 0, r5xx_ = 0, err_ = 0;
  uint64_t bytes_ = 0;
  uint32_t intMin_ = UINT32_MAX;
  uint32_t heapMin_ = UINT32_MAX;
};

}  // namespace nimbus::sys
