#pragma once
#include <functional>
#include <string>

#include "lab_rig.h"

// Live leg for albums as one turn (lane D):
//
//   albums-one-turn   an album of three photos plus the follow-up question, rendered
//                     exactly as the device renders one turn (the real batcher and the
//                     one media label) from captions the device's current captioner
//                     wrote for three synthetic photos; the head's one reply covers
//                     every photo and never claims one is missing.
//
// Run on claude-haiku-5-5 three times (effort low until the cap table lands) and once
// each on mistral-small-latest and glm-5.3-flash, with --record=<dir> so the spend is
// priced from provider-reported usage (tools/harness-lab/README.md has the commands).
namespace lab {
namespace albums {

struct Report {
  std::function<void(bool ok, const std::string& what)> check;
  std::function<void(const std::string& what)> note;
};

void albumsOneTurn(LabRig& rig, const Report& r);

}  // namespace albums
}  // namespace lab
