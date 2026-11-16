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

RoundBudget roundBudget(const ProviderDeps& pd, const std::string& host, const std::string& model, bool routed);
// The same, on the heap (lane STACK): a head round's step runs deep in a turn on the
// Telegram poll task, so its budget (two strings and the figures) stays off that stack.
std::unique_ptr<const RoundBudget> newRoundBudget(const ProviderDeps& pd, const std::string& host,
                                                  const std::string& model, bool routed);

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
size_t replayBytesFor(const RoundBudget& rb, size_t v511Bytes);

// ---- the round's load ----------------------------------------------------------------
// What one request carries: its text body, the photo base64 its splices add at send time
// and the photos' share of the window (image_parts.h photoTokens).
uint32_t textTokens(const RoundBudget& rb, size_t textBytes);
RoundLoad roundLoad(const RoundBudget& rb, size_t textBytes, size_t imageBytes, uint32_t photoTokens);
size_t splicedBytes(const BodySplices& splices);

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
RoundRecord& lastRound();
void recordRound(const RoundBudget& rb, RoundKind kind, uint32_t roundMs, const RoundLoad& load,
                 const OutputCap& cap);

// ---- the cap that fits the load --------------------------------------------------------
// The body was built with the table's cap before its load was known. The load-aware cap
// replaces the first attempt's max_tokens in place (the request's first "max_tokens" key:
// the builders write it right after the model, so a tool schema's own field of that name
// never comes first); the photo splices after it move with it. False when the body has no
// such field to change (the caller builds it again).
bool replaceMaxTokens(std::string& body, BodySplices& splices, uint32_t from, uint32_t to);

// The round's cap from its load: `cap` is the table cap the body was built with; on
// return it is the load-aware cap, and true when the body already carries its first
// attempt (patched in place). False: the effort or the field's presence changed, and the
// caller builds the body again with `cap`.
bool fitCapToLoad(std::string& body, BodySplices& splices, OutputCap& cap, const RoundBudget& rb, RoundKind kind,
                  uint32_t roundMs, const RoundLoad& load);

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
FitRates fitRates(const RoundBudget& rb, uint32_t promptTokens);
uint32_t paceMsForBody(size_t bodyBytes);
// The round's overhead before the first output token, in ms.
uint64_t overheadMs(const FitRates& f, const RoundLoad& load);
uint32_t roundFitTokens(const RoundBudget& rb, uint32_t roundMs, const RoundLoad& load);
uint32_t minFinalTokens(const RoundBudget& rb);

// The text bytes a photo round must give up for its fit to carry minFinal again: the
// token shortfall at the decode rate, in ms, over what one text byte costs in upload,
// pacing and prefill.
size_t photoFitShedBytes(const RoundBudget& rb, uint32_t roundMs, const RoundLoad& load);

// ---- the photo-fit refusal ---------------------------------------------------------------
// "photos do not fit the mistral time window (fit 900 of 1500 tokens): over by 41234 B".
// Nothing was sent. The loop reads it back (RoundRecovery::photoFit): older history goes
// first, then the photos are read as text on this host.
std::string photoFitError(const std::string& host, uint32_t fit, uint32_t minFinal, size_t overBytes);
bool parsePhotoFitError(const std::string& err, size_t& overBytes);

// True (with `err` set) when a round that shows photos cannot carry the host's minimum
// final inside its window: the step fails it before the send, as a refused request.
bool photosOverFit(const RoundBudget& rb, uint32_t roundMs, const RoundLoad& load, uint32_t photoTokens,
                   std::string& err);

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
RoundFit fitRoundToLoad(const RoundFitIn& in, std::string& body, BodySplices& splices, OutputCap& cap,
                        std::string& err);

// ---- the observed ratio (interim: lane B's tighten-only memo replaces it) ---------------
// After a 2xx: the text bytes the request carried and the prompt tokens the provider
// counted for them (the photos' estimate taken off).
struct ObservedRatio {
  std::string host, model;
  size_t textBytes = 0;
  uint32_t promptTokens = 0;
};
ObservedRatio& lastObservedRatio();
void noteObservedRatio(const RoundBudget& rb, size_t textBytes, uint32_t requestTokens, uint32_t photoTokens);

// After a 2xx: the round's usage joins the turn's and its observed ratio is noted. Out of
// line (lane STACK): the usage copy stays out of the round's frame.
void addRoundUsage(ArduinoJson::JsonObjectConst usageJson, nimbus::orch::TokenUsage* usage, const RoundBudget& rb,
                   size_t textBytes, uint32_t photoTokens);

// A routed request tells the router when the device gives up on it: the router starts no
// retry or fallback it cannot finish before then, and aborts its upstream call at that
// time (x-cumulo-deadline-ms = the socket deadline less 5 s). Old routers ignore it. Out of
// line (lane STACK): its strings stay out of exchange()'s frame on the poll task.
void addRoutedDeadline(HttpRequest& req, uint32_t timeoutMs);

}  // namespace wire
}  // namespace providers
}  // namespace agent
