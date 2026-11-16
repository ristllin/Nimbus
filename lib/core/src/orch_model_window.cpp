#include "nimbus/orch/model_window.h"

#include <algorithm>
#include <cctype>

#include "nimbus/orch/model_catalog.h"  // gptGeneration - the one gpt-<N> parser
#include "nimbus/orch/router_route.h"   // resolveRouterRoute - the one selector split

namespace nimbus {
namespace orch {

namespace {
bool startsWith(const std::string& s, const char* p) { return s.rfind(p, 0) == 0; }

std::string lower(const std::string& s) {
  std::string o = s;
  std::transform(o.begin(), o.end(), o.begin(),
                 [](unsigned char c) { return (char)std::tolower(c); });
  return o;
}

// The upstreams the Cumulo router serves (its "<upstream>/<model>" prefixes).
bool isRouterUpstream(const std::string& up) {
  return up == "openai" || up == "anthropic" || up == "mistral" || up == "zai";
}

// The family table proper, on a lowercased (provider, bare model) pair.
uint32_t familyCtxTokens(const std::string& p, const std::string& m) {
  // Family prefixes over exact ids: a new snapshot ("claude-sonnet-4-7") gets its
  // family's window without a table edit; a genuinely unknown family compacts
  // early on the conservative default, never late.
  if (p == "anthropic" || startsWith(m, "claude-")) return 200000;
  // OpenAI generations: the INPUT window (published context minus the max output
  // reserve, the same convention LiteLLM's max_input_tokens uses). gpt-5.x is
  // 400K - 128K; gpt-6-astra (2026-09) is 1,050,000 - 128,000. A later generation
  // inherits the gpt-6 figure: windows have only grown, and the owner cap
  // (compactAtK) bounds the tail long before either window matters.
  const int gen = gptGeneration(m);
  if (gen >= 6) return 922000;
  if (gen == 5) return 272000;
  if (startsWith(m, "gpt-4o")) return 128000;   // gpt-4o / gpt-4o-mini (the pre-2026-09 Cumulo default)
  if (m.size() >= 2 && m[0] == 'o' && m[1] >= '1' && m[1] <= '9') return 200000;
  if (p == "mistral" || startsWith(m, "mistral-")) return 128000;
  return kCtxDefaultTokens;
}
}  // namespace

uint32_t modelCtxTokens(const std::string& provider, const std::string& model) {
  const std::string m = lower(model);
  const std::string p = lower(provider);
  // CUM-530: a router selector names its upstream ("openai/gpt-5.6" is what the
  // Cumulo picker stores, even for the default). Split it with the ONE router
  // rule and read the upstream's window for the bare id; matching the raw
  // selector dropped the router head to the 100K default. A bare id on the router
  // keeps the default upstream (openai), so "gpt-5.6" reads exactly as before.
  // Another provider's id is split only when it names a router upstream: a
  // namespaced id on a LAN server ("mistralai/Mistral-7B-...", 32K) must stay on
  // the conservative default, never inherit a bigger family window.
  if (p == "cumulo") {
    const RouterRoute r = resolveRouterRoute(m);
    return familyCtxTokens(r.upstream, r.model);
  }
  const std::string::size_type slash = m.find('/');
  if (slash != std::string::npos && slash > 0 && isRouterUpstream(m.substr(0, slash)))
    return familyCtxTokens(m.substr(0, slash), m.substr(slash + 1));
  return familyCtxTokens(p, m);
}

uint32_t compactThreshold(uint32_t ctxTokens, uint32_t reservedOutTokens,
                          uint32_t ownerCapTokens) {
  if (ctxTokens == 0) ctxTokens = kCtxDefaultTokens;
  uint32_t reserved = reservedOutTokens + kCompactBufferTokens;
  uint32_t byWindow = ctxTokens > reserved ? ctxTokens - reserved : kCompactMinThreshold;
  uint32_t t = ownerCapTokens > 0 ? std::min(byWindow, ownerCapTokens) : byWindow;
  return std::max(t, kCompactMinThreshold);
}

}  // namespace orch
}  // namespace nimbus
