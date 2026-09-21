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
// return signals "embedding unavailable"; the callee writes the REAL cause into
// `err` (the device seam's `embeddings::embedWith` err token) so the tool can
// report why instead of a fixed guess (CUM-435). `err` is left empty on success.
using Embedder =
    std::function<std::vector<int8_t>(const std::string& text, std::string& err)>;

// ---- honest embedding-failure reasons (CUM-435) -----------------------------
// The memory tools used to collapse every embedding failure to one fixed string
// ("embedding unavailable (provider offline?)"), hiding the real cause from the
// model and the owner. These map the device seam's raw err token to an honest,
// secret-safe class + words, the same honest-refusal shape as voice_route.
//
// A class per real failure mode; a new class with no words fails the host test
// that iterates this enum, so the mapping cannot silently rot.
enum class EmbedFail {
  None = 0,         // an embedding was produced (no failure)
  NoKey,            // no embeddings key for the configured provider
  KeyRejected,      // provider returned 401/403
  Busy,             // the single on-device TLS slot was in use (retryable)
  Unreachable,      // could not connect to the provider host
  Timeout,          // the provider did not answer in time
  OutOfCredit,      // router refusal: funding_cap_reached
  RateLimited,      // router refusal: rate_limited
  RouteNotAllowed,  // router refusal: endpoint_not_allowed
  ProviderError,    // any other non-200 HTTP status
  BadResponse,      // a 200 whose body did not parse as an embedding
  NoModel,          // no embeddings model configured
  BadRequest,       // nothing to embed (empty text)
  Unknown           // an unmapped token (generic line)
};

// Classify a raw failure token from `agent::embeddings::embedWith` err. Pure +
// host-tested. `detail` receives ONLY a bounded, secret-safe extra (a KNOWN
// provider slug for NoKey, or the numeric HTTP status for ProviderError) or is
// left empty - it NEVER echoes arbitrary raw text, so a key-shaped string in
// `raw` can never reach the tool result.
EmbedFail classifyEmbedFail(const std::string& raw, std::string& detail);

// Concise, honest cause phrase for a failure class (no "embeddings:" prefix, no
// trailing period), AGENTS section 6 style: US English, no em dash, states what
// happened. `detail` from classifyEmbedFail is folded in where the class carries
// one. Used inside the search fallback label. Pure + host-tested.
std::string embedFailWords(EmbedFail kind, const std::string& detail);

// Convenience: classify `raw` then format "embeddings: <words>[; next step]" for
// a tool that must refuse (write/update). Pure + host-tested.
std::string embedFailReason(const std::string& raw);

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
