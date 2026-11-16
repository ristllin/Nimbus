#pragma once
// wire_budget - each request's own budget, its load and the caps that fit it (v5.1.2).
// INTERNAL to the provider files, like wire.h.
//
// The host and model that SEND a request decide its budget (request_budget.h): every
// round reads it at round start, never a turn-global value, so a round the fabric moves
// to a host with a smaller budget is refused locally before anything is sent. The round
// then builds its body, measures what it really carries (RoundLoad: the text, the photo
// base64 spliced in at send time, the prompt's tokens, routed or not, the heap's state)
// and sends the output cap that fits that load inside its read window
// (output_caps.h outputCapFor(..., RoundLoad)). A photo round that cannot carry the
// host's minimum final with its photos is refused with the photo-fit wording: the loop
// gives up older history first and reads the photos as text only when no older history
// is left (loop_common.cpp RoundRecovery::photoFit).
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>

#include "nimbus/harness/http.h"
#include "nimbus/harness/log.h"
#include "nimbus/harness/output_caps.h"
#include "nimbus/harness/providers.h"
#include "nimbus/orch/budget.h"           // headContextBudget - the replay floor (the derived replay at B)
#include "nimbus/orch/request_budget.h"
#include "nimbus/orch/token_usage_json.h"   // tokenUsageFromJson - a round's usage

