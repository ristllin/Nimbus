#pragma once
#include <string>
#include <vector>

#include "nimbus/orch/caps.h"

// scratchpad - the model's own structured working memory: short-term /
// mid-term / long-term goals + an active-task line. A scratchpad the model
// rewrites as it works, distinct from the user directive (immutable, elsewhere)
// and from associative vector memory (VDB, Ph3). This is the "what am I doing /
// planning" tier.
//
// The model proposes edits (via a `memory.scratchpad` tool, Ph1 tool registry);
// the DEVICE enforces the caps here - count (kScratchTierItems) and per-item /
// active byte length (UTF-8-safe, kScratchItemMax / kScratchActiveMax) - because
// a model cannot be trusted to self-limit and a runaway tier would blow the
// context budget. Setters return false when they had to reject/truncate so the
// device can log it, mirroring OrchMemory::setModel's truncation signal.
//
// Persistence is behind a seam (serialize/deserialize to one string) so the
// device can store it in NVS alongside the model memory; host tests use the
// round-trip directly. Arduino-free -> unit-tested via pio test -e native.
namespace nimbus {
namespace orch {

enum class Tier { Short = 0, Mid = 1, Long = 2 };

class Scratchpad {
 public:
  // ---- active task (single line: "what I'm doing right now") ----
  // Capped UTF-8-safe to kScratchActiveMax. Returns true if it fit without
  // truncation. Empty clears it.
  bool setActiveTask(const std::string& v);
  const std::string& activeTask() const { return active_; }

  // ---- tiers (short / mid / long) ----
  // Append one item. Rejected (returns false) if the tier is already full
  // (kScratchTierItems) or the item is empty after trimming; truncated
  // UTF-8-safe to kScratchItemMax otherwise (still returns true - truncation is
  // not rejection, matching setActiveTask).
  bool add(Tier t, const std::string& item);
  // Replace a whole tier at once (the common model op: "here is my new plan").
  // Applies the count cap (extra items dropped) and per-item byte cap. Returns
  // the number of items kept.
  int  replace(Tier t, const std::vector<std::string>& items);
  void clear(Tier t);
  const std::vector<std::string>& items(Tier t) const;
  int  count(Tier t) const { return (int)items(t).size(); }

  bool empty() const;
  void clearAll();

  // ---- prompt rendering (context assembler §2 §5) ----
  // Appends a "## SCRATCHPAD" block to `out` (only the non-empty parts). No-op
  // if empty() so the assembler can skip a blank section cleanly.
  void appendPromptBlock(std::string& out) const;

  // ---- persistence seam ----
  // Compact, self-delimiting serialization for NVS. deserialize() is tolerant:
  // a malformed blob yields an empty scratchpad rather than throwing, so a
  // corrupt NVS value degrades gracefully (same policy as the journal).
  std::string serialize() const;
  bool deserialize(const std::string& blob);

 private:
  std::vector<std::string>& tier(Tier t);
  const std::vector<std::string>& tierC(Tier t) const;

  std::string active_;
  std::vector<std::string> short_, mid_, long_;
};

}  // namespace orch
}  // namespace nimbus
