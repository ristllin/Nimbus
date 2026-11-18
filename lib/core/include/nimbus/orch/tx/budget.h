#pragma once
#include <cstdint>
#include <string>

#include "nimbus/orch/tx/status.h"

// tx/budget.h - profiles and the per-head budget (PRD section 2, "Budget"). The body
// cap per head is min(main's requestBudgetFor, the qualification table's cap); on top
// sits a resident target for tier 1 plus stubs since thru: compaction triggers at H and
// keeps L. The figures are generated from measured data by lane RB-T4; the constants
// here are the PRD's ceilings, which no lane raises.
//
// Seam declared by lane RB-B0a; the body (orch_tx_budget.cpp) is lane RB-T4's.
namespace nimbus {
namespace orch {
namespace tx {

// The slugs are stored in head.json and t items: frozen.
enum class Profile : uint8_t { Tight = 0, Lean = 1, Standard = 2, Wide = 3 };
inline const char* profileSlug(Profile p) {
  switch (p) {
    case Profile::Tight: return "tight";
    case Profile::Lean: return "lean";
    case Profile::Standard: return "standard";
    case Profile::Wide: return "wide";
  }
  return "lean";
}

inline constexpr uint32_t kFixedOverheadCeiling = 18432;   // system 8,192 + tools 10,240
inline constexpr uint32_t kSystemCeiling = 8192;
inline constexpr uint32_t kToolsCeiling = 10240;
inline constexpr uint32_t kFloorHeadBodyCap = 85000;       // routed, custom, unqualified heads
inline constexpr uint32_t kBulkWholeMax = 48000;           // tier 2 shown whole in its own turn

struct ProfileMarks {
  uint32_t high = 0;   // H: compaction triggers above it
  uint32_t low = 0;    // L: compaction keeps this much
};

// Which head a budget is for.
struct HeadKey {
  std::string host;
  std::string model;
  bool routed = false;
  bool qualified = false;   // the qualification table's state for this engine
};

struct HeadBudget {
  uint32_t bodyCap = 0;        // whole request body ceiling
  Profile profile = Profile::Lean;
  ProfileMarks marks;
  uint32_t bulkWholeCap = 0;   // min(48,000 B, bodyCap / 8)
  uint32_t digestBlock = 0;    // digest block plus recent-files list
};

// The marks of a profile (tight 24,000/12,000 ... wide 100,000/50,000).
Status profileMarks(Profile p, ProfileMarks& out);
// The budget of one head at a requested profile; an unqualified head is held at
// kFloorHeadBodyCap and the lean profile.
Status budgetFor(const HeadKey& head, Profile requested, HeadBudget& out);

}  // namespace tx
}  // namespace orch
}  // namespace nimbus
