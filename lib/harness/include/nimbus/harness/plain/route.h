#pragma once
#include <cstdint>
#include <string>

#include "nimbus/harness/plain/wire.h"
#include "nimbus/orch/model_caps.h"
#include "nimbus/orch/tx/budget.h"

// plain/route.h - the route of one head and the pre-flight of every request (lane E2-R4a; router
// design sections 2 C5, 5 items 1, 3, 9 and 12, 6.3 and 6.8).
//
// resolveRoute answers everything the engine needs to know about a model in one call: the HeadKey the
// shared functions read (tx::budgetFor, outputCapFor, readWindowMsFor), the provider and model rows of
// the facts table (caps::capsFor, narrowed by the boot's refused fields), the dialect, the request
// path, the auth style, the version header, the body cap and the route's real-byte limit. The endpoint
// itself (host, port, tls, basePath, effort, key) stays in HeadEndpoint: Route points at it.
//
// preflight runs inside the exchange on EVERY render, after the exchange chose the output cap, so no
// wrapper can skip it. It applies the row's cap mode (omit, required, the routed ceiling) and a
// caller's cap ceiling, the photo mode (no pixels for a row without image input, no provider file id
// where the route takes none), the structured-output switch, the effort set and the tool-choice set,
// and returns what it left out as a Dropped mask that rides the e item (tx::ExchangeFacts::dropped).
namespace agent {
namespace plain {

// What a pre-flight left out or changed (the e item's "dropped" figure; frozen bit values).
namespace dropped {
inline constexpr uint16_t kEffort = 1, kSchema = 2, kForcedChoice = 4, kPhotos = 8, kCapClamped = 16, kFileRef = 32,
                          kChoiceNone = 64, kReasoning = 128;
}  // namespace dropped

// The routed OpenAI output cap: a billing ceiling the router prices instead of the model's true
// maximum (design section 6.3); a row with a smaller documented maximum keeps its own.
inline constexpr uint32_t kRoutedCeilingTokens = 25000;
// The Cumulo router's real-byte limit for one request, base64 included (design section 7).
inline constexpr uint32_t kRoutedRealBytesMax = 4194304;

struct Route {
  const HeadEndpoint* head = nullptr;    // the endpoint (never copied)
  nimbus::orch::tx::HeadKey key;         // the budget's key: provider slug, the turn's model, routed
  std::string provider;                  // the provider the request reaches: a routed head's upstream
  std::string capHost;                   // the host whose cap and time tables size it (custom Messages: anthropic)
  std::string model;                     // the model the provider is asked for (a routed selector's bare id)
  nimbus::orch::caps::Caps caps;         // provider row and model row (routed overlay and refusals applied)
  uint16_t refused = 0;                  // the fields this head refused this boot (caps::refused::)
  bool hasSlot = false;                  // caps names a table slot (false: a host no slot knows)
  nimbus::orch::caps::Slot slot = nimbus::orch::caps::Slot::Anthropic;
  Dialect dialect = Dialect::AnthropicMessages;
  bool routed = false;
  std::string path;                      // the request path ("/v1/messages", "/router/anthropic/v1/messages")
  nimbus::orch::caps::Auth auth = nimbus::orch::caps::Auth::XApiKey;
  const char* version = "";              // the anthropic-version value ("" = none sent)
  uint32_t bodyCap = 0;                  // tx::budgetFor(key).bodyCap
  uint32_t realBytesMax = 0;             // the real bytes one request may carry (0 = no route limit)
};

// The route of the turn's head. InvalidArgument when nothing names the head (no slug) or the budget
// has no row for it. `refused` (the boot's refused fields; may be null) narrows the model row.
Status resolveRoute(const HeadEndpoint& head, const TurnCtx& turn, Route& out,
                    const nimbus::orch::caps::RefusedFields* refused = nullptr);

// What a pre-flight changes, owned by its caller (the exchange) so RenderInput's pointers stay valid:
// the turn and endpoint copies it may narrow, and the caller's photo function it wraps.
struct PreflightState {
  TurnCtx turn;
  HeadEndpoint head;
  PhotoFn photo;               // the caller's
  uint16_t lateDropped = 0;    // bits the wrapped photo function set while a render ran
};

// Pre-flight one render of `in` (whose maxTokens the exchange chose): fills `own`'s copies from
// in.turn and in.head, points `in` at them, and returns the dropped:: bits. `capCeiling` (0 = none) is
// the caller's ceiling (the summarizer's 4,096 and 8,192). The output cap after it is 0 only on a row
// whose cap mode omits the field; a row that requires one never gets 0 (Anthropic's table cap).
uint16_t preflight(const Route& route, uint32_t capCeiling, PreflightState& own, RenderInput& in);

}  // namespace plain
}  // namespace agent
