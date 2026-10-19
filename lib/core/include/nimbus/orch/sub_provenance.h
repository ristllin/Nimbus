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

}  // namespace orch
}  // namespace nimbus
