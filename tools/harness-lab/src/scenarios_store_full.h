#pragma once
#include <functional>
#include <string>

#include "lab_rig.h"

// Live legs for a full file store (storm leg g, lane FSN):
//
//   store-full-photos   five owner photos ("keep this photo") into a full store, each
//                       turn rendered as the device renders it now: the photo's label
//                       plus the store state as a fact the owner already has, and the
//                       file tools refusing with the device's words. Each turn is one
//                       reply that answers the photo without explaining the store
//                       again, asks no "which files can I delete" and tries no save.
//
// Run on claude-haiku-5-5 (effort low until the cap table lands) and once each on
// mistral-small-latest and glm-5.3-flash, with --record=<dir> so the spend is priced
// from provider-reported usage.
namespace lab {
namespace storefull {

struct Report {
  std::function<void(bool ok, const std::string& what)> check;
  std::function<void(const std::string& what)> note;
};

void storeFullPhotos(LabRig& rig, const Report& r);

}  // namespace storefull
}  // namespace lab
