#pragma once
#include "nimbus/harness/config.h"

// Device implementation of the harness's HarnessConfig contract, wrapping the
// NVS-backed agent::store:: accessors. Built once at orchestrator::begin();
// the closures capture nothing (store:: is process-global), so the table is a
// few dozen std::function slots of steady-state RAM.
namespace agent {

// The one place the harness's config view is assembled from device NVS.
HarnessConfig harnessConfigFromStore();

}  // namespace agent
