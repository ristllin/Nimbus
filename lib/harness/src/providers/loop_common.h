#pragma once
// runFabricLoop - declared on the public surface (nimbus/harness/providers.h);
// this header exists for the implementation + tests. See loop_common.cpp for
// the design notes (Stage 2 phase 5).
#include "nimbus/harness/providers.h"

namespace agent {
namespace providers {
using FabricNotify = std::function<void(const std::string& fromHost,
                                        const std::string& toHost)>;
}  // namespace providers
}  // namespace agent
