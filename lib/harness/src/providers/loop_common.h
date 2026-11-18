#pragma once
// runFabricLoop - declared on the public surface (nimbus/harness/providers.h);
// this header exists for the implementation + tests. See loop_common.cpp for
// the design notes (Stage 2 phase 5).
#include "nimbus/harness/providers.h"

namespace agent {
namespace providers {
using FabricNotify = std::function<void(const std::string& fromHost,
                                        const std::string& toHost)>;

// The note a round gets when it runs again after a cut (model-facing). `why` is the
// cut's error line; `partial` what the model wrote before it (quoted back in part,
// brackets turned to parentheses so it cannot pose as a block; "" = nothing).
std::string cutRetryNote(const std::string& why, const std::string& partial);
}  // namespace providers
}  // namespace agent
