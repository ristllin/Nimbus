#pragma once
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "nimbus/orch/episodic.h"
#include "nimbus/orch/mem_config.h"
#include "nimbus/orch/scratchpad.h"
#include "nimbus/orch/tool_registry.h"
#include "nimbus/orch/vector_archive.h"
#include "nimbus/orch/vector_memory.h"

// memory_tools - the `memory.*` MCP tool surface, wiring the portable memory
// engines (VectorMemory + Scratchpad + MemConfig) into ToolRegistry handlers:
// memory_write / memory_search + memory_config + a scratchpad tool for the
// goal tiers.
//
// This is the integration seam that makes the on-device MCP server real: an LLM
// (via the function-calling bridge) or an external MCP client calls
// memory.write / memory.search / memory.config / memory.scratchpad and it
// actually mutates the associative memory + working memory. The ONE thing that
// needs the device is the embedder (text -> vector via the provider /embeddings
// API); it is injected as a std::function, so host tests supply a deterministic
// fake and exercise the entire path (RPC -> tool -> engine) with no network.
//
// Everything here is Arduino-free and host-tested (pio test -e native).
namespace nimbus {
namespace orch {

// Text -> quantized embedding. The device binds this to the provider embeddings
// call (float result -> VectorMemory::quantize); tests bind a fake. An empty
// return signals "embedding unavailable" and the write/search tool reports it.
using Embedder = std::function<std::vector<int8_t>(const std::string& text)>;

// Borrowed engines + seams the memory tools operate on. All pointers must
// outlive the registry the tools are added to.
struct MemoryContext {
  VectorMemory*  vec = nullptr;
  Scratchpad*    scratch = nullptr;
  MemConfig*     cfg = nullptr;
  EpisodicStore* episodic = nullptr;         // optional: enables memory.episodic
  // Cold store for TTL-expired memories (CUM-225). Bound ONLY when the archive
  // exists, which the device does only when an SD card is present - so memory.archive
  // is registered (exposed) only with a card. `archiveAvailable` is a live check the
  // handler consults so a card pulled mid-run refuses cleanly (the tool stays
  // registered, but every action reports the archive is gone).
  VectorArchive* archive = nullptr;          // optional: enables memory.archive
  std::function<bool()> archiveAvailable = [] { return true; };
  Embedder       embed;                      // required for write/search
  std::function<uint32_t()> nowHours = [] { return 0u; };  // clock for TTL stamping
};

// Register memory.write / memory.search / memory.config / memory.scratchpad on
// `reg`, backed by `ctx`. `ctx` is captured by value (it holds pointers +
// std::functions), so the engines it points at must outlive `reg`.
void registerMemoryTools(ToolRegistry& reg, const MemoryContext& ctx);

}  // namespace orch
}  // namespace nimbus
