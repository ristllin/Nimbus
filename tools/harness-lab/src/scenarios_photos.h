#pragma once
#include <functional>
#include <string>

#include "lab_rig.h"

// Live legs for photos to the head as images (lane E, owner ruling R1). The photos
// come from a directory (NIMBUS_LAB_PHOTOS) holding JPEGs and a truth.json that lists,
// per photo, the fields a reader must copy exactly ({"photos":[{"file":..,
// "fields":[..]}]}); without it the legs skip loudly.
//
//   photos-native        an album of three label photos goes to the head as image
//                        parts (the real renderer, the body splice, the turn's photo
//                        slot); the one reply reads the label values from the pixels,
//                        and no caption call is made.
//   photo-label-honesty  a photo the provider could not read for billing (the typed
//                        label): the head says it could not read it because the
//                        provider is out of credit, and neither asks for a resend nor
//                        blames the owner's phone (L19-4).
//   caption-scoring      the fallback caption step on each keyed provider's current
//                        model, every photo twice: the share of fields copied exactly
//                        and no empty caption (pass: at least 80 percent, 20 or more
//                        calls in all).
namespace lab {
namespace photos {

struct Report {
  std::function<void(bool ok, const std::string& what)> check;
  std::function<void(const std::string& what)> note;
};

void photosNative(LabRig& rig, const Report& r);
void photoLabelHonesty(LabRig& rig, const Report& r);
void captionScoring(LabRig& rig, const Report& r);

}  // namespace photos
}  // namespace lab
