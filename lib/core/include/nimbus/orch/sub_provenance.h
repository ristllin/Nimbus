#pragma once
// sub_provenance - where a sub-agent's result came from.
//
// A sub-agent that ran no lookup still returns confident text: on 14 Oct a rail
// check answered "No disruptions reported." with zero tool calls, and the head
// passed it on as live status. The device knows how many tools each sub ran
// (ResultEnvelope.toolRuns / webRuns, from the provider's own record), so:
//   - a live-data task (status, weather, prices, news, ...) whose sub ran no
//     tool is marked unverified, exactly like the connector case (CUM-536);
//   - every [FRESH RESULTS] line says how many tools ran, so the head can judge
//     any result, live or not.
//
// Portable, Arduino-free, host-tested (test_orch_claims).

#include <cstddef>
#include <cstdint>
#include <string>

namespace nimbus {
namespace orch {

// True when the task asks for something that changes by the hour: a status,
// disruptions or delays, today's or current figures, the latest news, weather,
// prices, scores, traffic, transport lines. Whole words, case-insensitive,
// English only; a missed phrase only means no marker (the conservative side).
bool liveTask(const std::string& task);

// The result is the model's own guess: the task named an attached connector
// that never ran, or it asked for live data and no tool ran at all.
inline bool subResultUnverified(bool connectorAsked, uint16_t connectorCalls, bool live,
                                uint16_t toolRuns) {
  return (connectorAsked && connectorCalls == 0) || (live && toolRuns == 0);
}

// The marker put in front of an unverified live-data result (model-facing).
std::string liveDataNotCheckedNote();

// The provenance suffix of a [FRESH RESULTS] line: " [tools: 2, web: 1]",
// " [tools: 1]" or " [no tools ran]".
std::string freshProvenanceTag(uint16_t toolRuns, uint16_t webRuns);

// Which tools a sub-agent ran, by name, with counts, from the provider's own record
// (27 Oct: "Why Notion?" was answered with an invented "Notion is my only weather
// source", because later turns saw only "[tools: 2]"). A tool the run was offered
// and never ran counts 0, so "notion_search x2, web_search x0" says what the sub
// chose. At most kMaxNames names; a name is cut to kNameMax printable characters.
class ToolTally {
 public:
  static constexpr size_t kMaxNames = 8;
  static constexpr size_t kNameMax = 32;
  // One run of `name` (the provider's tool name).
  void ran(const std::string& name) { bump(name, 1); }
  // A tool the run was offered: listed with 0 runs unless it ran (an adapter that sees
  // the offer calls it: Mistral does, OpenAI and Anthropic report only what ran).
  void offered(const std::string& name) { bump(name, 0); }
  bool empty() const { return n_ == 0 && otherRuns_ == 0; }
  // "web_search x2, notion x0", the tools that ran first, then the idle ones, in
  // first-seen order; runs of tools past kMaxNames as "other tools xN". At most `cap`
  // bytes: a cut list keeps whole names and ends ", +N more". "" when nothing ran or
  // was offered.
  std::string render(size_t cap) const;
  // render() into a fixed field (the envelope's toolNames).
  template <size_t N>
  void copyTo(char (&field)[N]) const {
    const std::string r = render(N - 1);
    r.copy(field, N - 1);
    field[r.size() < N - 1 ? r.size() : N - 1] = '\0';
  }

 private:
  void bump(const std::string& name, uint16_t by);
  struct Row {
    char name[kNameMax + 1] = {};
    uint16_t runs = 0;
  };
  Row rows_[kMaxNames];
  size_t n_ = 0;
  uint16_t otherRuns_ = 0;   // runs of tools past kMaxNames
};

// The tool tally of a [FRESH RESULTS] line or a spawn ledger line: " [ran: web_search
// x2, notion x0]", or "" when the provider recorded no names (the count tag above
// still says how many ran).
std::string toolRunsTag(const char* names);

}  // namespace orch
}  // namespace nimbus
