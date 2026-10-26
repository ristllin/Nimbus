#pragma once
#include <cstddef>
#include <string>

// body_budget - the one wording of the request-body budget refusal.
//
// A provider refuses to SEND a request whose assembled body is over the wire
// budget (lib/harness providers.h kWireBodyMax): the step fails before any byte
// leaves the device, so nothing is billed for it. The tool loop (head_loop.h)
// reads the refusal back to shrink the turn's replayed tool results and run the
// same round again, instead of failing a turn whose rounds were already paid
// (Lumi, v4.6.0: 8 rounds, then "request body over the 85 KB budget: 86865 B").
// Both sides use these two functions, so the text and its reader cannot drift.
// Arduino-free, host-tested (test_loop_body_budget).
namespace nimbus {
namespace orch {

// "request body over the 85 KB budget: 86865 B" (a log and failure-chain line;
// `budget` is a whole number of kilobytes in bytes, as kWireBodyMax is).
inline std::string bodyBudgetError(size_t budget, size_t size) {
  return "request body over the " + std::to_string(budget / 1000) + " KB budget: " + std::to_string(size) +
         " B";
}

// True when `err` is that refusal; `budget` and `size` receive its two figures.
inline bool parseBodyBudgetError(const std::string& err, size_t& budget, size_t& size) {
  static const char kHead[] = "request body over the ";
  static const char kMid[] = " KB budget: ";
  if (err.rfind(kHead, 0) != 0) return false;
  size_t p = sizeof(kHead) - 1;
  auto number = [&err, &p](size_t& out) {
    const size_t from = p;
    out = 0;
    while (p < err.size() && err[p] >= '0' && err[p] <= '9') out = out * 10 + (size_t)(err[p++] - '0');
    return p > from;
  };
  size_t kb = 0;
  if (!number(kb) || err.compare(p, sizeof(kMid) - 1, kMid) != 0) return false;
  p += sizeof(kMid) - 1;
  if (!number(size)) return false;
  budget = kb * 1000;
  return true;
}

}  // namespace orch
}  // namespace nimbus
