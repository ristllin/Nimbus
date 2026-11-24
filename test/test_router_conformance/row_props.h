#pragma once
#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <vector>

#include "../support/scripted_provider/request_view.h"
#include "nimbus/harness/http.h"
#include "nimbus/orch/model_caps.h"
#include "nimbus/orch/token_usage.h"

// test_router_conformance/row_props.h - the layer-L3 properties of router design section 11 as pure
// checkers (lane E2-R3a). Each answers true or false with the reason in `why`; the suite runs them on
// every provider row's real renders and engine turns, and counter-tests each one on a deliberate break.
namespace rc {

namespace sp = harness_test::scripted;
namespace caps = nimbus::orch::caps;

// ---- 1. same input, same bytes --------------------------------------------------------------------
inline bool sameBytes(const std::string& a, const std::string& b, const std::string& what, std::string& why) {
  if (a == b) return true;
  size_t at = 0;
  while (at < a.size() && at < b.size() && a[at] == b[at]) ++at;
  why = what + ": the bytes differ at offset " + std::to_string(at);
  return false;
}

// ---- 2. stable prefix, tools identical every round ----------------------------------------------------
inline bool toolsSame(const sp::RequestView& a, const sp::RequestView& b, std::string& why) {
  if (a.toolsJson == b.toolsJson) return true;
  why = "the tools array changed between rounds";
  return false;
}

// ---- 4. tool round trip ---------------------------------------------------------------------------
// The call `name` of request `callAt`'s answer was dispatched exactly once, and request callAt + 1
// shows that call followed by its result, paired.
inline bool roundTripHolds(const std::vector<std::string>& dispatched, const std::string& name,
                           const sp::RequestView& next, std::string& why) {
  size_t runs = 0;
  for (const std::string& d : dispatched) runs += d == name;
  if (runs != 1) {
    why = name + " ran " + std::to_string(runs) + " times, not once";
    return false;
  }
  std::string callId;
  bool resultAfter = false;
  for (const sp::Message& m : next.messages)
    for (const sp::Block& b : m.blocks) {
      if (b.kind == sp::BlockKind::ToolCall && b.name == name) callId = b.id;
      if (b.kind == sp::BlockKind::ToolResult && !callId.empty() && b.id == callId) resultAfter = true;
    }
  if (callId.empty()) why = "the next request does not show the call " + name;
  else if (!resultAfter) why = "the next request shows no result after the call " + callId;
  else return sp::callsPaired(next, why);
  return false;
}

// ---- 5. a not-complete answer's calls never run ------------------------------------------------------
inline bool nothingRan(const std::vector<std::string>& dispatched, const std::string& name, std::string& why) {
  for (const std::string& d : dispatched)
    if (d == name) {
      why = "the call " + name + " of a cut answer ran";
      return false;
    }
  return true;
}

// ---- 7. usage arithmetic ----------------------------------------------------------------------------
// The whole input of a request (uncached + read + write) from a parsed usage, by the row's usage mode.
inline uint64_t requestIn(const nimbus::orch::TokenUsage& u, caps::UsageMode mode) {
  return mode == caps::UsageMode::InputExcludesCache
             ? (uint64_t)u.promptTokens + u.cacheReadTokens + u.cacheWriteTokens
             : (uint64_t)u.promptTokens + u.cacheWriteTokens;
}

// ---- 10. never reduce history ---------------------------------------------------------------------------
inline size_t ownerMessages(const sp::RequestView& v) {
  size_t n = 0;
  for (const sp::Message& m : v.messages)
    for (const sp::Block& b : m.blocks) n += m.role == "user" && b.kind == sp::BlockKind::Text;
  return n;
}
inline bool historyKept(const sp::RequestView& earlier, const sp::RequestView& later, std::string& why) {
  if (ownerMessages(later) >= ownerMessages(earlier) && later.messages.size() >= earlier.messages.size()) return true;
  why = "a later request carries less history (" + std::to_string(later.messages.size()) + " messages after " +
        std::to_string(earlier.messages.size()) + ")";
  return false;
}

// ---- 11. limits ---------------------------------------------------------------------------------------------
inline constexpr size_t kMaxString = 60000;
inline constexpr int kMaxNesting = 32;

// Bytes [at, end) are one valid UTF-8 sequence starting at `at`; returns its length, 0 when invalid.
inline size_t utf8Len(const std::string& s, size_t at) {
  const unsigned char c = (unsigned char)s[at];
  size_t n = c < 0x80 ? 1 : (c >> 5) == 0x6 ? 2 : (c >> 4) == 0xE ? 3 : (c >> 3) == 0x1E ? 4 : 0;
  if (n == 0 || (n == 2 && c < 0xC2) || at + n > s.size()) return 0;
  for (size_t i = 1; i < n; ++i)
    if (((unsigned char)s[at + i] >> 6) != 0x2) return 0;
  return n;
}

// One pass over a JSON body: valid UTF-8, no string longer than kMaxString bytes (escapes as sent),
// nesting at most kMaxNesting.
inline bool limitsHold(const std::string& body, std::string& why) {
  int depth = 0, deepest = 0;
  bool inStr = false;
  size_t strStart = 0, longest = 0;
  for (size_t i = 0; i < body.size();) {
    const size_t n = utf8Len(body, i);
    if (!n) {
      why = "invalid UTF-8 at offset " + std::to_string(i);
      return false;
    }
    const char c = body[i];
    if (inStr) {
      if (c == '\\') i += 1;
      else if (c == '"') {
        inStr = false;
        longest = std::max(longest, i - strStart);
      }
    } else if (c == '"') {
      inStr = true;
      strStart = i + 1;
    } else if (c == '{' || c == '[') {
      deepest = std::max(deepest, ++depth);
    } else if (c == '}' || c == ']') {
      --depth;
    }
    i += n;
  }
  if (longest > kMaxString) why = "a string of " + std::to_string(longest) + " B (limit 60,000)";
  else if (deepest > kMaxNesting) why = "nesting " + std::to_string(deepest) + " (limit 32)";
  else return true;
  return false;
}

// A routed request carries the deadline header and declares its output cap.
inline bool routedDeclares(const agent::HttpRequest& req, const sp::RequestView& v, std::string& why) {
  bool deadline = false;
  for (const auto& h : req.headers) deadline |= h.first == "x-cumulo-deadline-ms" && std::atol(h.second.c_str()) > 0;
  if (!deadline) why = "a routed request without x-cumulo-deadline-ms";
  else if (v.maxTokens == 0) why = "a routed request that declares no output cap";
  else return true;
  return false;
}

}  // namespace rc
