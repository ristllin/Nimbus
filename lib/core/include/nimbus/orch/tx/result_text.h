#pragma once
// tx/result_text.h - the words that end an internal result's stub (tx::stubText, lane E2-FIX9): what is true of
// the result in this request, and the call that reads it again. Model text: the catalog reads every literal of
// this file (tools/release_gate/gen_model_text_catalog.py, engine 2, class stubs):
//   '[stub <tool> r<q> ...] (shown in full earlier in this conversation; not attached to this request; view id=r<q>
//   shows it again)'
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
inline constexpr char kResultShownInFull[] = "shown in full earlier in this conversation; not attached to this request";
inline constexpr char kResultViewOpen[] = "; view id=r";
inline constexpr char kResultAgainOpen[] = "; ";
inline constexpr char kResultNoCard[] = "; full result not kept: no storage card";
inline constexpr char kResultTailClose[] = ")";

}  // namespace tx
}  // namespace orch
}  // namespace nimbus
