#pragma once
#include <cstdint>
#include <functional>

#include "nimbus/orch/tool_registry.h"

// tool_gates - the provider-gated tools, ONE table (CUM-508).
//
// The bug class this closes: a tool whose prerequisite is a provider key was
// registered only if the key existed AT BOOT (web.search checked the Tavily key
// once in memory::begin). A key added later showed "verified" in the web app,
// yet the tool never appeared until a restart, and the assistant told the owner
// it could not search the web. The rule now: a provider-gated tool is registered
// ONCE, unconditionally, and gated LIVE through ToolRegistry::setGate - listed
// (model, /api/tools, MCP tools/list) exactly while its key is present, refused
// honestly while it is not, and available on the very next turn after the key
// is saved. No restart.
//
// Every provider-gated tool is a row here, and the device opens each gate from
// one switch over ToolGate; test/test_tool_gates walks EVERY row (a key added
// after begin() lists the tool; a key removed hides it again). Hardware-gated
// tools (memory.archive needs a card present at boot) are out of scope: a card
// inserted later needs a restart by design (memory_subsystem.cpp).
namespace nimbus {
namespace orch {

enum class ToolGate : uint8_t {
  WebSearchKey,  // a Tavily key is configured
  ImageKey,      // an image provider key (OpenAI) is configured
};

struct GatedTool {
  const char* tool;     // registry name
  ToolGate    gate;     // what must be present
  const char* refusal;  // model-facing reason when called with the gate closed
};

inline constexpr GatedTool kGatedTools[] = {
    {"web.search", ToolGate::WebSearchKey,
     "web search is not set up on this device (no web-search key); the owner can "
     "add a Tavily key in the web app"},
    {"image.generate", ToolGate::ImageKey,
     "image generation is not set up on this device (no OpenAI key); the owner can "
     "add one in the web app"},
};

// Install the live gate for every row: `open(gate)` is evaluated on EVERY
// listing and call, never cached, so a key saved after boot counts at once.
// Order-independent: rows whose tool registers later are gated all the same.
inline void applyToolGates(ToolRegistry& reg, const std::function<bool(ToolGate)>& open) {
  for (const GatedTool& g : kGatedTools) {
    const ToolGate gate = g.gate;
    reg.setGate(g.tool, [open, gate] { return open && open(gate); }, g.refusal);
  }
}

}  // namespace orch
}  // namespace nimbus
