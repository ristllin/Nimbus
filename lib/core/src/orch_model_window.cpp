#include "nimbus/orch/model_window.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <utility>

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
  return headWindowTokens(provider, model);   // lane H's switch-over: the head's real window
}

uint32_t legacyCtxTokens(const std::string& provider, const std::string& model) {
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

namespace {
std::atomic<ListedCtxLookup> g_listedCtx{nullptr};
}  // namespace

void installListedCtxLookup(ListedCtxLookup f) { g_listedCtx.store(f); }

uint32_t listedWindowTokens(const std::string& host, const std::string& model) {
  const ListedCtxLookup f = g_listedCtx.load();
  return f ? f(host.c_str(), model.c_str()) : 0;
}

namespace {

// A Claude id's version as major*10 + minor ("claude-sonnet-4-6" 46, "claude-opus-5" 50,
// "claude-opus-4-5-20251101" 45); 0 for the old "claude-3-5-haiku" shape and anything
// else (a date suffix is not a minor).
int claudeVersion(const std::string& m) {
  if (!startsWith(m, "claude-")) return 0;
  size_t i = 7;
  while (i < m.size() && std::isalpha((unsigned char)m[i])) i++;
  if (i == 7 || i + 1 >= m.size() || m[i] != '-' || !std::isdigit((unsigned char)m[i + 1])) return 0;
  const int major = m[i + 1] - '0';
  int minor = 0;
  const size_t j = i + 2;
  const bool shortGroup = j + 1 < m.size() && m[j] == '-' && std::isdigit((unsigned char)m[j + 1]) &&
                          (j + 2 >= m.size() || !std::isdigit((unsigned char)m[j + 2]));
  if (shortGroup) minor = m[j + 1] - '0';
  return major * 10 + minor;
}

// The first prefix of `rows` that `m` starts with decides (longest first where they nest).
struct PrefixWindow {
  const char* prefix;
  uint32_t window;
};
uint32_t prefixWindow(const std::string& m, const PrefixWindow* rows, size_t n, uint32_t otherwise) {
  for (size_t i = 0; i < n; i++)
    if (startsWith(m, rows[i].prefix)) return rows[i].window;
  return otherwise;
}

// GLM: glm-5 and later 1M total less the 128K output reserve; glm-4.x 128K.
uint32_t glmWindow(const std::string& m) {
  if (startsWith(m, "zai-glm")) return 917504;
  const size_t at = startsWith(m, "glm-") ? 4 : std::string::npos;
  if (at == std::string::npos || at >= m.size() || !std::isdigit((unsigned char)m[at])) return 0;
  return m[at] >= '5' ? 917504 : 128000;
}

// Mistral's ids, recorded 2026-10 and 2026-11 (every row under its provider-listed
// window; test_model_window pins it).
constexpr PrefixWindow kMistralRows[] = {
    {"mistral-large-4", 1048576},
    {"mistral-large-2512", 262144},
    {"mistral-large-latest", 262144},
    {"mistral-medium", 262144},
    {"mistral-small", 262144},
    {"magistral-", 262144},
    {"mistral-vibe-cli", 262144},
    {"ministral-3b", 131072},
    {"ministral-", 262144},
    {"devstral", 262144},
    {"labs-leanstral", 262144},
    {"codestral-embed", 8192},
    {"codestral", 256000},
    {"mistral-code", 256000},
    {"voxtral-mini-tts", 4096},
    {"voxtral-mini-transcribe-realtime", 32768},
    {"voxtral-mini-realtime", 32768},
    {"voxtral-mini", 16384},
    {"voxtral", 32768},
    {"mistral-ocr", 16384},
    {"mistral-embed", 8192},
    {"mistral-moderation", 131072},
};

// The corrected table on a lowercased (upstream or host, bare model) pair.
uint32_t familyWindow(const std::string& p, const std::string& m) {
  if (p == "anthropic" || startsWith(m, "claude-")) return claudeVersion(m) >= 46 ? 1000000 : 200000;
  if (const uint32_t glm = glmWindow(m)) return glm;
  if (p == "mistral" || startsWith(m, "mistral-"))
    return prefixWindow(m, kMistralRows, sizeof(kMistralRows) / sizeof(kMistralRows[0]), 128000);
  return familyCtxTokens(p, m);   // OpenAI and everything else: v5.1.1's figures
}

// (upstream, bare model) of a head: a router selector on cumulo splits by the router
// rule; another host's id splits only when its prefix names a router upstream.
std::pair<std::string, std::string> windowKey(const std::string& host, const std::string& model) {
  const std::string m = lower(model);
  const std::string p = lower(host);
  if (p == "cumulo") {
    const RouterRoute r = resolveRouterRoute(m);
    return {r.upstream, r.model};
  }
  const std::string::size_type slash = m.find('/');
  if (slash != std::string::npos && slash > 0 && isRouterUpstream(m.substr(0, slash)))
    return {m.substr(0, slash), m.substr(slash + 1)};
  return {p, m};
}

}  // namespace

uint32_t tableWindowTokens(const std::string& host, const std::string& model) {
  const auto k = windowKey(host, model);
  return familyWindow(k.first, k.second);
}

uint32_t headWindowTokens(const std::string& host, const std::string& model) {
  const auto k = windowKey(host, model);
  const uint32_t listed = listedWindowTokens(k.first, k.second);
  return listed ? listed : familyWindow(k.first, k.second);
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
