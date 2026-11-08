#include "nimbus/orch/degraded.h"

// RED STUB (failing-first record): every entry point answers "nothing degraded". The real
// implementation replaces this file in the next commit.
namespace nimbus {
namespace orch {

const char* degradedSlug(DegradedId) { return ""; }
DegradedSet computeDegraded(const DegradedFacts&) { return DegradedSet(); }
std::string degradedProviderLabel(const std::string& slug) { return slug; }
ParkReason degradedParkReason(const ParkedFact&) { return ParkReason(); }
void ServingTracker::noteTurn(const std::string&, const std::string&, const std::string&, bool, uint32_t) {}
FallbackFact ServingTracker::fact(uint32_t) const { return FallbackFact(); }
void ServingTracker::clear() {}
bool DegradedDebounce::update(const DegradedSet&, uint32_t) { return false; }
void DegradedDebounce::reset() {}
std::vector<std::string> degradedStripRows(const DegradedSet&, size_t) { return {}; }
std::string degradedDetailText(const DegradedSet&) { return std::string(); }
void degradedInto(const DegradedSet&, uint32_t, JsonObject) {}

}  // namespace orch
}  // namespace nimbus
