#pragma once
#include "../adapter.h"

// adapter_factory - wires adapters into the HeavyFabric registry and binds
// categories from config. Ported from Nuage-Solide src/agent/adapters/
// adapter_factory.h. Called once when Orchestrator mode starts (after store /
// solide::memory is up). Zero core-loop changes to add backends.
namespace agent {

void fabricInit(HeavyFabric& fabric);

// Provider accent HUE (0-254 on the colour wheel; 255 = white / unknown) for a
// backend id. This maps DIRECTLY onto nimbus::attn::Event.accentHue - the Nimbus
// Router + ring_plan consume HUES, not RGB (the Nuage-Solide original returned
// RGB for its own LED renderer). Keeps provider knowledge in the adapter layer,
// out of the orchestrator / main.cpp.
uint8_t backendHue(const char* backend);

}  // namespace agent