namespace agent {
namespace providers {
namespace wire {

// ---- one round's budget ------------------------------------------------------------
struct RoundBudget {
  std::string host;     // the budget's host: the wire's own provider (the upstream when routed)
  std::string model;    // the model the request names
  bool routed = false;  // through the Cumulo router
  nimbus::orch::RequestBudget budget;
  bool fragmented = false;   // the DMA pool was under kDmaChunkFloor at round start
};

// A deps bundle whose requests travel through the Cumulo router (wire.h applyRouter).
inline bool routedDeps(const ProviderDeps& pd) { return pd.routerBase && !pd.routerBase().empty(); }

// The transport paces its body slices while the DMA-capable pool is under
// kDmaChunkFloor (transport_tls.cpp paceForHeadroom); a round that starts there is
// fragmented and its time fit counts the pacing.
inline bool fragmentedNow(const ProviderDeps& pd) {
  return pd.dmaLargestBlock && pd.dmaLargestBlock() < kDmaChunkFloor;
}

inline RoundBudget roundBudget(const ProviderDeps& pd, const std::string& host, const std::string& model,
                               bool routed) {
  RoundBudget rb;
  rb.host = host;
  rb.model = model;
  rb.routed = routed;
  rb.budget = wireBudgetFor(host, model, routed);
  rb.fragmented = fragmentedNow(pd);
  return rb;
}
// The same, on the heap (lane STACK): a head round's step runs deep in a turn on the
// Telegram poll task, so its budget (two strings and the figures) stays off that stack.
__attribute__((noinline)) inline std::unique_ptr<const RoundBudget> newRoundBudget(const ProviderDeps& pd,
                                                                                  const std::string& host,
                                                                                  const std::string& model,
                                                                                  bool routed) {
  return std::unique_ptr<const RoundBudget>(new RoundBudget(roundBudget(pd, host, model, routed)));
}

// The budget host of the custom wire: "zai" on a deps bundle the Z.ai policy pointed at
// Z.ai (direct or the router's zai upstream), else "custom" (a LAN or proxy endpoint, or
// a router upstream the native fabric does not drive).
inline const char* customBudgetHost(const ProviderDeps& pd) {
  return pd.customBudgetHost && *pd.customBudgetHost ? pd.customBudgetHost : "custom";
}
// The custom wire travels through the router when its path prefix says so.
inline bool customRouted(const ProviderDeps& pd) {
  const std::string prefix = pd.customPathPrefix ? pd.customPathPrefix() : std::string();
  return prefix.rfind("/router/", 0) == 0;
}

// A round's socket ceiling: the host's own, and at most kRoutedRoundCeilingMs through the
// router (the egress gives up at 95 s; the device gives up first and fails over).
inline uint32_t headCeilingMs(bool routed, uint32_t hostCeilingMs) {
  return routed && hostCeilingMs > kRoutedRoundCeilingMs ? kRoutedRoundCeilingMs : hostCeilingMs;
}

// The replayed tool results a wire keeps (Transcript::trimToolOutputs): the derived
// replay, never under the wire's own v5.1.1 figure (no field shrinks).
inline size_t replayBytesFor(const RoundBudget& rb, size_t v511Bytes) {
  const size_t derived = nimbus::orch::headContextBudget(rb.host, rb.model, nimbus::orch::BudgetOverrides()).replayBytes;
  return std::max(v511Bytes, derived);
}

// ---- the round's load ----------------------------------------------------------------
// What one request carries: its text body, the photo base64 its splices add at send time
// and the photos' share of the window (image_parts.h photoTokens).
inline uint32_t textTokens(const RoundBudget& rb, size_t textBytes) {
  const uint32_t r = rb.budget.rNomX100 ? rb.budget.rNomX100 : 400;
  return (uint32_t)((uint64_t)textBytes * 100 / r);
}
inline RoundLoad roundLoad(const RoundBudget& rb, size_t textBytes, size_t imageBytes, uint32_t photoTokens) {
  RoundLoad l;
  l.bodyBytes = textBytes + imageBytes;
  l.imageBytes = imageBytes;
  l.promptTokens = textTokens(rb, textBytes) + photoTokens;
  l.routed = rb.routed;
  l.fragmented = rb.fragmented;
  return l;
}
inline size_t splicedBytes(const BodySplices& splices) {
  size_t n = 0;
  for (const BodySplice& s : splices) n += spliceLen(s);
  return n;
}

// What the newest round of each kind sent (single task by design, like lastExchange):
// the log and the host suites read it.
struct RoundRecord {
  std::string host, model;
  RoundKind kind = RoundKind::ToolRound;
  uint32_t roundMs = 0;
  RoundLoad load;
  OutputCap cap;
  size_t textBudget = 0;
  uint32_t photoTokens = 0;
};
inline RoundRecord& lastRound() {
  static RoundRecord r;
  return r;
}
inline void recordRound(const RoundBudget& rb, RoundKind kind, uint32_t roundMs, const RoundLoad& load,
                        const OutputCap& cap) {
  RoundRecord& r = lastRound();
  r.host = rb.host;
  r.model = rb.model;
  r.kind = kind;
  r.roundMs = roundMs;
  r.load = load;
  r.cap = cap;
  r.textBudget = rb.budget.wireBodyBytes;
  r.photoTokens = 0;
}

// ---- the cap that fits the load --------------------------------------------------------
// The body was built with the table's cap before its load was known. The load-aware cap
// replaces the first attempt's max_tokens in place (the request's first "max_tokens" key:
// the builders write it right after the model, so a tool schema's own field of that name
// never comes first); the photo splices after it move with it. False when the body has no
// such field to change (the caller builds it again).
inline bool replaceMaxTokens(std::string& body, BodySplices& splices, uint32_t from, uint32_t to) {
  if (from == to) return true;
  const std::string key = "\"max_tokens\":" + std::to_string(from);
  const size_t at = body.find("\"max_tokens\":");
  if (at == std::string::npos || body.compare(at, key.size(), key) != 0) return false;
  const size_t end = at + key.size();
  if (end >= body.size() || (body[end] != ',' && body[end] != '}')) return false;
  const std::string with = "\"max_tokens\":" + std::to_string(to);
  body.replace(at, key.size(), with);
  const long delta = (long)with.size() - (long)key.size();
  for (BodySplice& s : splices)
    if (s.at >= end) s.at = (size_t)((long)s.at + delta);
  return true;
}

// The round's cap from its load: `cap` is the table cap the body was built with; on
// return it is the load-aware cap, and true when the body already carries its first
// attempt (patched in place). False: the effort or the field's presence changed, and the
// caller builds the body again with `cap`.
inline bool fitCapToLoad(std::string& body, BodySplices& splices, OutputCap& cap, const RoundBudget& rb, RoundKind kind,
                         uint32_t roundMs, const RoundLoad& load) {
  const OutputCap fitted = outputCapFor(rb.host, rb.model, kind, roundMs, load);
  const bool sameShape = std::strcmp(fitted.effort, cap.effort) == 0 && (fitted.maxTokens == 0) == (cap.maxTokens == 0);
  const bool inPlace = sameShape && (cap.maxTokens == 0 || replaceMaxTokens(body, splices, cap.maxTokens, fitted.maxTokens));
  cap = fitted;
  recordRound(rb, kind, roundMs, load, fitted);
  return inPlace;
}

// ---- the time fit (interim: lane B's derivation replaces these at its merge) -----------
// The protocol's fit: the output tokens a round can produce inside its read window after
// the connect, the upload at 70 KB/s, the pacing a fragmented heap pays (2 s plus 100 ms
// per 4 KB slice, the slice wait the transport may spend) and the prefill, at the model's
// slowest decode rate. minFinal is the smallest final answer a round must have room for.
struct FitRates {
  uint32_t windowMs;
  uint32_t prefill;    // tokens per second
  uint32_t decode;     // tokens per second, at this context
  uint32_t minFinal;   // tokens
};
inline FitRates fitRates(const RoundBudget& rb, uint32_t promptTokens) {
  std::string m = rb.model;
  const size_t slash = m.find('/');
  if (slash != std::string::npos) m = m.substr(slash + 1);
  FitRates f{120000, 1000, 50, 3000};
  if (rb.host == "mistral") {
    f.windowMs = 60000;
    if (*sameHostNonReasoningModel("mistral", m)) {   // the forced reasoner (Large 4)
      f.prefill = 9000;
      f.decode = 50;
      f.minFinal = 1500;
    } else {
      f.prefill = 2500;
      f.decode = promptTokens < 32000 ? 130 : 70;
    }
  } else if (rb.host == "zai") {
    f.windowMs = 50000;
    f.prefill = 9000;
    f.decode = promptTokens < 32000 ? 55 : 48;
    f.minFinal = 1600;
  } else if (rb.host == "anthropic") {
    f.prefill = m.find("opus") != std::string::npos ? 30000 : 60000;
    f.decode = m.find("haiku") != std::string::npos ? 120 : m.find("opus") != std::string::npos ? 50 : 75;
  } else if (rb.host == "openai") {
    f.prefill = 25000;
  }
  if (rb.routed && f.windowMs > kRoutedRoundCeilingMs) f.windowMs = kRoutedRoundCeilingMs;
  return f;
}
inline uint32_t paceMsForBody(size_t bodyBytes) {
  const size_t slices = (bodyBytes + nimbus::heapwatch::kBodySliceBytes - 1) / nimbus::heapwatch::kBodySliceBytes;
  return kRequestPaceMaxMs + (uint32_t)slices * kSliceStallMs;
}
// The round's overhead before the first output token, in ms.
inline uint64_t overheadMs(const FitRates& f, const RoundLoad& load) {
  const uint64_t uploadMs = (uint64_t)load.bodyBytes * 1000 / 70000;
  const uint64_t paceMs = load.fragmented ? paceMsForBody(load.bodyBytes) : 0;
  const uint64_t prefillMs = (uint64_t)load.promptTokens * 1000 / (f.prefill ? f.prefill : 1);
  return 1500 + uploadMs + paceMs + prefillMs;
}
inline uint32_t roundFitTokens(const RoundBudget& rb, uint32_t roundMs, const RoundLoad& load) {
  const FitRates f = fitRates(rb, load.promptTokens);
  const uint64_t window = roundMs && roundMs < f.windowMs ? roundMs : f.windowMs;
  const uint64_t over = overheadMs(f, load);
  return over >= window ? 0 : (uint32_t)((window - over) * f.decode / 1000);
}
inline uint32_t minFinalTokens(const RoundBudget& rb) { return fitRates(rb, 0).minFinal; }

// The text bytes a photo round must give up for its fit to carry minFinal again: the
// token shortfall at the decode rate, in ms, over what one text byte costs in upload,
// pacing and prefill.
inline size_t photoFitShedBytes(const RoundBudget& rb, uint32_t roundMs, const RoundLoad& load) {
  const FitRates f = fitRates(rb, load.promptTokens);
  const uint32_t fit = roundFitTokens(rb, roundMs, load);
  const uint32_t want = minFinalTokens(rb);
  if (fit >= want) return 0;
  const uint64_t shortMs = (uint64_t)(want - fit) * 1000 / (f.decode ? f.decode : 1);
  // micro-seconds one text byte costs: 1/70 KB/s upload, 100 ms per 4 KB slice paced,
  // and its share of a token at the prefill rate
  const uint32_t r = rb.budget.rNomX100 ? rb.budget.rNomX100 : 400;
  const uint64_t perByteNs = 1000000000ull / 70000 + (load.fragmented ? 1000000000ull / 4096 / 10 : 0) +
                             1000000000ull * 100 / r / (f.prefill ? f.prefill : 1);
  return (size_t)(shortMs * 1000000ull / perByteNs) + 1024;
}

// ---- the photo-fit refusal ---------------------------------------------------------------
// "photos do not fit the mistral time window (fit 900 of 1500 tokens): over by 41234 B".
// Nothing was sent. The loop reads it back (RoundRecovery::photoFit): older history goes
// first, then the photos are read as text on this host.
inline std::string photoFitError(const std::string& host, uint32_t fit, uint32_t minFinal, size_t overBytes) {
  char buf[160];
  std::snprintf(buf, sizeof buf, "photos do not fit the %s time window (fit %u of %u tokens): over by %u B",
                host.c_str(), (unsigned)fit, (unsigned)minFinal, (unsigned)overBytes);
  return buf;
}
inline bool parsePhotoFitError(const std::string& err, size_t& overBytes) {
  static const char kHead[] = "photos do not fit the ";
  static const char kOver[] = "): over by ";
  if (err.rfind(kHead, 0) != 0) return false;
  const size_t at = err.find(kOver);
  if (at == std::string::npos) return false;
  overBytes = (size_t)std::strtoul(err.c_str() + at + sizeof(kOver) - 1, nullptr, 10);
  return true;
}

// True (with `err` set) when a round that shows photos cannot carry the host's minimum
// final inside its window: the step fails it before the send, as a refused request.
inline bool photosOverFit(const RoundBudget& rb, uint32_t roundMs, const RoundLoad& load, uint32_t photoTokens,
                          std::string& err) {
  if (photoTokens == 0 && load.imageBytes == 0) return false;
  const uint32_t fit = roundFitTokens(rb, roundMs, load);
  const uint32_t want = minFinalTokens(rb);
  if (fit >= want) return false;
  err = photoFitError(rb.host, fit, want, photoFitShedBytes(rb, roundMs, load));
  hlog::logf("wire: %s", err.c_str());
  return true;
}

// ---- one round, sized to its load (lane STACK: out of the round's frame) ------------------
enum class RoundFit : uint8_t {
  Ready,     // the body carries the cap that fits its load, inside its budget
  Rebuild,   // the cap changed shape (its effort, or the field itself): build the body again with `cap`
  Refused,   // not sent: photos over the round's time fit, or the patched body over its budget (`err`)
};
// The body was built with `cap`, the table's. With its load known: a photo round that cannot
// carry the host's minimum final is refused; a capped round takes the load-aware cap
// (`capped` false: OpenAI and the custom endpoint, which never send one - the load is still
// recorded). `textBudget` is the request's own budget less a photo round's share.
struct RoundFitIn {
  const RoundBudget& rb;
  RoundKind kind;
  uint32_t roundMs;
  uint32_t photoTokens;
  size_t textBudget;
  bool capped;
};
__attribute__((noinline)) inline RoundFit fitRoundToLoad(const RoundFitIn& in, std::string& body, BodySplices& splices,
                                                         OutputCap& cap, std::string& err) {
  const RoundLoad load = roundLoad(in.rb, body.size(), splicedBytes(splices), in.photoTokens);
  RoundFit fit = RoundFit::Ready;
  if (!in.capped) recordRound(in.rb, in.kind, in.roundMs, load, OutputCap());
  else if (!fitCapToLoad(body, splices, cap, in.rb, in.kind, in.roundMs, load)) fit = RoundFit::Rebuild;
  lastRound().textBudget = in.textBudget;
  lastRound().photoTokens = in.photoTokens;
  if (photosOverFit(in.rb, in.roundMs, load, in.photoTokens, err)) return RoundFit::Refused;
  if (fit == RoundFit::Ready && !bodyWithinBudget(body, in.textBudget, err)) return RoundFit::Refused;   // a digit wider
  return fit;
}

// ---- the observed ratio (interim: lane B's tighten-only memo replaces it) ---------------
// After a 2xx: the text bytes the request carried and the prompt tokens the provider
// counted for them (the photos' estimate taken off).
struct ObservedRatio {
  std::string host, model;
  size_t textBytes = 0;
  uint32_t promptTokens = 0;
};
inline ObservedRatio& lastObservedRatio() {
  static ObservedRatio o;
  return o;
}
inline void noteObservedRatio(const RoundBudget& rb, size_t textBytes, uint32_t requestTokens, uint32_t photoTokens) {
  if (!requestTokens || !textBytes) return;
  ObservedRatio& o = lastObservedRatio();
  o.host = rb.host;
  o.model = rb.model;
  o.textBytes = textBytes;
  o.promptTokens = requestTokens > photoTokens ? requestTokens - photoTokens : 0;
}

// After a 2xx: the round's usage joins the turn's and its observed ratio is noted.
// noinline (lane STACK): the usage copy stays out of the round's frame.
__attribute__((noinline)) inline void addRoundUsage(ArduinoJson::JsonObjectConst usageJson,
                                                    nimbus::orch::TokenUsage* usage, const RoundBudget& rb,
                                                    size_t textBytes, uint32_t photoTokens) {
  const nimbus::orch::TokenUsage round = nimbus::orch::tokenUsageFromJson(usageJson);
  if (usage) *usage += round;
  noteObservedRatio(rb, textBytes, round.maxRequestIn, photoTokens);
}

}  // namespace wire
}  // namespace providers
}  // namespace agent
