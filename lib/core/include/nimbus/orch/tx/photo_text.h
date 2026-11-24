#pragma once
// tx/photo_text.h - the words of a photo's line when the photo is not attached to a request
// (tx::photoStub, lane E2-FIX4). Model text: the catalog reads every literal of this file
// (tools/release_gate/gen_model_text_catalog.py, engine 2, class stubs).
namespace nimbus {
namespace orch {
namespace tx {

// After '[photo p<ref> <when>: ' (and the photo's note and '; ' when one is stored).
inline constexpr char kPhotoNotAttached[] = "shown earlier in this conversation; not attached to this request";
// Only when the engine has bound the view tool: '; view id=p<ref> shows it again'.
inline constexpr char kPhotoViewOpen[] = "; view id=p";
inline constexpr char kPhotoViewClose[] = " shows it again";
// Lane E2-P-PHOTO (PC-0231): a head without image input, for a photo with no stored description.
inline constexpr char kPhotoNoSight[] = "this model cannot see photos";
// Lane E2-P-PHOTO (PC-0232): a photo the provider refused (its format) in this conversation.
inline constexpr char kPhotoRefused[] = "the provider refused this photo, so it is not attached";

}  // namespace tx
}  // namespace orch
}  // namespace nimbus
