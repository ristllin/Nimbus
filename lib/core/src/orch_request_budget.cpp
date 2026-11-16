#include "nimbus/orch/request_budget.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <mutex>

#include "nimbus/orch/model_price_defaults.h"   // builtinModelPrice - the price tier (longOver)
#include "nimbus/orch/model_window.h"           // headWindowTokens, legacyCtxTokens
#include "nimbus/orch/router_route.h"           // resolveRouterRoute - a selector's upstream

namespace nimbus {
namespace orch {

namespace {

bool startsWith(const std::string& s, const char* p) { return s.rfind(p, 0) == 0; }

std::string lower(const std::string& s) {
  std::string o = s;
  std::transform(o.begin(), o.end(), o.begin(), [](unsigned char c) { return (char)std::tolower(c); });
  return o;
}

bool isRouterUpstream(const std::string& up) {
  return up == "openai" || up == "anthropic" || up == "mistral" || up == "zai";
}

// The wire a request goes out on and the bare model it names there. Host "cumulo" reads
// its selector's upstream (and is routed); a direct host's id drops only its OWN prefix
// ("mistral/mistral-large-4-0" on mistral, what a router pick stores). Any other id is the
// server's own: a custom endpoint serving "openai/gpt-oss-120b" stays custom.
struct WireKey {
  std::string host;
  std::string model;
  bool cumulo = false;
};
WireKey wireKey(const std::string& host, const std::string& model) {
  const std::string m = lower(model);
  const std::string h = lower(host);
  if (h == "cumulo") {
    const RouterRoute r = resolveRouterRoute(m);
    return {r.upstream, r.model, true};
  }
  const std::string::size_type slash = m.find('/');
  if (slash != std::string::npos && isRouterUpstream(h) && m.compare(0, slash, h) == 0)
    return {h, m.substr(slash + 1), false};
  return {h, m, false};
}

// ---- model families ------------------------------------------------------------------
bool isGlm(const std::string& m) { return startsWith(m, "glm-") || startsWith(m, "zai-glm"); }
bool isLarge4(const std::string& m) { return startsWith(m, "mistral-large-4"); }
// Mistral Medium and Small and their aliases (the 130 -> 70 tok/s, 2,500 tok/s prefill class).
bool isMistralMidSmall(const std::string& m) {
  for (const char* p : {"mistral-medium", "mistral-small", "magistral-", "ministral-", "mistral-vibe-cli"})
    if (startsWith(m, p)) return true;
  return false;
}
// The Mistral heads that refuse effort "none" (output_caps mistralHeadEffort "low";
// test_output_caps pins the two agree over every recorded Mistral id).
bool forcedReasonerOnMistral(const std::string& m) { return isLarge4(m) || isGlm(m); }

// A Claude id's line ("sonnet", "haiku", ...), "" for anything else.
std::string claudeLine(const std::string& m) {
  if (!startsWith(m, "claude-")) return "";
  size_t i = 7;
  while (i < m.size() && std::isalpha((unsigned char)m[i])) i++;
  return m.substr(7, i - 7);
}

// A context-dependent floor: `lo` at or under 32k tokens, `hi` at 64k and above, linear
// between and rounded down (Medium and Small 130 -> 70, GLM 55 -> 48; plan v3 caps
// protocol).
uint32_t byContext(uint32_t ctx, uint32_t lo, uint32_t hi) {
  constexpr uint32_t kFrom = 32000, kTo = 64000;
  if (ctx <= kFrom) return lo;
  if (ctx >= kTo) return hi;
  const uint64_t drop = ((uint64_t)(lo - hi) * (ctx - kFrom) + (kTo - kFrom) - 1) / (kTo - kFrom);
  return lo - (uint32_t)drop;
}

uint32_t anthropicDecode(const std::string& m) {
  const std::string line = claudeLine(m);
  if (line == "haiku") return 120;
  if (line == "sonnet") return 75;   // measured 89.6 tok/s on a 5,175-token reply (15 Nov)
  if (line == "opus") return 50;
  return 40;   // fable, mythos, unknown: the slowest tier
}

uint32_t mistralDecode(const std::string& m, uint32_t ctx) {
  if (isGlm(m)) return byContext(ctx, 55, 48);
  if (isLarge4(m)) return 50;   // 15 Nov: 50-64 tok/s at 100-150k, no context trend
  if (isMistralMidSmall(m)) return byContext(ctx, 130, 70);
  return 55;   // any other Mistral id: v5.1.1's floor
}

// GLM's tokenizer on the wires that serve GLM (Z.ai, and Mistral's hosted GLM).
bool glmWire(const WireKey& k) { return k.host == "zai" || (k.host == "mistral" && isGlm(k.model)); }

uint32_t ratioNomX100(const WireKey& k) {
  if (k.host == "anthropic") return 250;
  if (k.host == "openai") return 390;
  if (glmWire(k)) return 330;
  if (k.host == "mistral") return isLarge4(k.model) ? 360 : 310;
  return 300;   // custom, unknown
}
uint32_t ratioSafeX100(const WireKey& k) { return k.host == "anthropic" ? 200 : 250; }

// The socket deadline every head round has (providers.h kMistralRoundMs, kZaiHeadTimeoutMs,
// kHeadRoundCeilingMs; test_engine_seams pins them equal).
uint32_t socketMs(const std::string& host) {
  if (host == "mistral") return 65000;
  if (host == "zai") return 50000;
  return 120000;
}

// The v5.1.1 replay cap of each wire (mistral.cpp, openai.cpp, custom.cpp 24,576;
// anthropic.cpp 16,384).
uint32_t v511Replay(const std::string& host) { return host == "anthropic" ? 16384 : 24576; }

// Heads whose live leg (one forced long final at the target) failed: they keep v5.1.1's
// 85,000 B in this release (RULINGS-v3), with the measurement recorded beside the row.
struct HeldHead {
  const char* host;
  const char* modelPrefix;
};
constexpr HeldHead kHeldAtV511[] = {
    {"", ""},   // none: every PL leg passed (lane B, 16 Nov 2026)
};
bool heldAtV511(const WireKey& k) {
  for (const HeldHead& h : kHeldAtV511)
    if (*h.host && k.host == h.host && startsWith(k.model, h.modelPrefix)) return true;
  return false;
}

std::atomic<BudgetLink> g_link{BudgetLink::Device};

// ---- the memo ------------------------------------------------------------------------
struct Memo {
  std::string host, model;
  uint16_t ratioX100 = 0;
};
constexpr size_t kMemoSlots = 16;
constexpr uint32_t kMemoMinPromptTokens = 2048;
constexpr uint16_t kMemoFloorX100 = 150;
std::mutex g_memoMx;
Memo g_memo[kMemoSlots];

uint16_t memoFor(const WireKey& k) {
  std::lock_guard<std::mutex> lk(g_memoMx);
  for (const Memo& m : g_memo)
    if (m.ratioX100 && m.host == k.host && m.model == k.model) return m.ratioX100;
  return 0;
}

// ---- the latency bound -----------------------------------------------------------------
uint32_t decodeFor(const WireKey& k, uint32_t ctx) {
  if (k.host == "anthropic") return anthropicDecode(k.model);
  if (k.host == "mistral") return mistralDecode(k.model, ctx);
  if (k.host == "zai") return byContext(ctx, 55, 48);
  return 50;   // OpenAI and custom: never capped, the target check only
}

uint64_t timeAtMs(const RequestBudget& b, const WireKey& k, uint32_t tokens) {
  const uint64_t bytes = (uint64_t)tokens * b.rNomX100 / 100;
  return roundOverheadMs(b.link, (size_t)bytes, true, tokens, b.prefillTokPerS) +
         (uint64_t)b.minFinalTokens * 1000 / decodeFor(k, tokens);
}

// The largest T (1k steps, up to the clamp) whose worst round fits the read window; 0 when
// none does. The round time only grows with T, so the walk stops at the first step over.
uint32_t latencyBound(const RequestBudget& b, const WireKey& k) {
  constexpr uint32_t kStep = 1000;
  uint32_t fit = 0;
  for (uint32_t t = kStep; t <= kBudgetTargetClampTokens && timeAtMs(b, k, t) <= b.readWindowMs; t += kStep) fit = t;
  return fit;
}

uint64_t atRatio(uint64_t tokens, uint32_t x100) { return tokens * x100 / 100; }

// The rates and windows of the head, before the bytes.
RequestBudget rates(const std::string& host, const std::string& model, const WireKey& k, bool routed,
                    BudgetLink link) {
  RequestBudget b;
  b.routed = routed;
  b.link = link;
  b.flat = k.host == "custom";
  // The window reads the ids as the window table does (a selector on any host names its
  // upstream's window, as in v5.1.1); the rates and the floor read the wire.
  b.windowTokens = headWindowTokens(host, model);
  b.v511CtxTokens = legacyCtxTokens(host, model);
  b.v511ReplayBytes = v511Replay(k.host);
  b.rNomX100 = (uint16_t)ratioNomX100(k);
  b.rSafeX100 = (uint16_t)ratioSafeX100(k);
  b.roundMs = routed ? std::min(socketMs(k.host), kRoutedReadWindowMs) : socketMs(k.host);
  b.readWindowMs = readWindowMsFor(k.host, k.model, routed);
  b.prefillTokPerS = prefillFloorTokPerS(k.host, k.model);
  b.minFinalTokens = minFinalTokensFor(k.host, k.model);
  const BuiltinPrice* price = builtinModelPrice(k.host.c_str(), k.model.c_str());
  b.tierTokens = price ? price->longOver : 0;
  b.floorBytes = (uint32_t)std::min<uint64_t>(kLegacyWireBodyBytes, atRatio(b.windowTokens, b.rSafeX100) * 85 / 100);
  return b;
}

// The bytes the bounds allow, and which bound set them.
void bound(RequestBudget& b, const WireKey& k) {
  const uint16_t memo = memoFor(k);
  const uint32_t dense = memo && memo < b.rSafeX100 ? memo : b.rSafeX100;
  b.latencyTokens = latencyBound(b, k);
  struct Cand {
    uint64_t bytes;
    BudgetBound why;
  };
  const Cand cands[] = {
      {atRatio(kBudgetTargetClampTokens, b.rNomX100), BudgetBound::Clamp},
      {atRatio(b.latencyTokens, b.rNomX100), BudgetBound::Latency},
      {atRatio(b.windowTokens, dense) * 3 / 4, BudgetBound::Window},
      {b.tierTokens ? atRatio(b.tierTokens, dense) * 9 / 10 : UINT64_MAX, BudgetBound::Tier},
  };
  Cand best = cands[0];
  for (const Cand& c : cands)
    if (c.bytes < best.bytes) best = c;
  b.wireBodyBytes = (uint32_t)(best.bytes / 1000 * 1000);
  b.bound = best.why;
}

}  // namespace

void installBudgetLink(BudgetLink link) { g_link.store(link); }
BudgetLink budgetLink() { return g_link.load(); }

const char* budgetBoundSlug(BudgetBound b) {
  switch (b) {
    case BudgetBound::Clamp:   return "clamp";
    case BudgetBound::Latency: return "latency";
    case BudgetBound::Window:  return "window";
    case BudgetBound::Tier:    return "tier";
    case BudgetBound::Floor:   return "floor";
    case BudgetBound::Held:    return "held";
  }
  return "?";
}

uint32_t prefillFloorTokPerS(const std::string& host, const std::string& model) {
  const WireKey k = wireKey(host, model);
  if (k.host == "anthropic") {
    const std::string line = claudeLine(k.model);
    return line == "sonnet" || line == "haiku" ? 60000 : 30000;
  }
  if (k.host == "openai") return 25000;
  if (glmWire(k)) return 9000;
  if (k.host == "mistral") return isLarge4(k.model) ? 9000 : 2500;
  return 1000;   // custom: an unknown server
}

uint32_t decodeFloorTokPerS(const std::string& host, const std::string& model, uint32_t contextTokens) {
  return decodeFor(wireKey(host, model), contextTokens);
}

uint32_t minFinalTokensFor(const std::string& host, const std::string& model) {
  const WireKey k = wireKey(host, model);
  if (k.host == "zai") return 1600;
  if (k.host == "mistral" && forcedReasonerOnMistral(k.model)) return 1500;
  return 3000;
}

uint32_t readWindowMsFor(const std::string& host, const std::string& model, bool routed) {
  const WireKey k = wireKey(host, model);
  uint32_t w = 120000;
  if (k.host == "mistral") w = 60000;   // Mistral's gateway closes a request at 60 s
  if (k.host == "zai") w = 50000;
  return routed || k.cumulo ? std::min(w, kRoutedReadWindowMs) : w;
}

uint32_t predictedPaceMs(size_t bodyBytes) {
  const uint64_t slices = (bodyBytes + kBudgetPaceSliceBytes - 1) / kBudgetPaceSliceBytes;
  return (uint32_t)std::min<uint64_t>(kBudgetPaceBaseMs + slices * kBudgetPaceSliceMs, UINT32_MAX);
}

uint32_t roundOverheadMs(BudgetLink link, size_t bodyBytes, bool fragmented, uint32_t uncachedTokens,
                         uint32_t prefillTokPerS, uint8_t uploads) {
  uint64_t ms = kBudgetConnectMs;
  if (link == BudgetLink::Device) {
    const uint64_t up = (uint64_t)bodyBytes * 1000 / kBudgetUploadBytesPerS + (fragmented ? predictedPaceMs(bodyBytes) : 0);
    ms += up * (uploads ? uploads : 1);
  }
  if (prefillTokPerS) ms += (uint64_t)uncachedTokens * 1000 / prefillTokPerS;
  return (uint32_t)std::min<uint64_t>(ms, UINT32_MAX);
}

RequestBudget requestBudgetFor(const std::string& host, const std::string& model, bool routed) {
  return requestBudgetFor(host, model, routed, budgetLink());
}

RequestBudget requestBudgetFor(const std::string& host, const std::string& model, bool routed, BudgetLink link) {
  return requestBudgetFor(host, model, routed, link, kRoutedBudgetLift);
}

RequestBudget requestBudgetFor(const std::string& host, const std::string& model, bool routed, BudgetLink link,
                               bool routedLift) {
  const WireKey k = wireKey(host, model);
  routed = routed || k.cumulo;
  RequestBudget b = rates(host, model, k, routed, link);
  bound(b, k);
  if (b.flat || (routed && !routedLift) || b.wireBodyBytes < b.floorBytes) {
    b.wireBodyBytes = b.floorBytes;
    b.bound = BudgetBound::Floor;
  }
  if (heldAtV511(k)) {
    b.wireBodyBytes = kLegacyWireBodyBytes;
    b.bound = BudgetBound::Held;
  }
  b.targetTokens = (uint32_t)((uint64_t)b.wireBodyBytes * 100 / b.rNomX100);
  b.decodeTokPerS = decodeFor(k, b.targetTokens);
  return b;
}

void noteObservedRatio(const std::string& host, const std::string& model, size_t textBytes, uint32_t promptTokens) {
  if (promptTokens < kMemoMinPromptTokens || !textBytes) return;
  const WireKey k = wireKey(host, model);
  const uint64_t r = (uint64_t)textBytes * 100 / promptTokens;
  const uint16_t x100 = (uint16_t)std::max<uint64_t>(kMemoFloorX100, std::min<uint64_t>(r, UINT16_MAX));
  std::lock_guard<std::mutex> lk(g_memoMx);
  Memo* free = nullptr;
  for (Memo& m : g_memo) {
    if (!m.ratioX100) {
      if (!free) free = &m;
      continue;
    }
    if (m.host != k.host || m.model != k.model) continue;
    if (x100 < m.ratioX100) m.ratioX100 = x100;   // tighten only
    return;
  }
  // A new head is remembered only while a slot is free: evicting one would loosen it.
  if (free && x100 < ratioSafeX100(k)) *free = Memo{k.host, k.model, x100};
}

uint16_t observedRatioX100(const std::string& host, const std::string& model) { return memoFor(wireKey(host, model)); }

void clearObservedRatios() {
  std::lock_guard<std::mutex> lk(g_memoMx);
  for (Memo& m : g_memo) m = Memo{};
}

}  // namespace orch
}  // namespace nimbus
