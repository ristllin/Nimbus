#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

// food_figures - the daily coach's figures, held to the device's records before delivery.
//
// The week of 28 Oct, in the owner's words: "Wtf. How 3989???", "38.3g protein left from
// what???", "Why 2500? Cant you verify the plan we built?". The model did the arithmetic and
// made up the targets: a 250 g protein target and a 2,500 kcal target no file, rule or message
// held, a day total carried over from yesterday, "you need ~102 g more" over a meal whose protein
// nobody knew. The device holds the ledger, and knows what the turn was given, so it checks:
//   - a daily target stated in a reply needs its number in something the turn read: the stored
//     targets, the standing rules, long-term memory, a file or tool result, a sub-agent's result or
//     the owner's own words. The assistant's own earlier replies and a fold summary are NOT
//     sources: that is how a made-up figure repeats itself into fact;
//   - a figure "left" implies a target: the ledger's total plus what is left. It needs a source too;
//   - a day total in a reply must be the ledger's;
//   - with a row of unknown protein, what is left is at most a figure, never a need.
// One correction round with the model, then one calm owner line per kind (claim_check.cpp).
//
// The same scanner rules as claims.h: deterministic, one pass, precision first. A question, a
// hypothetical, a future, a negated or retracted figure and another day's total are not claims.
// English only (the caller's scan decides). Portable, host-tested (test_food_figures).
namespace nimbus::orch {

// What the device holds right now for the chat's person.
struct FoodFacts {
  bool have = false;         // the ledger could be read and the clock is set
  bool kcalKnown = true;     // no row of today is unreadable
  double kcal = 0;
  int rows = 0;              // rows counted in kcal
  double protein = 0;        // the known part
  int proteinUnknown = 0;    // rows with no protein figure
};

enum class FoodGapKind : uint8_t { UnsourcedTarget = 0, WrongDayTotal, ProteinLeftUnknown };

struct FoodGap {
  FoodGapKind kind = FoodGapKind::UnsourcedTarget;
  std::string excerpt;   // the sentence, at most ~100 bytes
  double stated = 0;     // the figure the reply states (or, for a "left" figure, the target it implies)
  double truth = 0;      // WrongDayTotal: the ledger's figure
  bool protein = false;  // the figure is grams of protein, not kcal
  bool implied = false;  // UnsourcedTarget: a "left" figure implied it
};

// `sourceNumbers`: the normalized numbers (figureNumbers, claims.h) of what a target may come from.
std::vector<FoodGap> foodFigureGaps(const std::string& reply, const FoodFacts& facts,
                                    const std::string& sourceNumbers);
// The targets alone, with no ledger: a memory about to be written, a targets row.
std::vector<FoodGap> unsourcedFoodTargets(const std::string& text, const std::string& sourceNumbers);

// A routine citing old data as if it were the state of things (1 Nov, a daily check-in: "7
// consecutive days as of 2026-10-14", 18 days stale). A sentence with a status word and a date older
// than `maxAgeDays` before `today` (YYYY-MM-DD). Checked on scheduled turns only: an owner who asks
// about the past may be told about it. Returns the first such sentence, its age through `ageDays`.
inline constexpr int kStaleStatusDays = 7;
std::string staleStatusIn(const std::string& reply, const std::string& today, int maxAgeDays, int* ageDays);

// The owner's words a target may come from: a statement, not a question or a complaint about a
// figure ("Why 2500?", "You hallucinate the 250g"), which must not source the figure it disputes.
std::string ownerStatementsOf(const std::string& text);

// The owner's line on a stale status: what the device can say about it.
std::string staleStatusNote(int ageDays);
// Does this text speak of food figures at all? Any case: "Protein:", "Calories".
bool speaksOfFood(const std::string& text);
// The first target figure in a write to the food targets file that nothing this turn holds: a whole
// row, part of one (files.edit) or a bare figure. A date or a source's words are no target figure.
bool unsourcedTargetWrite(const std::string& text, const std::string& sourceNumbers, double* figure);

// The text of the one correction round.
std::string foodGapCorrection(const std::vector<FoodGap>& gaps, const FoodFacts& facts);
// The owner's lines, one per kind, each on its own paragraph ("" when no gaps).
std::string foodGapTail(const std::vector<FoodGap>& gaps, const FoodFacts& facts);

}  // namespace nimbus::orch
