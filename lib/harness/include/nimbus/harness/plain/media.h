#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "nimbus/harness/plain/hooks.h"
#include "nimbus/harness/plain/turn.h"

// plain/media.h - photos on the transcript (PRD section 5): pixels in the user message
// (Anthropic by Files API file_id, elsewhere base64 spliced at send time), then the
// photo rule of tx/rules.h, then the stub line and view. A photo never goes to a
// provider the owner did not choose. Registered in HookSlot::Media.
//
// Seam declared by lane RB-B0a; the body (plain_media.cpp) is lane RB-P1's.
namespace agent {
namespace plain {

struct MediaDeps {
  std::string visionHost;   // the vision provider the owner chose ("" = none)
};

// May this host receive the photo's pixels?
Status photoAllowed(const TurnCtx& turn, const MediaDeps& deps, bool& out);
Status installMedia(PlainHooks& hooks, const MediaDeps& deps);

}  // namespace plain
}  // namespace agent
