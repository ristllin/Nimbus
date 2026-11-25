#pragma once
// tx/result_text.h - the words of a kept result's stub (tx::stubText, lanes E2-FIX9 and E2-BFIX3): what is true of
// the result in this request, and the call that reads it again. Model text: the catalog reads every literal of
// this file (tools/release_gate/gen_model_text_catalog.py, engine 2, class stubs):
//   '[stub <tool> r<q> ...] (removed from this history after you read it in full and replied; view id=r<q> shows it
//   again)'; an outside result's stub puts the same words first: '[stub removed from this history after you read it
//   in full and replied: <tool>(<args>) r<q> outside content; view id=r<q> shows it again]' (no byte count).
// A result kept on the card was shown whole in its own turn (the bulk file holds every byte), and its stub carries
// no preview (a head read the first bytes as all the tool had returned). The way back is view when the engine bound
// it (the exact bytes the head was shown), else a file read's read_file call (tx::wayBack), else none. With no
// storage card a result was kept inline up to a cap and the preview is all that is left of it:
//   '[stub ...] <preview> ... (shown earlier in this conversation; not attached to this request; full result not
//   kept: no storage card)' (the photo line's words, tx/photo_text.h).
namespace nimbus {
namespace orch {
namespace tx {

inline constexpr char kResultFactsOpen[] = "(";
inline constexpr char kResultTailOpen[] = " ... (";
inline constexpr char kResultShownInFull[] = "removed from this history after you read it in full and replied";
inline constexpr char kResultViewOpen[] = "; view id=r";
inline constexpr char kResultAgainOpen[] = "; ";
inline constexpr char kResultNoCard[] = "; full result not kept: no storage card";
inline constexpr char kResultTailClose[] = ")";

}  // namespace tx
}  // namespace orch
}  // namespace nimbus
