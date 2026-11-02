#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

// food_label - the deterministic nutrition-label check, in the code path that
// stores a labelled row (2 Nov, Lumi v4.6.4: a Frive Grilled Chicken Breast Side
// whose label reads 41.0 g protein per serving and about 31.3 per 100 g was stored
// as 4.3 and 3.3; the label's own arithmetic says so at once: 4x41 + 9x7.3 + 4x1.2 is
// 234.5 against 235 kcal, while 4x4.3 with the same fat and carb is 88).
//
// Two checks, both pure arithmetic on the figures a label read states:
//   - Atwater, per column: energy against 4P + 4C + 9F (rounded label macros can be
//     off, so the tolerance is 12% or 10 kcal, whichever is larger);
//   - the columns against each other: per-100 g x net weight / 100 against per
//     serving, for kcal and for protein (a swapped or digit-dropped column fails here).
// A mismatch never lands silently: the store-time gate (labelRowGate) refuses the
// row and names the figure in doubt; the head re-reads the label once, and if it
// still disagrees, stores the row marked [unverified: <figure>] - which the gate
// lets through, so the doubt is on the row, not silent.
//
// The second read (a second vision provider's digits, compared with the head's:
// labelReadDisagreements) is surfaced, never averaged. Provenance
// (unsupportedDetails): a brand or detail a row asserts that no owner message,
// label read or tool result this turn holds is not written unless the row is
// marked [assumed].
//
// Portable, Arduino-free, host-tested (test_food_label).
namespace nimbus::orch {

// One column of a label read (per 100 g, or per serving / whole pack).
struct LabelColumn {
  bool has = false;          // any figure was found for this column
  bool hasKcal = false, hasProtein = false, hasCarb = false, hasFat = false;
  double kcal = 0, protein = 0, carb = 0, fat = 0;
};

// A label read: the two columns and the net weight, parsed from a label
// transcription or from the bracketed label note a ledger row carries.
struct LabelRead {
  bool hasPer100 = false, hasPerServing = false;
  LabelColumn per100, perServing;
  bool netKnown = false;
  double netG = 0;
};

// Parse the label figures in `text` (a vision transcription of a nutrition table,
// or a row's "[label per serving = whole pack: 611 kcal, 54.9 g protein, ...]").
// Tolerant and deterministic: "per 100 g"/"per 100g" opens the per-100 g column,
// "per serving"/"per pack"/"whole pack" the per-serving one; a figure belongs to
// the column opened last before it. "600/143 kcal" reads as the kcal value (143).
LabelRead parseLabelRead(const std::string& text);

// One figure the label's own arithmetic doubts.
struct LabelDoubt {
  std::string figure;   // "kcal per serving", "protein per 100 g", "kcal per serving vs per 100 g"
  double stated = 0;    // the figure as read
  double computed = 0;  // what the label's own arithmetic gives
  std::string why;      // "4P+4C+9F" or "per-100 g x net weight"
};

// The Atwater and cross-column checks on a parsed read. Empty when the read is
// consistent (or holds too little to check).
std::vector<LabelDoubt> checkLabelRead(const LabelRead& r);

// The store-time gate on one ledger row's text: "" when the row may land (no label
// figures in it, they check out, or the row is marked [unverified: ...]); otherwise
// the refusal naming the figure in doubt and the one re-read that may follow.
// `turnSources` is the turn's input text (the owner's message and the label reads
// in it): the row's figures are checked against it and merged with it.
std::string labelRowGate(const std::string& rowText, const std::string& turnSources);

// One figure two reads of the same label state differently.
struct FigureDisagreement {
  std::string field;   // "protein per serving", "kcal per 100 g", "net weight"
  double head = 0, second = 0;
};
// The figures the head's read and a second vision read disagree on. Never averaged:
// each disagreement is surfaced with both values.
std::vector<FigureDisagreement> labelReadDisagreements(const std::string& headText,
                                                       const std::string& secondText);
// The surfaced note for a second read of the same label (the post-turn vision
// record, by `byProvider`): "" when the two reads agree, else one line naming each
// figure they disagree on with both values - never averaged.
std::string secondReadNote(const std::string& headText, const std::string& secondText,
                           const std::string& byProvider);

// The brand/detail tokens a row's items assert that appear nowhere in `sourceText`
// (the turn's sources as raw text: the owner's message, the label reads, the tool
// results; normalized inside the same way as the candidates, so "M&S" in the sources
// backs "m s" in the row). A token counts as a brand/detail when it holds "&", is
// all caps, or has an internal capital ("M&S", "KFC", "McFlurry"). "" when every such
// token is backed, or the row is marked [assumed].
std::string unsupportedDetails(const std::string& rowText, const std::string& sourceText);

}  // namespace nimbus::orch
