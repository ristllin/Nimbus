#pragma once
#include <string>

#include "lab_rig.h"

// nimbus-lab photo-eval - the food-photo accuracy benchmark driver. For each
// photo in a manifest it runs one meal-analysis turn through the real engine
// (the same code the firmware runs) and records the reply verbatim. Scoring
// against the goldens happens outside, in Python, so the driver never grades.
namespace lab {

struct PhotoEvalOptions {
  std::string dir;    // the manifest directory: photos.json + the JPEGs
  std::string out;    // output directory for results.jsonl
  std::string prompt; // empty = the manifest's per-photo prompt
  bool sharedChat = false;   // every photo in one chat (--shared-chat), not a fresh one each
  bool noDrain = false;      // uploads wait in the delete queue (--no-drain): past its
                             // bound a turn's photo goes inline next to kept files
};

int cmdPhotoEval(const Env& env, const LabRig::Options& opt, const PhotoEvalOptions& pe);

}  // namespace lab
