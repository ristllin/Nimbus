#pragma once
#include <string>
#include <vector>

#include "nimbus/harness/plain/turn.h"
#include "nimbus/orch/model_caps.h"
#include "pending.h"

// tx_battery/rows.h - the battery's unit is a provider table row, not a dialect (lane E2-R3a; router
// design section 11, "Battery takes a row"). A row is one provider of the capability table
// (caps::builtinCaps().providers), direct, plus the same provider through the Cumulo router when the
// table says the router carries it (routedImagesMax > 0). Each row names one model of its slot, the
// endpoint host the battery dials, the engine dialect its provider row gives, and the wire lane whose
// codec it waits for (pending.h's sentinels).
//
// A row whose codec does not exist yet WAITS: a property suite runs the rows that are built, prints
// every waiting (property, row, lane) and counts them. It neither fails nor is silently skipped, and it
// turns on by itself when the codec's sentinel answers (txb::waitDecision; -DTXB_FORCE_PENDING runs
// every row).
namespace txb {

namespace caps = nimbus::orch::caps;

struct Row {
  std::string name;          // "anthropic", "openai routed", ...
  caps::Slot slot = caps::Slot::Anthropic;
  bool routed = false;
  plain::Dialect dialect = plain::Dialect::AnthropicMessages;
  std::string host;          // the endpoint the battery dials
  std::string model;         // one model of the slot the table knows (custom slots: their '*' row)
  const char* codecLane = "RB-W1a";   // the sentinel of the codec this row renders with
};

inline plain::Dialect engineDialect(caps::Dialect d) {
  switch (d) {
    case caps::Dialect::Messages: return plain::Dialect::AnthropicMessages;
    case caps::Dialect::Responses: return plain::Dialect::OpenAIResponses;
    case caps::Dialect::Chat: return plain::Dialect::ChatCompletions;
  }
  return plain::Dialect::ChatCompletions;
}

// The codec lane of a dialect (pending.h: RB-W1a Messages, RB-W2 Responses, RB-W3 Chat).
inline const char* codecLaneOf(plain::Dialect d) {
  switch (d) {
    case plain::Dialect::AnthropicMessages: return "RB-W1a";
    case plain::Dialect::OpenAIResponses: return "RB-W2";
    case plain::Dialect::ChatCompletions: return "RB-W3";
    case plain::Dialect::Flat: return "RB-W4";
  }
  return "RB-W3";
}

// The battery's model for a slot: the default head of each provider (the table must know it; the
// rows test checks), and a model of no table for the custom slots.
inline const char* batteryModel(caps::Slot s) {
  switch (s) {
    case caps::Slot::Anthropic: return "claude-sonnet-5-5";
    case caps::Slot::OpenAI: return "gpt-5.6";
    case caps::Slot::Mistral: return "mistral-large-4-0";
    case caps::Slot::Zai: return "glm-5.3";
    default: return "battery-model";
  }
}

inline constexpr const char* kRouterHost = "router.cumulo.test";
inline constexpr const char* kCustomHost = "custom.example.test";

// Every row of the built-in table, in table order, each direct row followed by its routed one.
inline std::vector<Row> tableRows() {
  const caps::CapsTable& t = caps::builtinCaps();
  std::vector<Row> out;
  for (size_t i = 0; i < t.providerCount; ++i) {
    const caps::ProviderRow& p = t.providers[i];
    Row r;
    r.slot = p.slot;
    r.name = caps::slotName(p.slot);
    r.dialect = engineDialect(p.dialect);
    r.codecLane = codecLaneOf(r.dialect);
    r.model = batteryModel(p.slot);
    const std::string host = caps::poolStr(t, p.host);
    r.host = host.empty() ? kCustomHost : host;
    out.push_back(r);
    if (p.routedImagesMax == 0) continue;
    r.name += " routed";
    r.routed = true;
    r.host = kRouterHost;
    out.push_back(r);
  }
  return out;
}

// The model a turn on the row names: a routed chat row names its upstream in the router selector
// ("zai/glm-5.3"); a bare id would reach the router's default upstream (lane E2-R6).
inline std::string turnModel(const Row& r) {
  if (!r.routed || r.dialect != plain::Dialect::ChatCompletions) return r.model;
  return std::string(caps::slotName(r.slot)) + "/" + r.model;
}

// Whether the row's table row offers tools (false: text mode, a custom endpoint before its verify).
inline bool rowTools(const Row& r) {
  return (caps::capsFor(r.slot, r.model.c_str(), r.routed).row.flags & caps::flag::kTools) != 0;
}

// The first lane a (row, extra lanes) case still waits for; "" when it runs. A lane with no sentinel
// fails the case (a battery bug), as TXB_WAITS_FOR does.
inline std::string rowWaitsFor(const Row& r, std::initializer_list<const char*> extra = {}) {
  std::string err;
  const std::string message = std::string("PENDING ") + r.codecLane;
  const std::string lane = firstUnbuilt(message.c_str(), extra, err);
  if (!err.empty()) return "ERROR " + err;
  return forcePending() ? "" : lane;
}

// The waiting cases of a run, printed by each property and counted at the end of the suite.
struct WaitLog {
  std::vector<std::string> lines;
  void note(const char* property, const Row& r, const std::string& lane) {
    lines.push_back(std::string(property) + " | " + r.name + " | waits for " + lane);
  }
};
inline WaitLog& waitLog() {
  static WaitLog w;
  return w;
}

}  // namespace txb
