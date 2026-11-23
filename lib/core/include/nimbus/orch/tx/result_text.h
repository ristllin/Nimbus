#pragma once
// tx/result_text.h - the words that end an internal result's stub (tx::stubText, lane E2-FIX9): what is true of
// the result in this request, and the call that reads it again. Model text: the catalog reads every literal of
// this file (tools/release_gate/gen_model_text_catalog.py, engine 2, class stubs). The middle words are the
// photo line's (tx/photo_text.h), so a result and a photo out of the request read alike:
//   '[stub <tool> r<q> ...] <preview> ... (shown earlier in this conversation; not attached to this request; view
//   id=r<q> shows it again)'
// The way back is view when the engine bound it (the exact bytes the head was shown), else a file read's
// read_file call (tx::wayBack), else none; with no storage card the full result was not kept.
namespace nimbus {
namespace orch {
namespace tx {

inline constexpr char kResultTailOpen[] = " ... (";
inline constexpr char kResultViewOpen[] = "; view id=r";
inline constexpr char kResultAgainOpen[] = "; ";
inline constexpr char kResultNoCard[] = "; full result not kept: no storage card";
inline constexpr char kResultTailClose[] = ")";

}  // namespace tx
}  // namespace orch
}  // namespace nimbus
